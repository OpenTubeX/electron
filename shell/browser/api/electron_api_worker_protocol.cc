// Copyright (c) 2026 Anthropic, PBC.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "gin/converter.h"
#include "gin/dictionary.h"
#include "net/base/net_errors.h"
#include "shell/browser/browser.h"
#include "shell/browser/electron_browser_context.h"
#include "shell/browser/net/worker_protocol.h"
#include "shell/browser/protocol_registry.h"
#include "shell/common/gin_converters/std_converter.h"
#include "shell/common/node_includes.h"

// protocol.handle() for Node.js worker threads: lib/worker_thread/api/protocol
// drives this binding; requests arrive from the IO thread through a
// WorkerProtocolEndpoint and never touch the browser's main thread.

namespace {

using electron::WorkerProtocolEndpoint;

// Per worker environment. Owns the JS callbacks and the endpoint; torn down by
// the environment's cleanup hook.
class WorkerProtocolBinding : public WorkerProtocolEndpoint::Delegate {
 public:
  WorkerProtocolBinding(v8::Isolate* isolate, v8::Local<v8::Context> context)
      : isolate_(isolate), context_(isolate, context) {
    endpoint_ = base::MakeRefCounted<WorkerProtocolEndpoint>(
        node::GetCurrentEventLoop(isolate), this);
    node::AddEnvironmentCleanupHook(isolate, &WorkerProtocolBinding::Cleanup,
                                    this);
  }

  static void Cleanup(void* arg) {
    auto* self = static_cast<WorkerProtocolBinding*>(arg);
    for (const auto& [scheme, partition] : self->schemes_)
      self->PostUnregister(partition, scheme);
    // Registrations still in flight land before this on the UI thread.
    for (const auto& [token, pending] : self->pending_)
      self->PostUnregister(pending.second, pending.first);
    self->endpoint_->Close();
    delete self;
  }

  static WorkerProtocolBinding* From(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    return static_cast<WorkerProtocolBinding*>(
        info.Data().As<v8::External>()->Value(
            v8::kExternalPointerTypeTagDefault));
  }

  // JS: setCallbacks(onRequest, onCancel, onWritten)
  static void SetCallbacks(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* self = From(info);
    if (!info[0]->IsFunction() || !info[1]->IsFunction() ||
        !info[2]->IsFunction()) {
      return;
    }
    self->on_request_.Reset(self->isolate_, info[0].As<v8::Function>());
    self->on_cancel_.Reset(self->isolate_, info[1].As<v8::Function>());
    self->on_written_.Reset(self->isolate_, info[2].As<v8::Function>());
  }

  // JS: handle(partition, scheme) -> Promise<boolean> resolved on this thread
  // once the UI thread has (or has not) registered the scheme.
  static void Handle(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* self = From(info);
    std::string partition;
    std::string scheme;
    if (!gin::ConvertFromV8(self->isolate_, info[0], &partition) ||
        !gin::ConvertFromV8(self->isolate_, info[1], &scheme)) {
      return;
    }
    info.GetReturnValue().Set(self->Register(partition, scheme));
  }

  static void Unhandle(const v8::FunctionCallbackInfo<v8::Value>& info) {
    std::string scheme;
    if (gin::ConvertFromV8(info.GetIsolate(), info[0], &scheme))
      From(info)->Unregister(scheme);
  }

  // JS: respond(id, status, statusText, [[name, value], ...], hasBody)
  static void Respond(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* isolate = info.GetIsolate();
    double id;
    int status;
    std::string status_text;
    std::vector<std::vector<std::string>> headers;
    bool has_body;
    if (!gin::ConvertFromV8(isolate, info[0], &id) ||
        !gin::ConvertFromV8(isolate, info[1], &status) ||
        !gin::ConvertFromV8(isolate, info[2], &status_text) ||
        !gin::ConvertFromV8(isolate, info[3], &headers) ||
        !gin::ConvertFromV8(isolate, info[4], &has_body)) {
      return;
    }
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(headers.size());
    for (const auto& header : headers) {
      if (header.size() == 2)
        pairs.emplace_back(header[0], header[1]);
    }
    From(info)->endpoint_->Respond(static_cast<uint64_t>(id), status,
                                   status_text, std::move(pairs), has_body);
  }

