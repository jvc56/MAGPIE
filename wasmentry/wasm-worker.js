// Shared worker transport for the analysis preview and the diagnostic page.
// Only one command batch owns the engine. Stop cancels the rest of that batch.
let Module;
let api;
let initialized = false;
let running = false;
let stopping = false;
let activeCommand = "";
let pegReadable = false;
let wmpAbort;
let requestId;
let fatal = false;
let memoryLimitMB = 1024;
const cachedFiles = new Set();
const kwgHashes = new Map();
const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const send = (type, fields = {}) => postMessage({ type, requestId, ...fields });

function failEngine(reason) {
  if (fatal) return;
  fatal = true;
  send("fatal", {text: String(reason)});
}
self.addEventListener("error", event => failEngine(event.message || "Engine worker failed."));
function checkEngine() {
  if (fatal) throw new Error("The engine aborted. Reload to restart it.");
}
function browserMemory() {
  for (const maximum of [1024, 512, 256]) {
    try {
      const memory = new WebAssembly.Memory({initial:4096, maximum:maximum * 16, shared:true});
      memoryLimitMB = maximum;
      return memory;
    } catch (error) {
      if (maximum === 256) throw error;
    }
  }
}
function readString(fn) {
  checkEngine();
  const ptr = fn();
  if (!ptr) return "";
  try {
    return Module.UTF8ToString(ptr);
  } finally {
    Module._free(ptr);
  }
}

(async () => {
  try {
    if (typeof SharedArrayBuffer === "undefined") {
      throw new Error(
        "This page needs HTTPS and cross-origin isolation (COOP/COEP headers).",
      );
    }
    const { default: createMagpie } = await import("./magpie_wasm.mjs");
    const { deviceBudget } = await import("./device-budget.mjs");
    const budget = deviceBudget();
    Module = await createMagpie({
      initialThreadPoolSize: budget.threads + 3,
      wasmMemory: browserMemory(),
      onAbort: failEngine,
      print: (text) => send("log", { text }),
      printErr: (text) => send("log", { text }),
      locateFile: (path) => new URL(path, self.location.href).href,
    });
    const wrap = (name, result, args = []) =>
      Module.cwrap(name, result, args);
    api = {
      prepareWMP: wrap("wasm_prepare_wmp", "number", ["number", "number", "number", "number", "number", "number", "number"]),
      witData: wrap("wasm_wit_data", "number"),
      witDataSize: wrap("wasm_wit_data_size", "number"),
      wmpData: wrap("wasm_wmp_data", "number"),
      wmpDataSize: wrap("wasm_wmp_data_size", "number"),
      cancelWMP: wrap("wasm_cancel_wmp", null),
      wmpProgress: wrap("wasm_wmp_progress", "number"),
      finishWMP: wrap("wasm_finish_wmp", "number", ["number"]),
      precache: wrap("precache_file_data", null, [
        "number",
        "number",
        "number",
      ]),
      init: wrap("wasm_magpie_init", "number", ["number"]),
      gameAction: wrap("browser_game_action", "number", [
        "number",
        "number",
        "number",
        "number",
        "number",
        "number",
        "number",
        "number",
        "number",
        "number",
        "number",
      ]),
      importGCG: wrap("browser_game_import_gcg", "number", ["number", "number"]),
      destroy: wrap("wasm_magpie_destroy", null),
      run: wrap("wasm_run_command_async", "number", ["number"]),
      output: wrap("wasm_get_output", "number"),
      error: wrap("wasm_get_error", "number"),
      status: wrap("wasm_get_status", "number"),
      pegOutcomes: wrap("wasm_get_peg_outcomes", "number", ["number"]),
      thread: wrap("wasm_get_thread_status", "number"),
      setSearchPaused: wrap("wasm_set_search_paused", "number", ["number"]),
      stop: wrap("wasm_stop_command", null),
    };
    send("ready", {memoryLimitMB});
  } catch (error) {
    send("init_failed", { text: error.message });
  }
})();

