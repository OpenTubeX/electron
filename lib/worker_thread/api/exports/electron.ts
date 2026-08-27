import { defineProperties } from '@electron/internal/common/define-properties';
import { workerThreadModuleList } from '@electron/internal/worker_thread/api/module-list';

module.exports = {};

defineProperties(module.exports, workerThreadModuleList);
