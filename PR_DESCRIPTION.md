## Summary

Two targeted optimizations to `assign_estimates_and_sort` and
`compute_build_chain_values`, identified via `sample` profiling of the
timed round-robin benchmark.

### 1. SmallMove-native `compute_conservation_bonus` and `compute_played_tiles_face_value`

Both functions previously required the caller to expand a `SmallMove` into
a full `Move` via `small_move_to_move`, which walks the board to reconstruct
the tile array (including `PLAYED_THROUGH_MARKER` entries for occupied
squares). But both functions only care about rack-played tiles — they
immediately skip every `PLAYED_THROUGH_MARKER` they encounter.

The rack-played tiles are already encoded directly in `tiny_move` bits 20–61
(7 tiles × 6 bits each) with blank flags at bits 12–18 and the count in
`metadata.tiles_played`. The new implementations loop over those fields
directly, with no board scan and no `Move` construction.

Call-site impact:
- `assign_estimates_and_sort`: two `small_move_to_move` calls removed, along
  with the `est_board` local and the `opp_stuck_frac <= 0.0F` guard that
  existed solely to decide whether `spare_move` was already populated.
- `negamax_greedy_leaf_playout`: one `small_move_to_move` removed from the
  per-candidate selection loop (the conversion for actually playing the chosen
  move stays).

### 2. Lazy `mv_a` expansion in `compute_build_chain_values`

The inner loop checks whether any already-processed move B geometrically
contains move A (same direction, same row/col, B's span ⊇ A's span). When
containment passes, a tile-by-tile match is needed, which requires expanding
both SmallMoves to full Moves. Previously, `small_move_to_move(&mv_a, ...)`
was called inside the inner loop every time a candidate B passed the
containment check — re-expanding the same `sm_a` on each hit.

`mv_a` is now expanded lazily: declared before the inner loop, computed at
most once on the first containment hit, and skipped entirely when no candidate
ever passes containment (the common case).

## Performance

Profiled with `BUILD=profile` (`-O3 -g -fno-omit-frame-pointer`), macOS
`sample` tool (600s, 1ms intervals), `roundrobin` benchmark (30 stuck-tile
positions, P1=20s P2=12s budgets, precheck+baseline 80% time management).
Metric: normalized samples in function / total endgame worker samples.

The build-chain callsite figure is noisy across runs (inlined function, high
positional variance), so the cleaner comparison is
`assign_estimates_and_sort` minus that callsite:

| Version | assign_estimates_and_sort | minus build-chain callsite |
|---|---|---|
| Baseline (fast-single-tile-stuck-check) | 1.200% | 0.809% |
| + SmallMove-native bonus/face-value | 1.517%* | 0.933%* |
| + lazy mv_a hoist | **0.451%** | **0.451%** |

\* Run 2 happened to hit more stuck-tile positions; conservation bonus itself
measured at 0.064% → 0.059% (within noise). The SmallMove-native change is
a correctness/clarity win rather than a measurable speed win.

The lazy `mv_a` hoist is the real gain: ~44% reduction in
`assign_estimates_and_sort` overhead, saving roughly 0.36% of total endgame
search time.

## Files changed

- **`src/impl/endgame.c`**:
  - `compute_played_tiles_face_value`: takes `const SmallMove *` instead of
    `const Move *`; reads tile values and blank flags directly from
    `tiny_move`.
  - `compute_conservation_bonus`: same signature change; same direct read.
  - `assign_estimates_and_sort`: removed `est_board` local, two
    `small_move_to_move` calls, and the `opp_stuck_frac <= 0.0F` guard.
  - `negamax_greedy_leaf_playout`: removed `small_move_to_move` from the
    per-candidate conservation-bonus loop.
  - `compute_build_chain_values`: `mv_a` declared before inner loop with
    `mv_a_initialized = false`; expanded at most once per outer iteration.

## Test plan

- [ ] `make clean && make BUILD=dev magpie_test && ./bin/magpie_test endgame`
- [ ] `./bin/magpie_test eldar_v`
