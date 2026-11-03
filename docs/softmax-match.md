# Equal-clock softmax versus no-inference match

`infmatch:PAIRS:GAME_MS:SEED:STRIDE:NULL` is an on-demand test harness. Player 0
uses softmax inference at margin 10 / temperature 2; player 1 samples uninformed
opponent racks. With `NULL=1`, both disable inference for harness smoke tests.
Both players simulate: CSW24, 15 equity candidates, two plies, top-two IDS,
30-rollout initialization, bag cycling, WMP/RIT/WIT enabled, PAT disabled, and
utility weights win=1, spread=0.5, spread scale=100. Empty-bag simulation uses the
engine's normal deterministic full-game rollout behavior.

Each player receives the same per-game wall-clock allowance. Candidate generation,
inference, simulation, and search teardown are charged. Shared setup, applying
moves, and logging are outside the clock. Simulation gets the remaining clock
split by estimated remaining turns. Inference can borrow from the game clock;
its elapsed time reduces later simulation allowances. A sleeping watchdog uses
the engine's cooperative search-budget stop at the remaining game deadline.
With less than 50 ms left, play falls back to static evaluation. Cooperative
stops may slightly overrun; actual times, clock stops, and fallbacks are logged.
This differs from ordinary `-tlim`, which does not charge preceding inference.

Pairs use the same game seed and alternate the starting player. Each worker uses
one compute thread; the timer sleeps. Different workers use interleaved disjoint
pair seeds. Timed move choices are not reproducible exactly from seeds because
machine scheduling affects how much search completes. Statistical comparisons
use completed pairs, with ties counting half a win.

## Overnight experiment, November 2–3, 2026

- 18 concurrent workers, one simulation/inference thread each.
- 10,000 ms per player per game.
- Main seed 2026110300, stride 18; pilots use separate seeds.
- Fixed stop: November 3, 7:00 AM Pacific (15:00 UTC).
- No stopping based on observed wins; incomplete pairs at the deadline are excluded.
- Output: `data/benchmarks/softmax-overnight-20261102/nightly/`.

The 18-worker calibration completed 72 games / 36 pairs in 73.245 seconds.
Neither player had a clock stop or clock-driven static fallback. Mean time per
game was 8.823 s for softmax and 8.727 s for control; unused allowance is expected
when a game ends or deterministic endgame rollouts finish quickly. Softmax used
nonempty inferred leaves on 10.319 of 10.819 inference attempts per game.
Mean rollouts per turn were 17,945 and 18,883 respectively, across candidates.
Bag-cycled rollouts are correlated rotations, not independent bag samples.

At that rate, about 27,000–28,000 games fit before the deadline. Independent-game
planning requires approximately 9,600 games for a 95% interval of ±1 percentage
point near 50%, but about 19,600 games for 80% power to detect a true 51% win rate
with a two-sided 5% test (about 32,500 for 95% power). Pairing changes the variance;
the final report uses the empirical variance of pair-average outcomes rather
than treating paired games as independent. These power figures assume the real
edge at this particular clock is 51/49; they do not predict an edge exists.

## Running and results

Build with `make magpie_test BUILD=no_pgo_release` and prepare RIT/WIT with
`make prepare_data`. The runner accepts a frozen binary and records its SHA-256:

```sh
python3 scripts/run_inference_match.py \
  --binary /absolute/path/to/frozen/magpie_test \
  --directory data/benchmarks/softmax-overnight-20261102/nightly \
  --stop-at 2026-11-03T07:00:00-08:00 --workers 18 --budget-ms 10000
```

`manifest.json` records the settings, `worker-*.log` contains per-game telemetry
and completed pair outcomes, and `summary.json` updates every 15 seconds. Its
primary estimate uses only `INFMATCH_PAIR` records. The 95% interval is a normal
interval across independent pairs; early small-sample intervals are provisional.
The deadline mechanism was smoke-tested with two workers and stopped at 8.019 s
for an 8 s deadline. Short-clock inference and null matches passed sanitizers.
