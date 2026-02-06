// Web Worker for MAGPIE WASM
// Runs MAGPIE in a background thread to keep UI responsive

let Module = null;
let precacheFileData = null;
let wasmMagpieInit = null;
let wasmMagpieDestroy = null;
let wasmRunCommand = null;
let wasmRunCommandAsync = null;
let wasmGetOutput = null;
let wasmGetError = null;
let wasmGetStatus = null;
let wasmGetThreadStatus = null;
let wasmStop = null;

let isInitialized = false;
let isRunning = false;
let statusCheckInterval = null;

// Import and initialize WASM module
(async function initWASM() {
  try {
    // Check for SharedArrayBuffer support (required for pthreads)
    if (typeof SharedArrayBuffer === 'undefined') {
      postMessage({
        type: 'error',
        text: 'SharedArrayBuffer is not available. Server must send COOP/COEP headers.'
      });
      return;
    }
    postMessage({ type: 'log', text: '✓ SharedArrayBuffer is available' });

    postMessage({ type: 'log', text: 'Starting WASM import...' });

    // Import the WASM module factory
    const module = await import('./magpie_wasm.mjs');
    const MAGPIE = module.default;

    postMessage({ type: 'log', text: 'WASM module imported, initializing...' });

    // Configure the Module
    const ModuleConfig = {
      print: (text) => {
        postMessage({ type: 'log', text: text });
      },
      printErr: (text) => {
        // Filter out harmless Emscripten warnings
        if (text.includes('program exited') && text.includes('keepRuntimeAlive')) {
          postMessage({ type: 'log', text: 'Runtime initialized (ignoring harmless exit warning)' });
          return;
        }
        // Filter out harmless "still waiting on run dependencies" on slower devices
        if (text.includes('still waiting on run dependencies') || text.includes('wasm-instantiate')) {
          postMessage({ type: 'log', text: 'WASM instantiating... (this is normal on mobile)' });
          return;
        }
        postMessage({ type: 'error', text: text });
      },
      locateFile: (path, prefix) => {
        // Files are in the same directory as the worker
        if (path.endsWith('.wasm') || path.endsWith('.worker.js')) {
          return './' + path;
        }
        return prefix + path;
      },
    };

    // Initialize the WASM module
    postMessage({ type: 'log', text: 'Calling MAGPIE factory...' });
    Module = await MAGPIE(ModuleConfig);

    postMessage({ type: 'log', text: 'WASM initialized, wrapping functions...' });

    // Wrap the exported functions
    precacheFileData = Module.cwrap('precache_file_data', null, [
      'number',
      'number',
      'number',
    ]);
    wasmMagpieInit = Module.cwrap('wasm_magpie_init', 'number', ['number']);
    wasmMagpieDestroy = Module.cwrap('wasm_magpie_destroy', null, []);
    wasmRunCommand = Module.cwrap('wasm_run_command', 'number', ['number']);
    wasmRunCommandAsync = Module.cwrap('wasm_run_command_async', null, ['number']);
    wasmGetOutput = Module.cwrap('wasm_get_output', 'number', []);
    wasmGetError = Module.cwrap('wasm_get_error', 'number', []);
    wasmGetStatus = Module.cwrap('wasm_get_status', 'number', []);
    wasmGetThreadStatus = Module.cwrap('wasm_get_thread_status', 'number', []);
    wasmStop = Module.cwrap('wasm_stop_command', null, []);

    postMessage({ type: 'ready' });
    postMessage({ type: 'log', text: 'Worker ready!' });
  } catch (error) {
    const errorMsg = `❌ WASM INITIALIZATION FAILED\n\nError: ${error.message}\n\nThis is likely a memory issue. On mobile devices, try reducing INITIAL_MEMORY in Makefile-wasm.\n\nFull error:\n${error.stack}`;
    postMessage({ type: 'error', text: errorMsg });
    postMessage({ type: 'init_failed', text: errorMsg });
    console.error('Worker initialization error:', error);
  }
})();

// Handle messages from main thread
onmessage = async (e) => {
  const { type, data } = e.data;

  try {
    switch (type) {
      case 'precache':
        await handlePrecache(data);
        break;

      case 'init':
        handleInit(data);
        break;

      case 'run':
        handleRun(data);
        break;

      case 'stop':
        handleStop();
        break;

      case 'destroy':
        handleDestroy();
        break;

      default:
        postMessage({ type: 'error', text: `Unknown message type: ${type}` });
    }
  } catch (error) {
    postMessage({ type: 'error', text: `Error: ${error.message}` });
  }
};

async function handlePrecache({ filename, url }) {
  if (!Module) {
    postMessage({ type: 'error', text: 'Module not ready' });
    return;
  }

  postMessage({ type: 'log', text: `Precaching ${filename} from ${url}...` });

  const filenamePtr = Module.stringToNewUTF8(filename);

  try {
    postMessage({ type: 'log', text: `Fetching ${url}...` });
    const resp = await fetch(url);

    if (!resp.ok) {
      throw new Error(`HTTP ${resp.status}: ${resp.statusText}`);
    }

    postMessage({ type: 'log', text: `Fetched ${filename}, loading into memory...` });
    const arrBuffer = new Uint8Array(await resp.arrayBuffer());
    const buf = Module._malloc(arrBuffer.length * arrBuffer.BYTES_PER_ELEMENT);
    Module.HEAPU8.set(arrBuffer, buf);

    postMessage({ type: 'log', text: `Calling precache_file_data for ${filename}...` });
    precacheFileData(filenamePtr, buf, arrBuffer.length);

    Module._free(buf);
    postMessage({ type: 'log', text: `✓ Precached ${filename}` });
    postMessage({ type: 'precache_complete', filename: filename });
  } catch (error) {
    const errorMsg = `Failed to precache ${filename} from ${url}: ${error.message}`;
    postMessage({ type: 'error', text: errorMsg });
    postMessage({ type: 'log', text: `❌ ${errorMsg}` });
  } finally {
    Module._free(filenamePtr);
  }
}

