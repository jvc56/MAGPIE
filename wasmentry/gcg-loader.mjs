export function readGCG(file, pastedText, defaultLexicon, progress = () => {}) {
  return new Promise((resolve, reject) => {
    progress("loader.create");
    const worker = new Worker(new URL("./gcg-loader-worker.js?v=2", import.meta.url));
    let settled = false;
    const finish = (error, result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      worker.terminate();
      progress(error ? "loader.failed" : "loader.complete", error ? { message: error.message } : {});
      if (error) {
        reject(error);
      } else {
        resolve(result);
      }
    };
    // Keep the watchdog on the page: it must still run if the loader is stuck.
    const timer = setTimeout(() => finish(new Error(
      file
        ? "File read timed out. Paste the GCG text, or try a file stored locally."
        : "GCG preparation timed out. Try loading the game again.",
    )), 10000);
    worker.onmessage = ({ data }) => {
      if (data?.type === "progress") {
        progress(data.stage);
        return;
      }
      if (data?.error) {
        finish(new Error(data.error));
      } else if (typeof data?.text !== "string" || !["CSW24", "NWL23"].includes(data.lexicon)) {
        finish(new Error("The GCG loader returned an incomplete result. Reload Magpie and try again."));
      } else {
        finish(null, data);
      }
    };
    worker.onerror = (event) => {
      event.preventDefault();
      finish(new Error("The GCG loading worker stopped. Try loading the game again."));
    };
    worker.onmessageerror = () => {
      finish(new Error("The GCG loading worker returned an unreadable result."));
    };
    try {
      worker.postMessage({ file, pastedText, defaultLexicon, reportProgress: true });
      progress("loader.dispatched");
    } catch (failure) {
      finish(failure);
    }
  });
}
