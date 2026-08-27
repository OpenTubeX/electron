module.exports = require('./webpack.config.base')({
  target: 'worker_thread',
  alwaysHasNode: true
});
