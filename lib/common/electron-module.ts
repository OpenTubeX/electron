// Puts 'electron' (and its per-process aliases) in the module cache and makes
// them resolve, so `require('electron')` returns this bundle's module.

const Module = require('module') as NodeJS.ModuleInternal;

// Make a fake Electron module that we will insert into the module cache
const makeElectronModule = (name: string) => {
  const electronModule = new Module('electron', null);
  electronModule.id = 'electron';
  electronModule.loaded = true;
  electronModule.filename = name;
  Object.defineProperty(electronModule, 'exports', {
    get: () => require('electron')
  });
  Module._cache[name] = electronModule;
};

makeElectronModule('electron');
makeElectronModule('electron/common');
if (process.type === 'browser') {
  makeElectronModule('electron/main');
} else if (process.type === 'renderer') {
  makeElectronModule('electron/renderer');
} else if (process.type === 'utility') {
  makeElectronModule('electron/utility');
} else if (process.type === 'worker-thread') {
  makeElectronModule('electron/worker-thread');
}

const originalResolveFilename = Module._resolveFilename;

// 'electron/{common,main,renderer,utility}' are module aliases
// of the 'electron' module for TypeScript purposes, i.e., the types for
// 'electron/main' consist of only main process modules, etc. It is intentional
// that these can be `require()`-ed from both the main process as well as the
// renderer process regardless of the names, they're superficial for TypeScript
// only.
const electronModuleNames = new Set([
  'electron',
  'electron/main',
  'electron/renderer',
  'electron/common',
  'electron/utility',
  'electron/worker-thread'
]);
Module._resolveFilename = function (request, parent, isMain, options) {
  if (electronModuleNames.has(request)) {
    return 'electron';
  } else {
    return originalResolveFilename(request, parent, isMain, options);
  }
};

export {};
