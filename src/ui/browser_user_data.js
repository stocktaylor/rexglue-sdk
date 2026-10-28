// Browser builds: the app's user data ($HOME, /home/web_user: saves, settings,
// the shader cache) persists in IndexedDB through Emscripten's IDBFS. It's
// restored before the program starts and written back every few seconds and
// when the page is hidden. The file system lives on the main thread (game
// threads' file calls are proxied to it), and so does this.
if (typeof WorkerGlobalScope === 'undefined' && typeof indexedDB !== 'undefined') {
  let rexSyncing = false;
  let rexSyncAgain = false;
  const rexSaveUserData = () => {
    if (rexSyncing) {
      rexSyncAgain = true;
      return;
    }
    rexSyncing = true;
    FS.syncfs(false, (error) => {
      rexSyncing = false;
      if (error) {
        err(`user data: saving to IndexedDB failed: ${error}`);
      }
      if (rexSyncAgain) {
        rexSyncAgain = false;
        rexSaveUserData();
      }
    });
  };
  Module['preRun'] = [].concat(Module['preRun'] || [], () => {
    FS.mount(IDBFS, {}, '/home/web_user');
    addRunDependency('rex-user-data');
    FS.syncfs(true, (error) => {
      if (error) {
        err(`user data: loading from IndexedDB failed: ${error}`);
      }
      removeRunDependency('rex-user-data');
      setInterval(rexSaveUserData, 5000);
      document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden') {
          rexSaveUserData();
        }
      });
      addEventListener('pagehide', rexSaveUserData);
    });
  });
}