async function run(commands) {
  if (!initialized) throw new Error("MAGPIE is not initialized.");
  if (running) throw new Error("An analysis is already running.");
  running = true;
  stopping = false;
  try {
    // Warm the chosen pool before a timed native search starts.
    const counts = commands.flatMap(command => [...command.matchAll(/-threads\s+(\d+)/g)].map(match => Number(match[1])));
    if (counts.length) await Module.warmThreads(Math.min(32, Math.max(...counts)) + 3);
    for (const command of commands) {
      checkEngine();
      if (stopping) break;
      activeCommand = command;
      if (/^peg(?: |$)/.test(command)) pegReadable = false;
      send("command_started", { command });
      const ptr = Module.stringToNewUTF8(command);
      let result;
      try {
        result = api.run(ptr);
      } finally {
        Module._free(ptr);
      }
      // Parse/data errors happen before a thread starts. Never poll an old
      // thread status (or UNINITIALIZED forever) when a command is rejected.
      if (result !== 0)
        throw new Error(readString(api.error) || `Could not run ${command}`);
      await pause(50);
      let previousStatus = "";
      let polls = 0;
      while (!fatal && api.thread() < 2) {
        if (stopping) api.stop();
        if (++polls % 5 === 0 && api.thread() === 1) {
          const text = readString(api.status);
          if (text?.includes("peginfo ")) pegReadable = true;
          if (text && text !== previousStatus) {
            send("status", { text, command });
            previousStatus = text;
          }
        }
        await pause(100);
      }
      checkEngine();
      // wasm_get_output joins the worker, including after USER_INTERRUPT.
      // Keep the batch locked until that join completes.
      const text = readString(api.output);
      if (text?.includes("peginfo ")) pegReadable = true;
      const error = readString(api.error);
      if (error.trim()) throw new Error(error);
      if (text.trim()) send("output", { text, command });
    }
    send("complete", { stopped: stopping });
  } finally {
    running = false;
    activeCommand = "";
  }
}

