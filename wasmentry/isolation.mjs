// Some static hosts ignore _headers. A same-origin service worker can supply
// COOP/COEP on the first reload without moving any computation off the device.
export async function ensureIsolation() {
  const reloadKey = "magpie-isolation-reload";
  const url = new URL(location.href);
  const reloadParam = "magpie-isolation-reload";
  const clearReload = () => {
    try { sessionStorage.removeItem(reloadKey); } catch {}
    if (url.searchParams.has(reloadParam)) {
      url.searchParams.delete(reloadParam);
      history.replaceState(null, "", url);
    }
  };
  if (globalThis.crossOriginIsolated) {
    clearReload();
    return true;
  }
  if (!globalThis.isSecureContext || !("serviceWorker" in navigator)) {
    throw new Error(
      "The engine needs HTTPS (or localhost) and a browser with WebAssembly threads and service workers.",
    );
  }
  if (window.top !== window.self) {
    throw new Error(
      "Open this preview in its own browser tab to enable the engine.",
    );
  }
  let reloaded = url.searchParams.has(reloadParam);
  try { reloaded ||= !!sessionStorage.getItem(reloadKey); } catch {}
  if (reloaded) {
    // Stop automatic retries, but permit a later user-initiated reload.
    clearReload();
    throw new Error(
      "This browser could not enable shared memory. Open the preview in a current Chrome, Firefox or Safari tab.",
    );
  }
  const workerURL = new URL("./isolation-worker.js", import.meta.url);
  const controlled = () =>
    navigator.serviceWorker.controller?.scriptURL === workerURL.href;
  const registration = await navigator.serviceWorker.register(workerURL, {
    scope: "./",
    updateViaCache: "none",
  });
  if (!controlled()) {
    await new Promise((resolve, reject) => {
      const watched = new Set();
      const check = () => {
        // An existing active worker may not control this document (for
        // example after a force reload). It can handle the next navigation;
        // waiting for controllerchange here would never finish.
        const active = registration.active;
        if (
          controlled() ||
          (active?.scriptURL === workerURL.href && active.state === "activated")
        ) {
          cleanup();
          resolve();
          return;
        }
        for (const worker of [
          registration.installing,
          registration.waiting,
          active,
        ]) {
          if (worker && !watched.has(worker)) {
            watched.add(worker);
            worker.addEventListener("statechange", check);
          }
        }
      };
      const timeout = setTimeout(() => {
        cleanup();
        reject(
          new Error(
            "Browser shared-memory setup timed out while activating its service worker. Reload the page to retry.",
          ),
        );
      }, 15000);
      const cleanup = () => {
        clearTimeout(timeout);
        navigator.serviceWorker.removeEventListener("controllerchange", check);
        registration.removeEventListener("updatefound", check);
        for (const worker of watched)
          worker.removeEventListener("statechange", check);
      };
      navigator.serviceWorker.addEventListener("controllerchange", check);
      registration.addEventListener("updatefound", check);
      check();
    });
  }
  // One automatic reload at most: never trap unsupported browsers in a loop.
  try { sessionStorage.setItem(reloadKey, "1"); }
  catch {
    url.searchParams.set(reloadParam, "1");
    history.replaceState(null, "", url);
  }
  location.reload();
  return false;
}
