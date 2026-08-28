const { workerData } = require('node:worker_threads');

workerData.port.postMessage(process.type + ':' + typeof require('electron'));
Atomics.store(workerData.sab, 0, 1);
Atomics.notify(workerData.sab, 0);
