# Move generation, RIT and WIT investigation (2026-11-01)

Notes from one working session, kept so the work can continue elsewhere. Nothing here changes the engine; the patches in `patches/` are throwaway instrumentation.

## Setup

- **Machine:** Apple M2, 8 GB RAM. Its memory is small enough that tables of several GB distort wall-clock A/B results (see "Lesson" below). Confirm timing on a bigger machine; on an M5, #926 measured no throughput change.
- **Data:** `./download_data.sh` (data 20260925). Wordmaps are built with `./bin/magpie_convert dawg2wordmap CSW24 ./data`, and RITs with `./bin/magpie_convert klvwmp2rit CSW24 ./data`.
- **Builds:** profiles and counters use `make magpie BUILD=no_pgo_release`. The early benchmarks use `make release` (PGO). `make release` needs `data/lexica/CSW24.wmp`, which the download does not include, so build it first.
- **Workloads (single-threaded, fixed work, from the repo root):**
  - **Static:** `autoplay games 3000 -lex CSW24 -wmp true -rit true -ritmmap true -wit true -s1 equity -s2 equity -r1 best -r2 best -numplays 1 -gp true -threads 1 -seed 42 -hr true` (132,850 turns, about 2.9 s user).
  - **Sim:** `bin/magpie < tools/sim_fixed.cmd`. This is the cycle-profile skill's fixed 2-ply sim with WIT added: 7 positions of `success_long_game.gcg`, 8 candidates, 4000 iterations.

## 1. RIT and WIT benchmarks (PGO release, 8 threads)

CSW21, static equity players, 20,000 game pairs (`-gp true -seed 42`). The results are identical in every configuration.

| Config | Wall | User |
|---|---|---|
| neither | 3.55 s | 26.6 s |
| WIT | 3.65 s | 26.3 s |
| RIT | 4.43 s | 24.4 s |
| RIT + WIT | 4.63 s | 24.0 s |
| RIT + WIT, `-ritmmap true` (file cached) | 3.54–3.62 s | 24.0 s |

- **RIT:** saves about 9% CPU. The default load reads 1.9 GB on one thread, which costs 0.7–0.9 s of wall time. `-ritmmap true` avoids that once the file is in the page cache.
- **2-ply sims:** `magpie_test simbench`, CSW24, 8 threads, test build. Without RIT 21,872 iters/s, with RIT 23,900 iters/s (+9%).

## 2. RIT size: coverage subsets, and 17-bit packing (#926)

- **Composition:** the CSW24 RIT is 3,199,724 entries × 584 B. 384 B of each entry is 128 leave values stored as 24-bit integers. The values span −41,654 to 74,572 (under 2^17), with only 87,957 distinct values in the table. A rack averages 96.7 distinct sub-racks out of 128.
- **Partial tables:** a table holding only some racks works with no code change. Move generation falls back on a lookup miss, and output is byte-identical. `tools/rit_subset.py` writes a subset of a v12 table, keeping the racks most likely to be drawn from a full bag.

| Racks kept | File | Full-bag draws covered | simbench iters/s | vs no RIT |
|---|---|---|---|---|
| none | – | – | 22,344 | – |
| 10% | 189 MB | 72% | 23,541 | +5.4% |
| 20% | 378 MB | 86% | 23,959 | +7.2% |
| 50% | 943 MB | 98% | 24,626 | +10.2% |
| 100% | 1,885 MB | 100% | 24,786 | +10.9% |

  50% and 100% are within noise (±3%, 2 runs each). For static play, CPU time stops improving between 30% and 50%.
- **#926:** packs leave values at the width the KLV needs: 17 bits for standard leaves, 18 for `_super21`. CSW24 goes from 1.885 GB to 1.527 GB, with no change in play or throughput. On the M2 it looked +4.9% on simbench, but that was memory pressure, and on an M5 it measured no faster.
- **Not done:** keeping only distinct sub-racks would save another ~14% of a 50% table, at the cost of variable-size entries. Indexing racks directly instead of hashing would save ~4%.

