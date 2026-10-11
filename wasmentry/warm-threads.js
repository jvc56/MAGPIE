// Compiled with the module so Closure can safely rename PThread internals.
Module['warmThreads'] = async function(count) {
  var workers = [];
  while (PThread.unusedWorkers.length < count) {
    PThread.allocateUnusedWorker();
    var worker = PThread.unusedWorkers[PThread.unusedWorkers.length - 1];
    workers.push(worker);
  }
  for (var idle of PThread.unusedWorkers) {
    if (!idle.magpieErrorHandler) {
      idle.magpieErrorHandler = true;
      idle.addEventListener('error', function(event) {
        Module['onAbort']?.(event.message || 'An engine thread failed.');
      });
    }
  }
  await Promise.all(workers.map(worker => PThread.loadWasmModuleToWorker(worker)));
};
