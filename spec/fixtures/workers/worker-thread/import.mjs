import * as electron from 'electron/worker-thread';

import { parentPort } from 'node:worker_threads';

parentPort.postMessage(Object.keys(electron).includes('protocol'));
