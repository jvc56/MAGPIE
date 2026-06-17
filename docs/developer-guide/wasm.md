# WebAssembly Build

> **Status:** outline — prose to be written.
> **Sources:** `Makefile-wasm`, `wasmentry/`
> **Upstream notes:** `wasmentry/WEBWORKER.md`, `wasmentry/PACKAGING.md`

## Building with Emscripten

<!-- NOTE: the Makefile-wasm flow; pthreads enabled for parallel move
     generation/sim/endgame. Outputs: magpie_wasm.mjs + .wasm, wasm-worker.js,
     and the data files served as static assets. -->

## The worker model

<!-- NOTE: summarize wasmentry/WEBWORKER.md — threading via web workers, the
     C↔JS interface in wasmentry/api.c. -->

## Packaging & integration

<!-- NOTE: summarize wasmentry/PACKAGING.md — serving from a liwords-ui app,
     where the wasm/data live, the TypeScript wrapper. -->

## Running wasm tests

<!-- NOTE: how the wasm-tests CI job runs; reproducing locally. -->