**Lesson:** on this 8 GB machine, alternating two multi-GB tables evicts each from the page cache. Compare user time, not wall time, and confirm on a bigger machine before claiming throughput.

## 3. Move generation profile

Recorded with `.claude/skills/cycle-profile`; see its `record.sh` and `xct.py`.

- **Static** (6,000 pairs, 6.16 G cycles, about 23k cycles per turn):
  - Move generation is 87.7% of cycles: `gen_shadow` 36.5%, expanding anchors about 45%.
  - `play_move` (cross sets) is 9.4%.
  - No single function exceeds 10%.
- **Sim** (5.31 G cycles):
  - Rollouts 57.4%, inference 34.2%.
  - Config loading 7.6%, of which `kwg_get_hash` is 4.8%.
  - `board_copy` 4.8%, memmove 7.8%.
  - `gen_shadow` 21.9%.
- **Hottest line in both:** placing an anagram at a start column, in `wordmap_gen_check_playthrough_and_crosses`, called from `wordmap_gen_record_subrack`.
- **Easy separate win:** `players_data_validate_word_info_tables` hashes the whole KWG on every config load, about 3.5 ms per command. That is per-command latency, not search speed, and the hash could be computed once per KWG load.

## 4. Where anchor expansion wastes work

Exact counts from `patches/counters.patch`, per move generation:

| | static | sim |
|---|---|---|
| anchors in heap / expanded | 158 / 35.7 | 71 / 34.0 |
| expanded anchors that place any word | 8.7 (24%) | 6.8 (20%) |
| through-board anchors expanded / productive | 27.2 / 4.5 (17%) | 30.3 / 5.0 (17%) |
| rejected outright by the WIT | 3.2 | 3.0 |
| subrack lookups (share returning words) | 103 (93%) | 122 (81%) |
| words returned | 227 | 309 |
| placements checked / passed | 355 / 37 | 517 / 22 |
| through-board placements failing on a board letter | 80% | 85% |

**Oracle upper bound.** `patches/oracle_dump_reuse.patch` records which expanded anchors place a word, then replays the same games skipping the unproductive ones. Output is byte-identical; time with `tools/oracle_time.sh`.

| Skip unproductive anchors | static | sim |
|---|---|---|
| all | −19.8% user | −28.9% |
| through-board only | −16.6% | −26.2% |
| non-playthrough only | −3.0% | −1.1% |

## 5. Could the WIT hold more useful information?

The WIT stores letter sets per block (per final word length), combined over all start columns. Its word-plus-floater masks cover blocks of only 2–4 letters, and its position lengths are used in shadow. It never says which letter can go at which position.

`DUMP=file DUMP_EVERY=10` (in the oracle patch) samples expanded through-board anchors: lane letters, cross sets, rack, word length, start range, whether the anchor was productive, and whether the WIT rejected it. `tools/eval_filters.py WORDS.txt ANCHORS.txt` evaluates filters offline. The word list comes from `bin/magpie convert dawg2text CSW24`, which writes `data/lexica/CSW24.txt`. No filter rejected a productive anchor.

| Filter (share of unproductive through-board anchors rejected) | static | sim |
|---|---|---|
| WIT today | 14.4% | 11.9% |
| WIT + positional masks (block, length, offset → letters per position), with cross-sets and rack | 29.0% | 32.7% |
| same, rack only (no cross-sets) | 23.4% | 23.4% |
| exact: some word fits the board letters and allowed sets | 65.3% | 64.9% |
| exact, also respecting rack tile counts | 80.4% | 85.0% |

The remaining unproductive anchors have words that fit but lose to the subrack equity bounds, so no existence filter can catch them.

- **Positional masks:** 732,924 keys and 1,974,003 masks, about 8 MB for all of CSW24 (the WIT is 130 MB today). Rough gain, assuming cost per anchor is uniform (unverified): about 3% static, 6% sims.
- **Exact-fit check:** rough ceiling if free, about 10–13% static and 16–22% sims. Its cost is the open question.

