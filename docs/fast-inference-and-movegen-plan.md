# Fast inference, move generation, and table memory optimizations: plan

Source: profiling notes on `origin/movegen-investigation-notes` (`391b4b6c`,
measured on Apple M2 and M5 Max after `#928`), `census` / `census_play_scores`
(`#921`–`#923`), `rit` (`#918`, `#926`), and full codebase review at `v0.1.4`
(`57c8f810`).

Companion roadmap documents:
- `docs/inference-parity-and-softmax-plan.md` — `inference.c` parity fixes
  (PR 1) and standalone static/PAT softmax inference (PR 2).
- `docs/lategame-search-and-solver-plan.md` — late-game racing sim (`bag` 5–8),
  `PlayChooser` upgrades, endgame/PEG scaling, and strategy pipeline.

### Empirically ruled-out paths (do not re-attempt)
Per `origin/movegen-investigation-notes` (`notes/movegen_investigation/README.md`)
and `origin/claude/static-ish-rollouts`:
- **Per-lane shadow upper bounds** to skip entire lanes before shadow: skips
  25.7%–30.5% of unproductive lanes with RIT word-existence checks, but computing
  or maintaining the bound costs more cycles than the skipped shadow work.
- **Cross-movegen lane shadow caching outside endgames:** hit rates in midgame
  static play and sims do not pay for cache hashing and management (unlike
  `MOVE_RECORD_BEST_SMALL` in endgames where racks and boards repeat densely).
- **Fixed-trip multiplier rebuilds, stepping over board-tile runs in shadow, and
  force-inlining `shadow_record_wmp`:** removed zero branch mispredicts on M5 Max.

---

## 1. Inference acceleration (34.2% of `sim` CPU)

In `tools/sim_fixed.cmd` profiles, rack inference accounts for roughly a third
of total simulation CPU. Every candidate leave in an `infer` call runs against
**one immutable board** with PAT disabled (`get_top_equity_move_for_inferences`,
`src/impl/gameplay.c:961-976`).

### 1.1 Single-pass board play-scores for exchanges and small plays (`census_play_scores`)

#### Problem
When the opponent exchanges (`0` tiles played, `7` inferred), passes (`7`
inferred), or plays 1–3 tiles (`4–6` tiles inferred), `infer` currently calls
`generate_moves` tens of thousands to hundreds of thousands of times on the
**exact same board**. Each call reloads `MoveGen`, unpacks RIT/KLV leaves, runs
shadow across 30 lanes, heaps anchors, and scans WMP/GADDAG entries — even
though the board's legal tile-placement multisets and their maximum scores never
change across the entire `infer` call.

#### Key observation
Inference (`evaluate_possible_leave`, `src/impl/inference.c:213-240`) does
**not** need the best `Move`'s coordinates, direction, or word strip — it only
needs the scalar maximum static equity $E_{\text{top}}(R)$ over legal moves on
candidate 7-tile rack $R = P_{\text{obs}} \cup L$ (or early detection that
$E_{\text{top}}(R) > \text{cutoff}$)!

With `#922` (`src/impl/census_play_scores.h:22-25`,
`src/impl/census_play_scores.c:609-639`), `census_play_scores_build` already
walks the board **once** (~1–2 ms) and records the maximum score achievable on
the board for every playable rack multiset $S$ ($1 \le |S| \le \text{RACK\_SIZE}$)
in the `CensusLattice` (`src/impl/census.c:14-28`). Even better, on a typical
midgame board only a few thousand multisets $S$ have nonzero play scores (which
`CensusPlayScoresCacheEntry` in `src/impl/census_gen.c:130-135` already stores
sparsely).

#### Exact algorithm
Gate this path to positions where per-rack `MoveGen` is expensive and board-only
play scores are exact:
- `tiles_to_infer >= 4` (or `bag_unseen_combinations >= threshold`, e.g. `2,000`
  candidate leaves),
- `board_get_tiles_played(board) > 0` (non-opening board, so
  `placement_adjustment == 0`; on an empty opening board, `census_play_scores`
  does not include `placement_adjustment` unless extended with opening penalties
  during the single-lane opening scan),
- PAT disabled in inference (`disable_pat == true`),
- Standard/classic variant (`!is_woadwage && !is_wordsmog`) and `BitRack` /
  lattice compatible.

