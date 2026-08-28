// Hosts a protocol handler in a worker thread for api-protocol-spec: runs the
// async function body in workerData.source with `protocol` in scope, reports
// readiness, and exits cleanly when told to.
const { parentPort, workerData } = require('node:worker_threads');

const { protocol } = require('electron');

// eslint-disable-next-line no-new-func
const setup = new Function('protocol', 'parentPort', `return (async () => { ${workerData.source} })();`);
setup(protocol, parentPort).then(
  (value) => parentPort.postMessage({ ready: value }),
  (error) => parentPort.postMessage({ error: String(error) })
);

parentPort.on('message', (message) => {
  if (message === 'stop') {
    if (protocol.isProtocolHandled(workerData.scheme)) protocol.unhandle(workerData.scheme);
    parentPort.postMessage('stopped');
  } else if (message === 'exit') {
    process.exit(0);
  }
});