## 6. Shadow: cost, and whether caching can help

**Self time (static):**

| Shadow sub-part | Self time |
|---|---|
| rightward extension (`shadow_play_right`) | 9.6% |
| recording bounds (`shadow_record_impl`, `wmp_move_gen_maybe_update_anchor`, `shadow_record_wmp`, add/reset anchors) | ~9.7% |
| multiplier bookkeeping (`insert_unrestricted_*`, `maybe_recalculate_effective_multipliers`) | ~5.1% |
| anchor scanning and starts | ~4.5% |

Per move generation: 329 bound records and 307 right steps (static), 248 and 231 (sim).

**Reuse.** These are unbounded hash sets over the whole run, weighted by shadow work. The window key hashes the squares an anchor's shadow can read.

| Key (share of shadow work seen before) | static, `-gp false` | sim |
|---|---|---|
| lane, or anchor window, only | 63–68% | 99% |
| + rack letter set + rack size | 0.2% | 34% |
| + full rack | 0.1% | 4.5–6.9% |

- **Game pairs inflate reuse.** With `-gp true`, the second game of each pair replays the first game's racks and boards, so the rack-aware keys show 50% reuse. That is an artifact.
- **The walk depends on the rack.** `possible_letters_here` includes `rack_cross_set`, and single-letter hooks restrict tiles (8–10% of steps, which depends on counts). Every record reads the rack's tile scores, RIT unions, nonplaythrough checks and best leaves.
- **Conclusion:** caching shadow results across regular move generations doesn't pay. At best it's about a third of sim work, keyed on letter set, and a hit still needs per-rack scoring. Estimated 2–4%, not pursued.
- **Existing caches:** `LaneShadowCache` and the endgame's incremental path lists exploit reuse in endgames, where racks are subsets of fixed racks. They don't apply here, and `LaneShadowCache` is disabled when a WIT is loaded. The sim carry (`best_move_carry.h`) keys by rack and lane hash, not by anchor.

## Next steps

1. **Exact-fit check before anchor expansion.**
   - What: a constrained existence search through the GADDAG for each through-board anchor. Start at a block, extend left and right through squares whose allowed letters are the cross-set ∩ rack (any letter with a blank, optionally tracking counts), and accept a word of the anchor's length that starts in its range and has empty squares at both ends. Stop at the first word found.
   - Where: hook it in `wordmap_gen` before `wmp_move_gen_build_playthrough_subracks`. It could also narrow the start-column range.
   - Soundness: output must stay byte-identical, which the oracle replay shows is achievable.
   - Measure: search cost against the 16.6% / 26.2% ceiling.
2. **Positional masks in the WIT**, if the exact check costs too much: about 8 MB, roughly double today's pruning.
3. **Cache the KWG hash** used by WIT validation, for per-command latency.
4. **Confirm any timing gain on a large-memory machine.**

## Reproducing the instrumentation

```
git apply notes/movegen_investigation/patches/oracle_dump_reuse.patch   # or counters.patch
make magpie BUILD=no_pgo_release
# counters / SHADOW / ANCHOR lines print to stderr at exit
ORACLE=record:/tmp/static.bits DUMP=/tmp/static_anchors.txt DUMP_EVERY=10 bin/magpie <STATIC command>
ORACLE=record:/tmp/sim.bits DUMP=/tmp/sim_anchors.txt DUMP_EVERY=15 bin/magpie < notes/movegen_investigation/tools/sim_fixed.cmd
notes/movegen_investigation/tools/oracle_time.sh bin/magpie /tmp 3
python3 notes/movegen_investigation/tools/eval_filters.py CSW24.txt /tmp/static_anchors.txt 20
git checkout src/impl/move_gen.c
```

The two patches don't stack, so apply one at a time. The `ANCHOR` and `SHADOW` lines report reuse; the `INSTR` lines (counters patch) report per-run totals.