Steps for an inference with fixed observed played multiset $P$ ($|P| = p \in [0, 3]$,
empty for exchanges/passes) and candidate leaves $L$ of size $k = \text{RACK\_SIZE} - p$:
1. Build the unseen-pool or full-distribution `CensusLattice` and run
   `census_play_scores_build` **once** on `game_get_board(game)` (parallelizable
   across lanes or 1–2 ms single-threaded), producing sparse nonzero pairs
   $(S, \text{max\_score}(S))$ for $1 \le |S| \le \text{RACK\_SIZE}$.
2. Allocate an array `best_equity[num_candidate_leaves]` initialized to each
   candidate leave $L$'s non-placement floor:
   - `EQUITY_PASS_VALUE` when exchanges are illegal ($B < 7$), or
   - the best exchange equity on $R = P \cup L$ when $B \ge 7$ (computed on the
     lattice via DP over submultisets of $R$: for each multiset $M$ of size
     $1..\text{RACK\_SIZE}$, $\text{best\_subleave}(M) = \max(\text{KLV}(M), \max_{c \in M} \text{best\_subleave}(M \setminus \{c\}))$,
     which computes best exchanges for **all** candidate racks in a single
     bottom-up lattice sweep with **zero** per-rack KLV tree walks!).
3. For each nonzero board play multiset $(S, \text{score}(S))$:
   - Decompose $S$: any valid play on $R = P \cup L$ uses some submultiset
     $S_P \subseteq P \cap S$ from the known played tiles and requires the
     remaining tiles $S_L = S \setminus S_P$ (with $|S_L| \le k$) to come from
     the candidate leave $L$, provided $S_L$ fits within `bag_as_rack`.
   - Iterate over the (few) candidate leaves $L \supseteq S_L$ in the
     `bag_as_rack` sub-lattice: the remaining leave after playing $S$ from
     $P \cup L$ is $L' = (P \cup L) \setminus S$, whose KLV value is one array
     lookup, updating `best_equity[idx(L)] = max(best_equity[idx(L)], score(S) + klv[L'] + peg_adjust[|S|])`.
   - Alternatively, invert via upward zeta/DP propagation on the `bag_as_rack`-restricted
     7-tile rack lattice when `tiles_to_infer == 7` (exchanges/passes), matching
     `census`'s exact upward sweep!
4. Score every candidate leave $L$ directly from `best_equity[idx(L)]` against
   `E_obs` (supports **both** hard-margin `-eqmargin` and PR 2's Regret
   Boltzmann softmax!).

**Validation:** Add an `infer_cmp_test` case asserting byte-identical
`InferenceResults` between `census_play_scores` inference and `MoveGen`
inference across 100 random midgame positions covering exchanges, passes, and
1-, 2-, and 3-tile plays.

---

### 1.2 RIT-accelerated exchange inference (`find_best_exchange_leave`)

When the `MoveGen` path runs on an exchange of $N$ tiles (`src/impl/inference.c:169-205`),
`find_best_exchange_leave` currently does a recursive `klv_get_leave_value`
walk over all $\binom{7}{7-N}$ subracks of `current_target_leave` (which for an
exchange is the full 7-tile candidate rack), even when the target player's
`RackInfoTable` (`RIT`) is loaded!

Fix (`src/impl/inference.c`):
- Cache `const RackInfoTable *rit` on `Inference` (gated by
  `bit_rack_is_compatible_with_ld(ld)` just like `gen_load_position` at
  `src/impl/move_gen.c:3249-3250`).