onmessage = async ({ data: { type, data = {}, requestId: incomingId } }) => {
  if (fatal) return;
  const control = type === "stop" || type === "search_pause" || type === "peg_outcomes";
  if (control && running && incomingId !== requestId) return;
  if (!running && !control) requestId = incomingId;
  try {
    if (!api) throw new Error("The engine is still loading.");
    if (running && !control)
      throw new Error("Stop the analysis before changing the engine.");
    switch (type) {
      case "precache": {
        if (cachedFiles.has(data.filename)) {
          send("precache_complete", {filename:data.filename});
          break;
        }
        const response = await fetch(data.url);
        if (!response.ok)
          throw new Error(
            `Could not load ${data.filename} (HTTP ${response.status}).`,
          );
        const bytes = new Uint8Array(await response.arrayBuffer());
        if (data.filename.endsWith(".kwg")) {
          const { sha256 } = await import("./wmp-assets.mjs");
          kwgHashes.set(data.filename, await sha256(bytes));
        }
        const name = Module.stringToNewUTF8(data.filename);
        const ptr = Module._malloc(bytes.length);
        try {
          Module.HEAPU8.set(bytes, ptr);
          api.precache(name, ptr, bytes.length);
        } finally {
          Module._free(ptr);
          Module._free(name);
        }
        cachedFiles.add(data.filename);
        send("precache_complete", { filename: data.filename });
        break;
      }
      case "init": {
        const ptr = Module.stringToNewUTF8(data.dataPath);
        let result;
        try {
          result = api.init(ptr);
        } finally {
          Module._free(ptr);
        }
        const error = readString(api.error);
        if (result !== 0 || error.trim())
          throw new Error(error || "Could not initialize MAGPIE.");
        initialized = true;
        send("init_complete");
        break;
      }
      case "game_action": {
        const strings = [
          data.text,
          data.lexicon,
          data.rack || "",
          data.move || "",
          data.note || "",
          data.cgp || "",
        ].map((value) => Module.stringToNewUTF8(value));
        try {
          const game = JSON.parse(
            readString(() =>
              api.gameAction(
                strings[0],
                strings[1],
                data.index,
                strings[2],
                strings[3],
                strings[4],
                data.action,
                strings[5],
                data.seed || 0,
                data.onTurn || 0,
                data.challengeRule ?? 0,
              ),
            ),
          );
          if (game.error) throw new Error(game.error.replace(/\(error \d+\)\s*/g, ""));
          send("game_updated", { game });
        } finally {
          strings.forEach((ptr) => Module._free(ptr));
        }
        break;
      }
      case "import_gcg": {
        const text = Module.stringToNewUTF8(data.text);
        const lexicon = Module.stringToNewUTF8(data.lexicon);
        try {
          const game = JSON.parse(
            readString(() => api.importGCG(text, lexicon)),
          );
          if (game.error) throw new Error(game.error.replace(/\(error \d+\)\s*/g, ""));
          send("gcg_loaded", { game });
        } finally {
          Module._free(text);
          Module._free(lexicon);
        }
        break;
      }
      case "wmp_cache": {
        const { listWMPs, removeWMP } = await import("./wmp-cache.mjs");
        if (data.remove) await removeWMP(data.remove);
        send("wmp_cache_updated", { files: await listWMPs() });
        break;
      }
      case "prepare_wmp":
        await prepareWMP(data);
        break;
      case "run":
        await run(data.commands);
        break;
      case "peg_outcomes": {
        const readable = pegReadable && (!running || /^peg(?: |$)/.test(activeCommand));
        const name = Module.stringToNewUTF8(data.move);
        try {
          const outcomes = readable ? JSON.parse(readString(() => api.pegOutcomes(name)) || "null") : null;
          self.postMessage({type:"peg_outcomes", move:data.move, key:data.key, sessionId:data.sessionId, outcomes});
        } finally {
          Module._free(name);
        }
        break;
      }
      case "search_pause": {
        const accepted = running && !stopping && /^(peg|endgame)(?: |$)/.test(activeCommand)
          && !!api.setSearchPaused(data.paused ? 1 : 0);
        send("search_pause", {accepted, paused:!!data.paused});
        break;
      }
      case "stop":
        if (running) {
          stopping = true;
          wmpAbort?.abort();
          if (activeCommand === "WMP") api.cancelWMP();
          else if (api.thread() === 1) api.stop();
          send("stopping", { command: activeCommand });
        }
        break;
      case "destroy":
        pegReadable = false;
        api.destroy();
        initialized = false;
        cachedFiles.clear();
        kwgHashes.clear();
        send("destroyed");
        break;
      default:
        throw new Error(`Unknown worker message: ${type}`);
    }
  } catch (error) {
    if (fatal || error instanceof WebAssembly.RuntimeError || /Aborted\(/.test(error.message)) failEngine(error.message);
    else send("error", { text: error.message, filename: data.filename, requestId: incomingId });
  }
};

async function prepareWMP({ source, lexicon, threads, manifestURL, cache = false, skipCache = false }) {
  if (!initialized || !["build", "download"].includes(source) ||
      !["CSW24", "NWL23"].includes(lexicon)) throw new Error("Invalid WMP request.");
  if (memoryLimitMB < 1024) {
    send("wmp_cache_warning", { text: "Word maps are unavailable with this browser’s memory limit. Using standard move generation." });
    send("wmp_ready", { unavailable: true, cached: false });
    return;
  }
  running = true;
  stopping = false;
  activeCommand = "WMP";
  let ptr = 0, witPtr = 0, name = 0, nativeStarted = false, nativeFinished = false, retryBuild = false, unavailable = false;
  wmpAbort = new AbortController();
  const progress = (text) => send("wmp_progress", { text });
  const warning = (error) => send("wmp_cache_warning", { text: `WMP cache unavailable: ${error.message}` });
  try {
    const { wmpAsset, downloadWMP, sha256, validateWMP } = await import("./wmp-assets.mjs");
    const { readWMP, saveWMP, removeWMP } = await import("./wmp-cache.mjs");
    const kwgHash = kwgHashes.get(`data/lexica/${lexicon}.kwg`);
    let bytes, wit, cached = false, fromCache = false, witFromCache = false;
    let invalidCache = false;
    try {
      const saved = skipCache ? null : await readWMP(lexicon, kwgHash);
      if (saved) {
        invalidCache = true;
        if (saved.bytes.byteLength !== saved.metadata.bytes ||
            await sha256(saved.bytes) !== saved.metadata.sha256) throw new Error("Stored WMP checksum mismatch.");
        validateWMP(saved.bytes);
        bytes = saved.bytes;
        fromCache = true;
        if (saved.wit && saved.wit.byteLength === saved.metadata.wit_bytes &&
            saved.wit.byteLength <= 256 * 1024 * 1024 &&
            await sha256(saved.wit) === saved.metadata.wit_sha256) {
          wit = saved.wit;
          witFromCache = true;
          cached = true;
        }
        invalidCache = false;
        progress("Loading cached WMP…");
      }
    } catch (error) {
      warning(error);
      if (invalidCache) await removeWMP(lexicon).catch(() => {});
    }
    if (stopping) return send("wmp_ready", { stopped: true });
    if (!bytes && source === "download") {
      progress("Checking hosted WMP…");
      const response = await fetch(manifestURL, { signal: wmpAbort.signal, cache: "no-store" });
      if (!response.ok) throw new Error("No hosted WMP is available for this release. Choose Build on this device.");
      const asset = wmpAsset(await response.json(), lexicon, kwgHash, manifestURL);
      bytes = await downloadWMP(asset, (received, total) => {
        progress(`Downloading WMP · ${(received / 1048576).toFixed(1)} / ${(total / 1048576).toFixed(1)} MB`);
      }, wmpAbort.signal);
    }
    if (stopping) return send("wmp_ready", { stopped: true });
    if (bytes) {
      ptr = Module._malloc(bytes.length);
      if (!ptr) throw Object.assign(new Error("Not enough memory to load WMP."), {code: "WMP_MEMORY"});
      Module.HEAPU8.set(bytes, ptr);
      progress(fromCache ? "Loading cached WMP indexes…" : "Loading WMP indexes…");
    }
    if (wit) {
      witPtr = Module._malloc(wit.length);
      if (!witPtr) throw Object.assign(new Error("Not enough memory to load WIT."), {code: "WMP_MEMORY"});
      Module.HEAPU8.set(wit, witPtr);
    }
    name = Module.stringToNewUTF8(lexicon);
    const count = Math.max(1, Math.min(14, Math.floor(threads) || 1));
    await Module.warmThreads(count + 3);
    checkEngine();
    if (stopping) return send("wmp_ready", { stopped: true });
    if (!bytes) send("wmp_build_started", {lexicon});
    if (api.prepareWMP(name, ptr, bytes?.length || 0, count, cache && (!fromCache || !witFromCache) ? 1 : 0, witPtr, wit?.length || 0) !== 0)
      throw new Error("Could not start WMP preparation.");
    nativeStarted = true;
    let value;
    while (!fatal && !(value = JSON.parse(readString(api.wmpProgress))).done) {
      const { stage, total, completed: complete } = value;
      // Once WIT starts, the native reader no longer borrows the WMP input.
      if (stage >= 6 && ptr) { Module._free(ptr); ptr = 0; }
      if (!cache && stage >= 6) bytes = undefined;
      if (stage === 6) {
        progress(stopping ? "Stopping after WIT preparation…" : "Building WIT indexes…");
      } else if (stage === 7) {
        progress(stopping ? "Stopping after WIT preparation…" : "Loading cached WIT indexes…");
      } else if (stage === 8) {
        progress(stopping ? "Stopping after WIT preparation…" : "Preparing WIT cache…");
      } else if (!bytes) {
        progress(stopping ? "Stopping after WMP preparation…" :
          `Building WMP · ${["reading words", "word entries", "single blanks", "double blanks", "indexes", "saving file"][stage]}${total ? ` · ${complete}/${total} lengths` : ""} · ${count} threads`);
      }
      await pause(100);
    }
    checkEngine();
    Module._free(ptr); ptr = 0;
    Module._free(witPtr); witPtr = 0;
    if (!stopping && cache && !fromCache && !bytes) {
      const start = api.wmpData(), size = api.wmpDataSize();
      if (start && size) bytes = Module.HEAPU8.slice(start, start + size);
    }
    if (!stopping && cache && (!fromCache || !witFromCache)) {
      const start = api.witData(), size = api.witDataSize();
      if (start && size) wit = Module.HEAPU8.slice(start, start + size);
    }
    const result = api.finishWMP(stopping ? 0 : 1);
    nativeFinished = true;
    const preparationError = readString(api.error);
    if (!stopping && result !== 0) {
      if (fromCache && !skipCache) {
        await removeWMP(lexicon).catch(warning);
        retryBuild = true;
      } else throw new Error(preparationError || "Could not install WMP.");
    }
    if (!stopping && !retryBuild && cache && !fromCache && !bytes) {
      warning(new Error("Not enough memory to export WMP for caching."));
    }
    if (!stopping && !retryBuild && cache && (!fromCache || !witFromCache) && bytes) {
      progress("Saving WMP and WIT on this device…");
      try {
        if (!wit) throw new Error("Not enough memory or an export error prevented WIT caching.");
        await saveWMP(lexicon, kwgHash, await sha256(bytes), source, bytes, wit, await sha256(wit));
        cached = true;
      }
      catch (error) { warning(error); }
    }
    if (!retryBuild) send("wmp_ready", { stopped: stopping, fromCache, witFromCache, cached });
  } catch (error) {
    if (stopping && error.name === "AbortError") send("wmp_ready", { stopped: true });
    else if (!fatal && error.code === "WMP_MEMORY") unavailable = true;
    else throw error;
  } finally {
    // Do not release borrowed memory while the native coordinator owns it.
    if (!fatal && nativeStarted && !nativeFinished) {
      while (!JSON.parse(readString(api.wmpProgress)).done) await pause(100);
      api.finishWMP(0);
      readString(api.error);
    }
    Module._free(ptr);
    Module._free(witPtr);
    Module._free(name);
    wmpAbort = null;
    running = false;
    activeCommand = "";
  }
  if (unavailable) {
    send("wmp_cache_warning", {text: "Not enough memory for word maps. Using standard move generation; saved files were kept."});
    send("wmp_ready", {unavailable: true, cached: false});
  }
  if (retryBuild && stopping) return send("wmp_ready", { stopped: true });
  if (retryBuild) await prepareWMP({ source: "build", lexicon, threads, manifestURL, cache, skipCache: true });
}