function handleInit({ dataPath }) {
  if (!Module) {
    postMessage({ type: 'error', text: 'Module not ready' });
    return;
  }

  const dataPathPtr = Module.stringToNewUTF8(dataPath);
  const result = wasmMagpieInit(dataPathPtr);
  Module._free(dataPathPtr);

  if (result !== 0) {
    postMessage({ type: 'error', text: 'Failed to initialize MAGPIE' });
    return;
  }

  isInitialized = true;
  postMessage({ type: 'init_complete' });
}

async function handleRun({ commands }) {
  if (!Module || !isInitialized) {
    postMessage({ type: 'error', text: 'MAGPIE not initialized' });
    return;
  }

  if (isRunning) {
    postMessage({ type: 'error', text: 'Already running a command' });
    return;
  }

  isRunning = true;

  // Execute commands sequentially
  for (let i = 0; i < commands.length; i++) {
    const cmd = commands[i];
    postMessage({ type: 'log', text: `[${i+1}/${commands.length}] Running command: ${cmd}` });

    const cmdPtr = Module.stringToNewUTF8(cmd);
    postMessage({ type: 'log', text: `Starting async command...` });

    // Start the async command (spawns a pthread)
    wasmRunCommandAsync(cmdPtr);
    Module._free(cmdPtr);

    postMessage({ type: 'log', text: `Command started on pthread, checking status...` });

    // Poll for completion - check thread status
    // 0 = UNINITIALIZED, 1 = STARTED, 2 = USER_INTERRUPT, 3 = FINISHED

    // Wait a moment for pthread to start
    await new Promise(resolve => setTimeout(resolve, 50));

    while (true) {
      let threadStatus = wasmGetThreadStatus();

      // Validate thread status (should be 0-3)
      if (threadStatus < 0 || threadStatus > 3) {
        postMessage({ type: 'log', text: `Invalid thread status: ${threadStatus}, waiting...` });
        await new Promise(resolve => setTimeout(resolve, 50));
        continue;
      }

      postMessage({ type: 'log', text: `Thread status: ${threadStatus}` });

      // If still UNINITIALIZED, the pthread hasn't started yet - keep waiting
      if (threadStatus === 0) {
        await new Promise(resolve => setTimeout(resolve, 50));
        continue;
      }

      // Check if thread finished (3 = THREAD_CONTROL_STATUS_FINISHED)
      if (threadStatus === 3) {
        postMessage({ type: 'log', text: `Command completed (thread status = FINISHED)` });
        break;
      }

      // Check for user interrupt (2 = THREAD_CONTROL_STATUS_USER_INTERRUPT)
      if (threadStatus === 2) {
        postMessage({ type: 'log', text: `Command interrupted by user` });
        break;
      }

      // Wait before next poll
      await new Promise(resolve => setTimeout(resolve, 100));
    }

    // Get output after command completes
    const outputPtr = wasmGetOutput();
    if (outputPtr) {
      const output = Module.UTF8ToString(outputPtr);
      Module._free(outputPtr);
      if (output && output.trim()) {
        postMessage({ type: 'output', text: output });
      }
    }

    // Check for errors
    const errorPtr = wasmGetError();
    if (errorPtr) {
      const error = Module.UTF8ToString(errorPtr);
      Module._free(errorPtr);
      if (error && error.trim()) {
        postMessage({ type: 'error', text: `Command "${cmd}" failed: ${error}` });
        isRunning = false;
        return;
      }
    }
  }

  // Commands completed successfully
  postMessage({ type: 'log', text: 'All commands completed' });
  isRunning = false;
  postMessage({ type: 'complete' });
}

function startStatusPolling() {
  // Note: This is not currently used since sim command is synchronous
  // and blocks until complete. If we need async status updates in the future,
  // we'll need to restructure to run commands in a separate thread.
  let lastStatus = '';

  statusCheckInterval = setInterval(() => {
    if (!isRunning) {
      clearInterval(statusCheckInterval);
      return;
    }

    const statusPtr = wasmGetStatus();
    if (statusPtr) {
      const status = Module.UTF8ToString(statusPtr);
      Module._free(statusPtr);

      // Only send if status changed
      if (status !== lastStatus) {
        lastStatus = status;
        postMessage({ type: 'status', text: status });

        // Check if command is complete
        const lowerStatus = status.toLowerCase();
        if (
          lowerStatus.includes('complete') ||
          lowerStatus.includes('finished') ||
          lowerStatus.includes('done')
        ) {
          finishCommand();
        }
      }
    }
  }, 500); // Poll every 500ms
}

function finishCommand() {
  clearInterval(statusCheckInterval);
  statusCheckInterval = null;

  // Get final output
  const outputPtr = wasmGetOutput();
  if (outputPtr) {
    const output = Module.UTF8ToString(outputPtr);
    Module._free(outputPtr);
    postMessage({ type: 'output', text: output });
  }

  isRunning = false;
  postMessage({ type: 'complete' });
}

function handleStop() {
  if (!isRunning) {
    return;
  }

  wasmStop();
  postMessage({ type: 'stopped' });

  // The status polling will detect the stop and finish
}

function handleDestroy() {
  if (statusCheckInterval) {
    clearInterval(statusCheckInterval);
  }

  if (isInitialized) {
    wasmMagpieDestroy();
    isInitialized = false;
  }

  postMessage({ type: 'destroyed' });
}
