# Benchmark Infrastructure

## Overview

The benchmark infrastructure supports round-robin tournaments between
configurable Scrabble AI player types. It runs deterministic game pairs
(same seed = same tile draws) to reduce variance and isolate strategic
differences.

## Files

| File | Purpose |
|---|---|
| `test/roundrobin_benchmark.c` | Main tournament driver |
| `test/roundrobin_benchmark.h` | Header |
| `test/debug_infer_turn2.c` | Debug test for the minp=1 bingo bug |
| `test/debug_infer_turn2.h` | Header |
| `test/inference_gamepair_benchmark.c` | Game-pair benchmark for inference |
| `test/inference_gamepair_benchmark.h` | Header |
| `test/inference_timing_benchmark.c` | Timing benchmark for inference |
| `test/inference_timing_benchmark.h` | Header |

## Running

All benchmarks are registered as on-demand tests in `test/test.c`:

```bash
./bin/magpie_test roundrobin    # Full round-robin tournament
./bin/magpie_test debug_infer   # Replay seed 7014 turn 2 (minp bug)
./bin/magpie_test infgp         # Inference game-pair benchmark
./bin/magpie_test inftiming     # Inference timing benchmark
```

## Round-Robin Tournament Design

### Player Types

The tournament supports four player types, configured via enum:

- **PLAYER_STATIC:** Picks the equity-best move with no simulation or
  inference. Instant decisions. Baseline player.

- **PLAYER_SIM:** Runs 2-ply Monte Carlo simulation for the full turn
  budget. No opponent modeling.

- **PLAYER_INFER_EQ1:** Runs inference (opponent rack estimation) before
  simulation. `equity_margin=1` means inference runs whenever any move is
  within 1 equity point of the best — effectively almost always.

- **PLAYER_INFER_EQ20:** Same as above but `equity_margin=20`. Inference
  only runs when the top moves are within 20 equity points of each other.
  Preserves more budget for simulation on clear-cut decisions.

### Game Pair Structure

For each seed, every pair of player types plays TWO games:
- Game A: Player X as p0, Player Y as p1
- Game B: Player Y as p0, Player X as p1

Same seed means same tile bag ordering. With 4 players, there are
C(4,2) = 6 matchups × 2 games = 12 games per seed.

### Determinism

Given the same seed, STATIC will always make the same moves. SIM and
INFER players may differ from STATIC (and from each other) when simulation
or inference changes the move choice. When two players agree on every move,
their games produce identical scores — this is expected, not a bug.

### Turn Flow

For SIM/INFER players, each turn proceeds:

1. **Inference phase** (INFER players only): If the previous move was a
   tile placement and the bag has enough tiles, run inference on the
   opponent's last move to estimate their rack. Time limit: 1s.

2. **Budget check:** `sim_budget = turn_budget - infer_elapsed`. If
   sim_budget < 1.0s, fall back to equity-best (skip simulation).

3. **Simulation phase:** Run 2-ply BAI simulation with remaining budget.
   If inference succeeded, the sim uses the inferred rack distribution
   to weight opponent tile draws.

4. **Move selection:** Play the BAI-selected best move.

### Endgame

When the bag is empty, ALL player types (including STATIC) use the
endgame solver. This isolates the midgame strategy differences.

### Output

Results are appended to a CSV log file:
```
seed,p0_type,p1_type,p0_score,p1_score,turns,elapsed_s
9000,STATIC,SIM2,369,504,20,21.5
```

### Key Parameters

```c
#define NUM_SEEDS 350        // Total seeds to play
#define NUM_PLAYS 15         // Move candidates for simulation
#define NUM_PLIES 2          // Simulation depth
#define NUM_THREADS 10       // Parallel simulation threads
#define BUDGET_AFTER_SCORING_S 2.0  // Time budget per turn
#define INFER_TIME_LIMIT_S 1.0      // Max time for inference
```

The `min_play_iterations` parameter (set to 50) controls the minimum
number of BAI samples per move arm before the algorithm begins
concentrating on the best candidates.

## Analyzing Results

Tournament logs can be analyzed with the awk scripts in `/tmp/`:
- `/tmp/crosstable.awk` — Per-matchup W-L and spread breakdown
- `/tmp/textedit_table3.awk` — Sorted crosstable (win% and spread)
- `/tmp/elo.awk` — Bradley-Terry Elo ratings
- `/tmp/sweep_crosstable.awk` — Game-pair sweep/split analysis
- `/tmp/significance.awk` — Statistical significance tests
- `/tmp/full_report.awk` — Comprehensive report generator

These are ephemeral (in /tmp) and would need to be recreated. The key
patterns are preserved in this documentation.

## Lessons for Future Tournaments

1. **min_play_iterations must be >= 50.** With minp=1, BAI permanently
   writes off moves after a single unlucky playout.

2. **Budget fallback is essential.** When inference takes >1s, the
   remaining sim budget may be insufficient. Always check and fall back
   to equity-best if sim_budget < 1.0s.

3. **sim_results can be stale.** SimResults is not reset between
   simulate() calls. If sim exits early (timer interrupt during initial
   phase), results from the previous call persist. Always verify results
   are fresh before using them.

4. **~100 seeds gives rough Elo estimates (±15-20 points).** For
   statistical significance on small edges (2-3%), you need 500+ seeds.

5. **Game pairs reduce variance.** Same seed means same tiles; strategic
   differences are isolated. Most pairs between similar players split
   (each wins one game), confirming that tile luck dominates.

6. **Wall time:** ~4.6 minutes per seed on M4 Mac Mini with 10 threads
   and 2s budget. 100 seeds ≈ 7.5 hours. 350 seeds ≈ 27 hours.
