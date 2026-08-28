// Runs the function body passed as workerData.source and posts its result.
const { parentPort, workerData } = require('node:worker_threads');

// eslint-disable-next-line no-new-func
parentPort.postMessage(new Function('require', workerData.source)(require));
