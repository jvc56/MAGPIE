## Summary

This PR adds lockless hashing for thread-safe transposition table access and fixes critical performance bugs in the ABDADA parallel search that made multi-threaded endgame solving **150x slower** than single-threaded at shallow depths.

### Lockless Transposition Table Hashing

Implements lockless hashing ([Hyatt 1999](https://www.cis.uab.edu/hyatt/hashing.html)) to prevent torn reads in the multi-threaded TT:
- Each 16-byte TTEntry is stored as two 8-byte halves with the key half XOR'd against the data half
- On load, a torn read (from concurrent write) produces invalid hash bits and the entry is rejected
- Zero measurable performance impact
- Unified TT struct across native and WASM platforms (removed WASM-specific `bytes_5_6` layout)
- Replaced volatile casts with C11 atomics (`atomic_load/store_explicit` with `memory_order_relaxed`)

### ABDADA Parallel Search Fixes

Diagnosed and fixed four issues that caused 16-thread search to take **9+ minutes** at depth 2 vs **3.5 seconds** single-threaded:

1. **Disable ABDADA exclusion at depth < 3**: The exclusive-search protocol (check-busy, defer, retry) has more overhead than the redundant work it prevents at shallow depths. The `abdada_active` flag now gates both the NPROC enter/leave and the is_busy check.

2. **Add `sched_yield()` in ABDADA deferral loop**: When all moves are deferred (being searched by other threads), yield CPU instead of tight-spinning. This prevents the busy-wait that burned 1482% CPU with zero algorithmic progress.

3. **Enable move ordering jitter at all depths**: Previously jitter was only applied at `depth > 2`, causing all threads to use identical move ordering at shallow depths and maximizing NPROC contention.

4. **Wider iterative deepening depth spread**: Threads now start at depths 1 through `min(4, plies-1)` instead of just 1 or 2, reducing contention at shallow iterative deepening levels.

### Benchmark Results

**7-ply endgame, 14domino position, release build (-O3), tt_frac=0.25:**

| Threads | Time (s) | Speedup |
|---------|----------|---------|
| 1       | 961.2    | 1.00x   |
| 2       | 728.2    | 1.32x   |
| 3       | 547.0    | 1.76x   |
| 4       | 423.1    | 2.27x   |
| 6       | 407.9    | 2.36x   |
| 8       | 358.9    | 2.68x   |
| **9**   | **301.9**| **3.18x** |
| 10      | 310.4    | 3.10x   |
| 11      | 313.7    | 3.06x   |
| 12      | 400.2    | 2.40x   |

- **Before fix**: 16T at depth 2 took 9+ minutes (vs 3.5s single-threaded), crashed with ASAN at depth 4
- **After fix**: Optimal at 9 threads = 301.9s (3.18x speedup over 1T)
- Sweet spot is 9-10 threads; sharp degradation past 11 due to ABDADA contention overhead

### Other Changes

- **PV display improvements**: Fixed PV moves showing score 0 by rewriting `pvline_reconstruct_from_tt`. Added end-of-game annotations showing opponent rack tiles and 2x rack point adjustments. Added `[P1 wins by X]` / `[P2 wins by X]` / `[Tie]` to per-ply output.

- **14domino / kue14domino on-demand tests**: Added the 14-tile domino endgame position as an on-demand test (`14domino`, `kue14domino`). Added `-wmp false` to all endgame test configs since WMP files are not needed.

- **Thread sweep benchmark** (`estquality` on-demand test): Parameterized benchmark that sweeps thread counts and prints a summary table with speedup factors.

- **Ground truth value map infrastructure**: Added `ValueMap` (direct-mapped hash table) for recording negamax values during a shallow search and replaying them as move estimates (with optional Gaussian noise) in a deeper search. This enables future experiments on how estimate quality affects alpha-beta pruning efficiency.

- **WASM CI**: Added `magpie_wasm_test` target and `wasm-tests` GitHub Actions job with Node.js WASM execution. Made TT tests portable across native (2^24) and WASM (2^21) table sizes.

- **`print_pv_callback` buffering fix**: Changed from `printf` (stdout, block-buffered when piped) to `fprintf(stderr)` for immediate per-depth output during iterative deepening.

## Test plan

- [x] ASAN clean through 7-ply solve with 4 threads (dev build, `-fsanitize=address,undefined`)
- [x] Correct endgame value (52) across all thread counts (1-12)
- [x] Thread sweep benchmark completes with consistent results
- [x] Single-threaded performance unchanged (~961s release, 7-ply 14domino)
- [ ] Run existing endgame test suite (`14domino`, `kue14domino`)
- [ ] WASM test target builds and passes