- In `get_best_exchange_equity`, if `inference->rit != NULL`, look up the
  candidate 7-tile `BitRack` in `inference->rit`. On a hit:
  - Note: `evaluate_possible_leave` (`src/impl/inference.c:248-274`) needs
    **both** `best_value` and `inference->best_exchange_leave` (the specific
    $(7-N)$-tile `Rack` kept, or all competitive $(7-N)$-tile subleaves under
    PR 2's intra-rack exchange softmax).
  - Unpack the 128 subrack leave values from `RackInfoTableEntry`
    (`rack_info_table_unpack_leaves`, `src/ent/rack_info_table.h:155-210`) once
    into a `LeaveMap` (the exact array `MoveGen` is about to use on line 230 —
    in fact `MoveGen`'s per-thread `rit_cache` will immediately hit on the same
    `BitRack`!) and scan only masks of popcount `keep_size` in $O(1)$ array
    indexing without touching the KLV DAWG.

---

### 1.3 Batched subtree claiming in `iterate_through_all_possible_leaves`

#### Problem
In `src/impl/inference.c:468-500`, every worker thread traverses the **entire**
combinatorial leave tree from the root (`increment_letter_for_inference` /
`decrement_letter_for_inference` at every node), and at every leaf calls:
```c
return atomic_fetch_add(&ctx->rack_claim_counter, 1);
```
comparing `rack_index == *target_rack_index`. With 8–16 threads and 50,000
leaves:
1. All 16 threads walk all 50,000 leaves ($800{,}000$ recursive visits instead
   of $50{,}000$), wasting L1 work on unowned leaves.
2. Every leaf executes a cross-core `atomic_fetch_add` on one shared cache line.
3. Consecutive leaves (which share 6 of 7 tiles, maximizing `MoveGen` subrack
   cache and RIT page locality!) are round-robined across different cores
   instead of staying on one core.

#### Fix
1. Precompute a tiny `uint64_t subtree_leaves[MAX_ALPHABET_SIZE + 1][RACK_SIZE + 1]`
   DP table once in `infer()` before launching threads:
   `subtree_leaves[ml][r]` = number of distinct multisets of size `r` formed
   from letters `ml .. ld_size - 1` given multiplicities `bag_as_rack[ml..]`:
   $$T(\text{ml}, r) = \sum_{c = 0}^{\min(r, \text{count}[\text{ml}])} T(\text{ml} + 1, r - c).$$
   This table is `(MAX_ALPHABET_SIZE + 1) * (RACK_SIZE + 1) * 8` bytes (~3.6 KB)
   and takes `< 1` microsecond to fill.
2. Workers claim ranges `[claim_lo, claim_hi)` in batches of `BATCH_SIZE = 64`
   via `atomic_fetch_add(&ctx->rack_claim_counter, 64)`.
3. In `iterate_through_all_possible_leaves(ml, tiles_remaining, leaf_base_idx)`:
   - Let `span = subtree_leaves[ml][tiles_remaining]`.
   - If `leaf_base_idx + span <= claim_lo`, skip the entire subtree in $O(1)$
     (`return leaf_base_idx + span;` without incrementing/decrementing any
     rack letters!).
   - If `leaf_base_idx >= claim_hi`, claim the next `[claim_lo, claim_hi)` batch
     (or return immediately if `claim_lo >= total_leaves`).
4. Result: $64\times$ fewer atomic operations, $>90\%$ of unowned tree nodes
   skipped in $O(1)$, and 64 consecutive leaves sharing 5–6 tiles evaluated
   back-to-back on the same thread's `MoveGen` caches.

---

### 1.4 Multi-turn & CGP inference fixes

1. **Issue `#839` (`commit` after `cgp` loses base board in `GameHistory`):**
   - `parse_cgp` (`src/impl/cgp.c`) populates `Game`'s board directly without
     recording the initial CGP string in `GameHistory`. A subsequent `commit`
     appends an event to an otherwise empty-board `GameHistory`, so
     `populate_inference_args_with_game_history` (`src/impl/inference.c:741`)
     calls `game_play_n_events` from an empty board instead of from the CGP
     board!
   - **Fix:** Store an optional `char *initial_cgp` (or base board/score/rack
     snapshot) on `GameHistory` whenever `cgp` loads a position; have
     `game_history_reset` clear it and `game_play_n_events` load `initial_cgp`
     before replaying events `0 .. n - 1`. Add a unit test in `test/infer_test.c`:
     `cgp <nonempty> -> commit <move> -> infer` verifying `infer` sees the CGP
     board.
2. **64-bit draw counts (`BOARD_DIM=21` / `RACK_SIZE=8` safety):**
   - `get_number_of_draws_for_rack` (`src/impl/inference.c:91`) returns
     `uint64_t`, but lines 269, 273, 282, 287 cast `(int)number_of_draws_for_leave`
     when calling `leave_rack_list_insert_rack` and `alias_method_add_rack`. On
     Super Scrabble (`200` tiles) or `RACK_SIZE=8`, $\prod \binom{n_i}{k_i}$ or
     summed `total_count` can exceed `INT32_MAX`. Widen draw counts in
     `LeaveRack` (`uint64_t`) and `AliasMethod` (`double` or `uint64_t`, pairing
     cleanly with PR 2 in `docs/inference-parity-and-softmax-plan.md`).
3. **Cross-turn Bayesian leave persistence (research track):**
   - When the opponent exchanges $k$ tiles on turn $t - 2$ and plays 2 tiles on
     turn $t$, today turn $t$'s inference treats all unseen tiles symmetrically,
     forgetting that $7 - k$ of the opponent's tiles survived turn $t - 2$'s
     exchange. Passing an optional prior tile-marginal or `LeaveRackList` from
     turn $t - 2$ into turn $t$'s draw-weight calculation tightens 2-turn
     exchange-then-setup inferences.

---

## 2. Move generation, WMP, WIT, RIT & memory footprint

### 2.1 Zero-RAM GADDAG exact-fit filter before playthrough subrack expansion

#### Profiling target
In `notes/movegen_investigation/README.md`, unproductive through-board WMP
anchors (`playthrough_blocks > 0` that ultimately record 0 legal plays) consume
**16.6% of static** and **26.2% of `sim` CPU** (building playthrough subrack
lists, probing WMP hashes, and failing cross/playthrough checks). On WASM
(`wasmentry/api.c:25-27`), both WIT and RIT are disabled to fit browser memory,
so unproductive anchor expansion is even larger there.

#### Mechanism (0 MB extra RAM)
Every lexicon `KWG` already builds per-node `sibling_tiles` and
`accepting_tiles` bitsets at load time (`kwg_try_build_tile_bitsets`,
`src/ent/kwg.h:139-173`). Before calling
`wmp_move_gen_build_playthrough_subracks` for a `(length, tiles_to_play)` WMP
anchor in `wordmap_gen` (`src/impl/move_gen.c:1154`):
- We know the exact placement window `[leftmost_start_col, rightmost_start_col]`
  around the rightmost anchor/playthrough block, the exact fixed board letters
  on occupied squares, the exact `cross_set` bitmask on empty squares, and the
  player's `rack_cross_set` (plus blank availability).
- A tiny stack-allocated bounded GADDAG existence walk (starting at the anchor
  block in the GADDAG, stepping left to `start_col`, crossing `SEPARATOR_MACHINE_LETTER`,
  and stepping right to `start_col + word_length - 1`) intersects
  `kwg_get_sibling_tiles(kwg, node_idx) & allowed_square_mask` in $O(1)$ per
  level and tests `kwg_get_accepting_tiles(kwg, node_idx) & final_square_mask`
  at the last square without chasing the final arc!
- Better yet, running the existence check per `start_col` produces a bitmask
  `valid_start_cols_mask` of start columns where **at least one** lexicon word
  fits the board letters, cross-sets, and rack letter set:
  - If `valid_start_cols_mask == 0`, skip `wmp_move_gen_build_playthrough_subracks`
    and all WMP hash probes for this anchor completely!
  - Otherwise, tighten `[leftmost_start_col, rightmost_start_col]` to the lowest
    and highest set bits of `valid_start_cols_mask` (and pass
    `valid_start_cols_mask` into `wordmap_gen_record_subrack` to skip dead
    `start_col` offsets immediately!).

**Benchmark gate (`AGENTS.md`):** Measure with `cycle-profile` (`xctrace`) on
both `autoplay games 3000` (static, WIT/RIT on and off) and
`tools/sim_fixed.cmd`. Must not regress WIT-on speed and should give a major
speedup when WIT is off (including WASM). Confirm byte-identical move lists via
`movegen` tests.

---

### 2.2 Branchless / SWAR playthrough-and-cross check

After `#928` removed the four shadow branch mispredicts, the hottest remaining
functions and ~21% of static branch mispredicts sit in
`wordmap_gen_check_playthrough_and_crosses` (`src/impl/move_gen.c:842-900`) and
`wordmap_gen_record_subrack` (`src/impl/move_gen.c:936-981`), which iterate a
variable-trip `while (playthrough_mask != 0)` loop with `lowest_set_bit_index`
on every `(word, start_col)` pair.

Optimization (`BOARD_DIM == 15`):
- Cache an aligned `uint8_t lane_unblanked_letters[32]` array once per lane when
  `MoveGen` loads a row/column (`0` on empty squares, unblanked `MachineLetter`
  `1..35` on occupied squares) alongside a 16-bit `lane_occupied_mask`.
- For a candidate `word` (`const MachineLetter *word` of length $L \le 15$) at
  `start_col`:
  - Load 16 bytes from `lane_unblanked_letters + start_col` and 16 bytes from a
    zero-padded buffer of `word` (or read directly from the WMP word arena when
    at least 16 bytes remain before arena end).
  - Compare all 16 bytes at once (portable C99 SWAR on two `uint64_t` chunks, or
    platform SIMD hidden behind `src/compat/` per `check_portability.py`!), mask
    by `(lane_occupied_mask >> start_col) & ((1U << L) - 1)`, and reject any
    playthrough mismatch with **one** predictable branch instead of `k`
    data-dependent branches!
- Only words whose fixed board letters all match proceed to the empty-square
  nontrivial cross-set loop (`nontrivial_cross_mask`), which is usually 0–2
  bits.

---

### 2.3 Probability-ranked partial RITs in `auto` mode (8–16 GB machines)

- **Problem:** A full CSW24 RIT is `1.89 GB` (`1.53 GB` with `#926`'s 17-bit
  leave packing). `contribute_choose_tables` (`auto` mode) currently makes a
  binary all-or-nothing choice based on system RAM, so 8–16 GB laptops either
  get no RIT or risk page-cache thrashing (`notes/movegen_investigation/README.md`
  documented how multi-GB tables distort wall-clock performance on an 8 GB M2).
- **Fact:** RIT misses fall back transparently to KLV/WMP enumeration with
  byte-identical output. Sorting full 7-tile racks by full-bag hypergeometric
  draw probability, the top **20%–30%** of racks (~305–460 MB with `#926`)
  cover **86%–92% of real game racks** and capture ~75% of the full RIT speedup!
- **Design:**
  - Extend `magpie_convert klvwmp2rit` with an optional `--max-bytes <N>` or
    `--coverage <pct>` flag (keeping only the highest-draw-probability racks in
    a compact open-addressed or bucketed table).
  - Alternatively, store racks in the `.rit` file sorted in descending draw
    probability blocks with a small block-index header so `auto` on an 8–16 GB
    machine can `mmap` or read just the top $M$ MB tier from a **single** `.rit`
    file without shipping two files!

---

### 2.4 Smaller KWG bitsets, memoized KWG/KLV hashes, and `endian_io.h` hygiene

1. **32-bit `sibling_tiles` / `accepting_tiles` for alphabets with `max_tile < 32`:**
   - `KWG` (`src/ent/kwg.h:18-29`) currently allocates two `uint64_t` arrays
     (`16` bytes per KWG node — `~24 MB` extra for CSW24, `~60 MB` for Polish
     OSPS49).
   - English, French, and German have `ld_size <= 29 < 32`. Using `uint32_t *`
     bitsets when no node has `tile >= 32` cuts bitset RAM in half (`24 MB -> 12 MB`
     on CSW24), helping L3 cache locality and 32-bit WASM worker heaps while
     keeping the `uint64_t *` path for Polish/Catalan/Super alphabets (`tile >= 32`).
2. **Memoize `kwg_get_hash` (`src/ent/kwg.h:45-52`) and `klv_get_hash`
   (`src/ent/klv.h:60-61`):**
   - `players_data_validate_word_info_tables` (`src/ent/players_data.c:504`)
     recomputes an FNV-1a hash over the entire KWG node array (~3.5 ms, 4.8% of
     short sim cycles) whenever validating tables. Compute `kwg->hash` and
     `klv->hash` once at load/mutation time and return the cached `uint64_t` in
     $O(1)$.
3. **Route `KWG` I/O through `src/compat/endian_io.h`:**
   - `kwg_read_nodes_from_stream` (`src/ent/kwg.h:194-204`) and
     `kwg_write_to_file` (`src/ent/kwg.h:270-288`) still contain raw
     `IS_LITTLE_ENDIAN` / `le32toh` / `htole32` loops instead of
     `fread_le_uint32s` / `fwrite_le_uint32s` (`AGENTS.md` Pitfall #10). Replace
     them in a small cleanup PR.
