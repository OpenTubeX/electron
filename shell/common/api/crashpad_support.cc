// Copyright (c) 2022 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "build/build_config.h"
#include "shell/common/node_includes.h"

#if BUILDFLAG(IS_LINUX)
#include "components/crash/core/app/crashpad.h"  // nogncheck
#endif

namespace {

#if BUILDFLAG(IS_LINUX)
void GetCrashdumpSignalFD(const v8::FunctionCallbackInfo<v8::Value>& args) {
  int fd;
  args.GetReturnValue().Set(
      crash_reporter::GetHandlerSocket(&fd, nullptr) ? fd : -1);
}

void GetCrashpadHandlerPID(const v8::FunctionCallbackInfo<v8::Value>& args) {
  int pid;
  args.GetReturnValue().Set(
      crash_reporter::GetHandlerSocket(nullptr, &pid) ? pid : -1);
}
#endif

// Loaded by child_process in every Node.js environment, worker threads
// included, so plain V8 callbacks rather than gin_helper templates whose
// holders outlive a worker's environment.
void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
#if BUILDFLAG(IS_LINUX)
  NODE_SET_METHOD(exports, "getCrashdumpSignalFD", GetCrashdumpSignalFD);
  NODE_SET_METHOD(exports, "getCrashpadHandlerPID", GetCrashpadHandlerPID);
#endif
}

}  // namespace

NODE_LINKED_BINDING_CONTEXT_AWARE(electron_common_crashpad_support, Initialize)
