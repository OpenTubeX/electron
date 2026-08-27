// Runs in every Node.js worker thread an Electron process creates (via Node's
// embedder preload, before the worker's own script). `process` and `require`
// are the worker's; see OnNodePreload in shell/common/node_bindings.cc.

Object.defineProperty(process, 'type', {
  configurable: false,
  enumerable: true,
  writable: false,
  value: 'worker-thread'
});

// Resolve 'electron' and its aliases to the worker-thread flavor of the module.
require('@electron/internal/common/electron-module');
