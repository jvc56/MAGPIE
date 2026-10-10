export class EngineClient extends EventTarget {
  constructor() {
    super();
    this.worker = new Worker(new URL("./wasm-worker.js", import.meta.url));
    this.pending = null;
    this.nextRequestId = 0;
    this.fatalError = null;
    this.lastLog = "";
    this.loadedFiles = new Set();
    this.initialized = false;
    this.ready = new Promise((resolve, reject) => {
      this.resolveReady = resolve;
      this.rejectReady = reject;
    });
    // A rejection can precede the first analysis button click.
    this.ready.catch(() => {});
    this.worker.onmessage = ({ data }) => {
      if (data.type === "fatal" || data.type === "init_failed") {
        this.fail(new Error(`The engine stopped. Reload to try again. ${data.text}`));
        return;
      }
      if (data.requestId != null && data.requestId !== this.pending?.id) return;
      if (data.type === "log") this.lastLog = data.text;
      this.dispatchEvent(new CustomEvent("message", { detail: data }));
      if (data.type === "ready") this.resolveReady();
      if (data.type === "error" || data.type === "init_failed") {
        const error = new Error(data.text);
        this.rejectReady(error);
        const pending = this.pending;
        this.pending = null;
        clearTimeout(pending?.watchdog);
        pending?.reject(error);
      } else if (data.type === this.pending?.response) {
        const pending = this.pending;
        this.pending = null;
        clearTimeout(pending.watchdog);
        if (data.type === "destroyed") this.resetState();
        pending.resolve(data);
      }
    };
    this.worker.onerror = (event) => {
      const error = new Error(
        `The engine worker stopped. Reload to try again. ${this.lastLog || event.message || ""}`,
      );
      this.fail(error);
    };
  }
  resetState() {
    this.loadedFiles.clear();
    this.initialized = false;
    this.wmpKey = null;
    this.wmpCached = false;
    this.wmpUnavailable = false;
  }
  fail(error) {
    if (this.fatalError) return;
    this.fatalError = error;
    this.worker.terminate();
    clearTimeout(this.pending?.watchdog);
    this.rejectReady(error);
    this.pending?.reject(error);
    this.pending = null;
    this.resetState();
    this.dispatchEvent(new CustomEvent("message", {detail: {type:"fatal", text:error.message}}));
  }
  request(type, data, response) {
    if (this.fatalError) return Promise.reject(this.fatalError);
    if (this.pending) return Promise.reject(new Error("The engine is busy."));
    return new Promise((resolve, reject) => {
      const id = ++this.nextRequestId;
      const limit = ["import_gcg", "game_action"].includes(type) ? 30000 : 0;
      const watchdog = limit ? setTimeout(() => {
        this.fail(new Error("Game loading or editing timed out. Reload to restart the engine; the current record was not changed."));
      }, limit) : null;
      this.pending = { id, response, resolve, reject, watchdog };
      this.worker.postMessage({ type, data, requestId:id });
    });
  }
  async prepare(lexicon) {
    if (!["CSW24", "NWL23"].includes(lexicon)) {
      throw new Error("Choose a supported lexicon before preparing the engine.");
    }
    if (this.fatalError) throw this.fatalError;
    await this.ready;
    const files = [
      `lexica/${lexicon}.kwg`,
      `lexica/${lexicon}.klv2`,
      "letterdistributions/english.csv",
      "strategy/winpct_english.csv",
      "layouts/standard15.txt",
    ];
    for (const file of files) {
      if (this.loadedFiles.has(file)) continue;
      await this.request(
        "precache",
        {
          filename: `data/${file}`,
          url: new URL(`../data/${file}`, import.meta.url).href,
        },
        "precache_complete",
      );
      this.loadedFiles.add(file);
    }
    if (!this.initialized) {
      await this.request("init", { dataPath: "data" }, "init_complete");
      this.initialized = true;
    }
  }
  async prepareWMP(lexicon, source, threads, cache = false) {
    const key = `${lexicon}:${source}`;
    if (this.wmpKey === key && (!cache || this.wmpCached || this.wmpUnavailable || source === "off")) return;
    this.wmpKey = null;
    // Disable the old map before changing lexica so Config never tries to
    // discover an optional WMP on disk. Installing transfers the new map.
    await this.run([`set -wit false -wmp false -lex ${lexicon}`]);
    if (source !== "off") {
      const result = await this.request("prepare_wmp", {
        lexicon, source, threads, cache,
        manifestURL: new URL("../data/wmp-manifest.json", import.meta.url).href,
      }, "wmp_ready");
      if (result.stopped) return;
      this.wmpCached = result.cached;
      this.wmpUnavailable = !!result.unavailable;
    }
    this.wmpKey = key;
  }
  async wmpCache(remove) {
    if (remove && this.wmpKey?.startsWith(`${remove}:`)) {
      await this.run(["set -wit false -wmp false"]);
      this.wmpKey = null;
    }
    return this.request("wmp_cache", { remove }, "wmp_cache_updated");
  }
  gameAction(data) {
    return this.request("game_action", data, "game_updated");
  }
  importGCG(text, lexicon) {
    return this.request("import_gcg", { text, lexicon }, "gcg_loaded");
  }
  run(commands) {
    if (this.wmpUnavailable) commands = commands.map(command => command.replace(/-(wmp|wit) true\b/g, "-$1 false"));
    return this.request("run", { commands }, "complete");
  }
  destroy() {
    return this.request("destroy", {}, "destroyed");
  }
  stop() {
    if (this.fatalError) return;
    this.worker.postMessage({ type: "stop", requestId:this.pending?.id });
  }
}
