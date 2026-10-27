// Game data from a folder the user picked in the page (BrowserFileDevice).
//
// The page calls Module.rexSetGameFiles([{path, file}, ...]) with paths
// relative to the game folder ("default.xex", "data/x.bin") and the picked
// File objects, which stay references to the user's disk. A page that sets
// Module.rexWaitForGameFiles holds the program until then.
//
// The game reads files synchronously on its threads (FileReaderSync, workers
// only), and a running thread never returns to its event loop, so the list is
// sent to every worker when it's set and to each worker created afterwards,
// before any game thread can start on it. Emscripten's own worker messages all
// carry a cmd, which this one doesn't, so its handler ignores it.
if (typeof WorkerGlobalScope !== 'undefined') {
  self.addEventListener('message', (e) => {
    if (e.data && e.data.rexGameFiles) {
      globalThis.rexGameFiles = e.data.rexGameFiles;
    }
  });
} else if (typeof Worker !== 'undefined') {
  const rexWorkers = [];
  let rexFiles = null;
  let rexWaiting = false;
  const RealWorker = Worker;
  globalThis.Worker = class extends RealWorker {
    constructor(...args) {
      super(...args);
      rexWorkers.push(new WeakRef(this));
      if (rexFiles) {
        this.postMessage({rexGameFiles: rexFiles});
      }
    }
  };
  Module['rexSetGameFiles'] = (files) => {
    rexFiles = files;
    for (const ref of rexWorkers) {
      ref.deref()?.postMessage({rexGameFiles: files});
    }
    if (rexWaiting) {
      rexWaiting = false;
      removeRunDependency('rex-game-files');
    }
  };
  if (Module['rexWaitForGameFiles']) {
    Module['preRun'] = [].concat(Module['preRun'] || [], () => {
      if (!rexFiles) {
        rexWaiting = true;
        addRunDependency('rex-game-files');
      }
    });
  }
}