  // JS: write(id, Uint8Array)
  static void Write(const v8::FunctionCallbackInfo<v8::Value>& info) {
    double id;
    if (!gin::ConvertFromV8(info.GetIsolate(), info[0], &id) ||
        !info[1]->IsArrayBufferView()) {
      return;
    }
    auto view = info[1].As<v8::ArrayBufferView>();
    std::string bytes(view->ByteLength(), '\0');
    view->CopyContents(bytes.data(), bytes.size());
    From(info)->endpoint_->Write(static_cast<uint64_t>(id), std::move(bytes));
  }

  // JS: finish(id, netError)
  static void Finish(const v8::FunctionCallbackInfo<v8::Value>& info) {
    double id;
    int net_error;
    if (gin::ConvertFromV8(info.GetIsolate(), info[0], &id) &&
        gin::ConvertFromV8(info.GetIsolate(), info[1], &net_error)) {
      From(info)->endpoint_->Finish(static_cast<uint64_t>(id), net_error);
    }
  }

 private:
  ~WorkerProtocolBinding() override = default;

  v8::Local<v8::Value> Register(const std::string& partition,
                                const std::string& scheme) {
    v8::Local<v8::Context> context = context_.Get(isolate_);
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver))
      return v8::Undefined(isolate_);
    const uint64_t token = ++next_token_;
    resolvers_[token].Reset(isolate_, resolver);
    pending_[token] = {scheme, partition};
    UpdateKeepAlive();
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](scoped_refptr<WorkerProtocolEndpoint> endpoint,
               std::string partition, std::string scheme, uint64_t token,
               WorkerProtocolBinding* self) {
              std::string error;
              if (!electron::Browser::Get()->is_ready()) {
                error = "protocol.handle() cannot be used before app is ready";
              } else if (electron::ProtocolRegistry::IsBuiltinScheme(scheme)) {
                error = "Built-in scheme " + scheme +
                        " cannot be handled from a worker thread";
              } else {
                // Only the default session for now; `partition` is plumbed
                // for a session option later.
                auto* registry =
                    electron::ElectronBrowserContext::From(partition, false)
                        ->protocol_registry();
                if (!registry->RegisterWorkerProtocol(scheme, endpoint))
                  error = "Scheme " + scheme + " is already handled";
              }
              endpoint->PostToWorker(base::BindOnce(
                  &WorkerProtocolBinding::OnHandled, base::Unretained(self),
                  token, scheme, partition, error));
            },
            endpoint_, partition, scheme, token, this));
    return resolver->GetPromise();
  }

  void Unregister(const std::string& scheme) {
    if (auto it = schemes_.find(scheme); it != schemes_.end()) {
      PostUnregister(it->second, scheme);
      schemes_.erase(it);
    } else {
      // Still registering: OnHandled() releases it when the reply arrives.
      std::erase_if(pending_, [&](const auto& entry) {
        return entry.second.first == scheme;
      });
    }
    UpdateKeepAlive();
  }

  void PostUnregister(const std::string& partition, const std::string& scheme) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(
                       [](scoped_refptr<WorkerProtocolEndpoint> endpoint,
                          std::string partition, std::string scheme) {
                         if (!electron::Browser::Get()->is_ready())
                           return;
                         electron::ElectronBrowserContext::From(partition,
                                                                false)
                             ->protocol_registry()
                             ->UnregisterWorkerProtocol(scheme, endpoint.get());
                       },
                       endpoint_, partition, scheme));
  }

  // `error` is empty when the scheme was registered.
  void OnHandled(uint64_t token,
                 std::string scheme,
                 std::string partition,
                 std::string error) {
    if (isolate_->IsExecutionTerminating())
      return;
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> context = context_.Get(isolate_);
    v8::Context::Scope context_scope(context);
    const bool wanted = pending_.erase(token) > 0;
    if (error.empty()) {
      if (wanted) {
        schemes_[scheme] = partition;
      } else {
        PostUnregister(partition, scheme);
        error = "Scheme " + scheme + " was unhandled before it was registered";
      }
    }
    auto it = resolvers_.find(token);
    if (it == resolvers_.end())
      return;
    v8::Local<v8::Promise::Resolver> resolver = it->second.Get(isolate_);
    resolvers_.erase(it);
    UpdateKeepAlive();
    std::ignore = resolver->Resolve(context, gin::StringToV8(isolate_, error));
  }

  void UpdateKeepAlive() {
    endpoint_->SetKeepAlive(!schemes_.empty() || !resolvers_.empty());
  }

  // WorkerProtocolEndpoint::Delegate:
  void OnWorkerProtocolEvents(
      std::vector<WorkerProtocolEndpoint::Event> events) override {
    v8::HandleScope handle_scope(isolate_);
    v8::Local<v8::Context> context = context_.Get(isolate_);
    v8::Context::Scope context_scope(context);
    node::Environment* env = node::GetCurrentEnvironment(context);
    for (auto& event : events) {
      using Kind = WorkerProtocolEndpoint::Event::Kind;
      v8::Local<v8::Function> fn;
      std::vector<v8::Local<v8::Value>> args;
      args.push_back(v8::Number::New(isolate_, static_cast<double>(event.id)));
      if (event.kind == Kind::kRequest) {
        fn = on_request_.Get(isolate_);
        const auto& r = *event.request;
        gin::Dictionary dict = gin::Dictionary::CreateEmpty(isolate_);
        dict.Set("scheme", r.scheme);
        dict.Set("url", r.url.spec());
        dict.Set("method", r.method);
        dict.Set("referrer", r.referrer);
        std::vector<std::vector<std::string>> headers;
        headers.reserve(r.headers.size());
        for (const auto& [name, value] : r.headers)
          headers.push_back({name, value});
        dict.Set("headers", headers);
        v8::Local<v8::Object> body;
        if (r.body &&
            node::Buffer::Copy(isolate_, r.body->data(), r.body->size())
                .ToLocal(&body)) {
          dict.Set("body", body);
        }
        args.push_back(gin::ConvertToV8(isolate_, dict));
      } else if (event.kind == Kind::kCancel) {
        fn = on_cancel_.Get(isolate_);
      } else if (event.kind == Kind::kCallback) {
        std::move(event.callback).Run();
        continue;
      } else {
        fn = on_written_.Get(isolate_);
        args.push_back(v8::Number::New(
            isolate_, static_cast<double>(event.bytes_written)));
      }
      if (fn.IsEmpty())
        continue;
      node::CallbackScope callback_scope(env, v8::Object::New(isolate_),
                                         {0, 0});
      std::ignore =
          fn->Call(context, v8::Undefined(isolate_), args.size(), args.data());
    }
  }

  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  scoped_refptr<WorkerProtocolEndpoint> endpoint_;
  v8::Global<v8::Function> on_request_, on_cancel_, on_written_;
  std::map<std::string, std::string> schemes_;  // scheme -> partition
  uint64_t next_token_ = 0;
  std::map<uint64_t, v8::Global<v8::Promise::Resolver>> resolvers_;
  // token -> (scheme, partition) of registrations awaiting the UI thread.
  std::map<uint64_t, std::pair<std::string, std::string>> pending_;
};

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  v8::Isolate* const isolate = v8::Isolate::GetCurrent();
  // Leaked into the environment's cleanup hook.
  auto* binding = new WorkerProtocolBinding(isolate, context);
  // Nothing installed here may rely on the main isolate's teardown to be
  // freed; a worker isolate never runs it.
  v8::Local<v8::External> self =
      v8::External::New(isolate, binding, v8::kExternalPointerTypeTagDefault);
  auto set = [&](const char* name, v8::FunctionCallback callback) {
    exports
        ->Set(context, gin::StringToSymbol(isolate, name),
              v8::Function::New(context, callback, self).ToLocalChecked())
        .Check();
  };
  set("setCallbacks", &WorkerProtocolBinding::SetCallbacks);
  set("handle", &WorkerProtocolBinding::Handle);
  set("unhandle", &WorkerProtocolBinding::Unhandle);
  set("respond", &WorkerProtocolBinding::Respond);
  set("write", &WorkerProtocolBinding::Write);
  set("finish", &WorkerProtocolBinding::Finish);
}

}  // namespace

NODE_LINKED_BINDING_CONTEXT_AWARE(electron_worker_protocol, Initialize)
