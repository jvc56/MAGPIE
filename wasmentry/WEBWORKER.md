# MAGPIE WASM - Web Worker Implementation

This document explains the Web Worker implementation for running MAGPIE in the browser without freezing the UI.

## Overview

The Web Worker implementation runs MAGPIE's computation-intensive operations in a background thread, keeping the browser UI responsive during long-running simulations.

### Architecture

```
Main Thread (UI)                Web Worker (Background)
├─ test-worker.html            ├─ magpie-worker.js
│  ├─ User interface           │  ├─ Loads WASM module
│  ├─ Status display           │  ├─ Handles file precaching
│  └─ Message passing          │  ├─ Runs MAGPIE commands
│                               │  └─ Sends status updates
└─ Stays responsive            └─ Can use multiple pthreads
```

## Quick Start

### 1. Build WASM

```bash
make -f Makefile-wasm clean
make -f Makefile-wasm magpie_wasm
```

This creates:
- `bin/magpie_wasm.mjs` - JavaScript module
- `bin/magpie_wasm.wasm` - WebAssembly binary
- `bin/magpie_wasm.worker.js` - pthread worker support

### 2. Copy Web Worker Files

```bash
cp wasmentry/magpie-worker.js bin/
cp wasmentry/test-worker.html bin/
```

### 3. Start CORS Server

The server must send special headers for SharedArrayBuffer (required for pthreads):

```bash
python wasmentry/cors_server.py
```

Server runs at: http://localhost:8080

### 4. Test in Browser

Open: http://localhost:8080/wasmentry/test-worker.html

## Required Server Headers

For multi-threading (pthreads) to work, your web server **MUST** send these headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Without these headers, SharedArrayBuffer will be disabled and pthreads won't work.

### Testing Headers

In browser console:
```javascript
console.log(crossOriginIsolated); // Should be true
```

If `false`, check your server configuration.

## API Reference

### Web Worker Messages

#### Main Thread → Worker

**Initialize MAGPIE:**
```javascript
worker.postMessage({
  type: 'init',
  data: { dataPath: 'data' }
});
```

**Precache a file:**
```javascript
worker.postMessage({
  type: 'precache',
  data: {
    filename: 'data/lexica/CSW21.kwg',
    url: 'CSW21.kwg'  // Actual fetch URL
  }
});
```

**Run commands:**
```javascript
worker.postMessage({
  type: 'run',
  data: {
    commands: [
      'set -lex CSW21',
      'cgp 15/15/15/15/... EEEIILZ/ 0/0 0',
      'sim -numplays 15 -threads 8'
    ]
  }
});
```

**Stop execution:**
```javascript
worker.postMessage({ type: 'stop' });
```

**Cleanup:**
```javascript
worker.postMessage({ type: 'destroy' });
```

#### Worker → Main Thread

**Ready:**
```javascript
{ type: 'ready' }
```

**Status update:**
```javascript
{ type: 'status', text: 'Iteration 5 complete...' }
```

**Output:**
```javascript
{ type: 'output', text: 'Move list...' }
```

**Complete:**
```javascript
{ type: 'complete' }
```

**Error:**
```javascript
{ type: 'error', text: 'Error message' }
```

## File Structure

```
magpie/
├── wasmentry/
│   ├── api.c                    # WASM entry point (cmd_api wrapper)
│   ├── magpie-worker.js         # Web Worker script
│   ├── test-worker.html         # Test page
│   ├── cors_server.py           # Dev server with COOP/COEP headers
│   └── WEBWORKER.md            # This file
├── bin/
│   ├── magpie_wasm.mjs          # Generated JS module
│   ├── magpie_wasm.wasm         # Generated WASM binary
│   ├── magpie_wasm.worker.js    # Generated pthread worker
│   ├── magpie-worker.js         # Copied from wasmentry/
│   └── test-worker.html         # Copied from wasmentry/
└── Makefile-wasm                # Build configuration
```

## Example Usage

### Basic Simulation

```javascript
const worker = new Worker('magpie-worker.js');

worker.onmessage = (e) => {
  switch (e.data.type) {
    case 'ready':
      // Initialize
      worker.postMessage({
        type: 'init',
        data: { dataPath: 'data' }
      });
      break;

    case 'init_complete':
      // Run simulation
      worker.postMessage({
        type: 'run',
        data: {
          commands: [
            'set -lex NWL20',
            'cgp 15/15/... ABCDEFG/ 0/0 0',
            'generate'
          ]
        }
      });
      break;

    case 'output':
      console.log('Moves:', e.data.text);
      break;

    case 'complete':
      console.log('Done!');
      break;
  }
};
```

### With Status Updates

```javascript
worker.onmessage = (e) => {
  if (e.data.type === 'status') {
    updateProgressBar(e.data.text);  // UI stays responsive!
  }
};
```

## Performance Notes

- **Threads**: Uses `navigator.hardwareConcurrency` for optimal performance
- **Blocking**: Worker thread blocks on `wasm_run_command()`, but main thread stays responsive
- **Memory**: Each command's output is copied from WASM to JS (caller must free)
- **Polling**: Status checked every 500ms during execution

## Troubleshooting

### "SharedArrayBuffer is not defined"

**Cause**: Server not sending COOP/COEP headers
**Fix**: Use the provided `cors_server.py` or configure your server

### "Worker failed to load"

**Cause**: Incorrect worker path
**Fix**: Check that `magpie-worker.js` is in the same directory as the HTML or adjust the path

### "Module is not defined"

**Cause**: WASM module failed to load
**Fix**: Check browser console for network errors, ensure `magpie_wasm.mjs` is accessible

### UI still freezes

**Cause**: Not using the worker implementation
**Fix**: Ensure you're using `test-worker.html` not `wasm_shell.html`

## Production Deployment

1. **Build for production:**
   ```bash
   make -f Makefile-wasm magpie_wasm
   ```

2. **Configure server headers** (Apache example):
   ```apache
   Header set Cross-Origin-Opener-Policy "same-origin"
   Header set Cross-Origin-Embedder-Policy "require-corp"
   ```

3. **Deploy files:**
   - `magpie_wasm.mjs`
   - `magpie_wasm.wasm`
   - `magpie_wasm.worker.js`
   - `magpie-worker.js`
   - Your HTML file
   - Data files (kwg, klv2, csv)

4. **Serve with HTTPS** (required for SharedArrayBuffer in production)

## See Also

- [cmd_api.h](../src/impl/cmd_api.h) - C API used by the worker
- [Emscripten pthreads](https://emscripten.org/docs/porting/pthreads.html)
- [SharedArrayBuffer requirements](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer#security_requirements)
