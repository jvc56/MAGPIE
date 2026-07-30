# PEG time-budget calibration

## Result

On 40 held-out positions balanced across bag sizes 1–4, 2-ply PEG was
materially better than the greedy PEG seed, but increasing the 2-ply cap from
30 to 60 or from 60 to 120 seconds did not change a nominated move. Longer
caps did finish more candidates on the hard tail. This result supports keeping
production PEG shallow, but it does not justify a TimeManager policy change or
a particular longer cap yet.

The branch is based on draft PR #633 at
`5a1dc19e3ca2862e2eafb08957ae26200e47c9ae`. The calibration code adds
telemetry and a benchmark-only forced stride for small bags; all production
callers retain their prior behavior.

## Protocol

- Build: `no_pgo_release`; RIT enabled and memory-mapped; CSW24; 10 PEG
  workers.
- Positions: two prospectively locked, deterministic self-play panels with
  distinct seed families, each containing five positions per bag size. The
  contested-position generator retained positions whose quick PEG seed win
  estimate was between 5% and 95%. The second panel was added after the first
  20-position result, not selected for arm disagreement or outcome.
- Execution: one process and one position at a time. Arm order alternated
  between greedy/30/60/120 and its reverse.
- Arms: greedy seed and the same single 2-ply refinement stage of at most 32
  candidates, capped at 30, 60, or 120 seconds. Completion of a candidate,
  rather than elapsed time within an unfinished candidate, is the recorded
  work boundary.
- Judgment: only distinct arm nominees were sent to a direct 3-ply
  pair-conditioned judge. All nominees used one deterministic
  weight-stratified root-scenario sample at stride 4 and a 600-second cap. A
  result was accepted only if every nominee completed. Exact arm agreements
  received zero regret without an oracle. A prospectively selected subset used
  stride 2 as a sensitivity check.
- Utility: win probability + `1e-4 * spread`. Confidence intervals are
  deterministic bag-stratified bootstrap 95% intervals; disagreement-rate
  intervals are Wilson 95% intervals.

The runner records every returned move, candidate completion and timestamp,
candidate total and rank, cumulative scenario count, nested endgame node
count, wall and process CPU time, scheduled-core occupancy, and completed or
partial-stage status.

## Quality

| Step | Disagreement rate (95% CI) | Utility gain (95% CI) | Win gain, percentage points (95% CI) | Spread gain (95% CI) |
| --- | --- | --- | --- | --- |
| Greedy to 30s | 21/40 = 0.525 [0.375, 0.671] | +0.0903 [0.0363, 0.1568] | +8.9703 [3.5794, 15.5935] | +6.3598 [2.9525, 10.2848] |
| 30s to 60s | 0/40 = 0.000 [0.000, 0.088] | 0.0000 [0.0000, 0.0000] | 0.0000 [0.0000, 0.0000] | 0.0000 [0.0000, 0.0000] |
| 60s to 120s | 0/40 = 0.000 [0.000, 0.088] | 0.0000 [0.0000, 0.0000] | 0.0000 [0.0000, 0.0000] | 0.0000 [0.0000, 0.0000] |

Conditional on the 21 greedy-to-30 disagreements, the 30-second arm gained
0.1721 utility [0.0757, 0.2893], 17.0863 win percentage points [7.4964,
28.7927], and 12.1138 spread [6.3416, 18.1185]. There were no conditional
30-to-60 or 60-to-120 disagreements to analyze.

The zero gain intervals for the two longer-budget comparisons describe the
identical nominations observed in this panel. They do not prove a zero
population effect: the Wilson 95% upper bound on either disagreement rate is
8.8%.

All 21 required stride-4 judges completed; none was replaced by a seed or
2-ply value. All 13 stride-2 sensitivity judges also completed. The best
nominee sets agreed in every sensitivity position. Across common nominees, the
median absolute stride-2 versus stride-4 difference was 0.007134 win, 2.146
spread, and 0.007190 utility. One stride-4 tie was handled as a set-valued best
result, rather than as an arbitrary move-order disagreement.

## Completion and portable work

| Arm | Returned | Full requested stage | Partial stage | Median completed candidates | Median scenarios | Median nested nodes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Greedy | 40 | 40 | 0 | 0 | 34,545 | 0 |
| 30s | 40 | 34 | 6 | 32 | 36,663 | 2,997,068 |
| 60s | 40 | 37 | 3 | 32 | 36,663 | 3,017,272 |
| 120s | 40 | 39 | 1 | 32 | 36,663 | 3,007,072 |

The mean/median incremental completed-candidate counts were +1.52/0 from
30 to 60 seconds and +0.85/0 from 60 to 120. The corresponding nested-node
increments were +1,052,561/5,375 and +645,166/4,091. The long right tail is
real even though the median position already completed at 30 seconds. Examples
that retained the same move while buying more completed work include:

- bag 4 `b4-p01`: 7, 15, and 26 of 32 candidates at 30/60/120 seconds;
- bag 2 `x-b2-p05`: 6, 11, and 32;
- bag 3 `b3-p01`: 18, 30, and 32.

Full-stage counts at 30/60/120 seconds were 9/10, 10/10, 10/10 for bag 1;
8/10, 9/10, 10/10 for bag 2; 9/10, 9/10, 10/10 for bag 3; and 8/10, 9/10,
9/10 for bag 4.

## Load monitoring

Identical 8-candidate sentinels ran before, between, and after each panel. The
first run had normalized throughput CV 0.048 and before-to-after drift +11.9%.
Its early segment was flagged: the first sentinel had 0.711 scheduled-core
occupancy, versus 0.790 and 0.803 later. The expansion had CV 0.051, drift
-0.7%, occupancy 0.895/0.819/0.887, and no flagged segment. Sentinels are
summarized per panel because the two panels used different positions.

Wall seconds are therefore only local work-to-time conversions. Structurally
valid quality observations from the noisy segment remain included; strength is
reported in candidate completions, scenarios, and nested nodes.

## Reproduction and artifacts

The committed panels are
`tools/peg_time_calibration/heldout_positions.tsv` and
`tools/peg_time_calibration/heldout_positions_expansion.tsv`. Build and test:

```sh
make -j10 magpie magpie_test BUILD=no_pgo_release
./bin/magpie_test peg
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest \
  tools/test_analyze_peg_time_calibration.py
```

Run and analyze one panel:

```sh
python3 tools/run_peg_time_calibration.py \
  --panel tools/peg_time_calibration/heldout_positions.tsv \
  --output-dir obj/peg_time_calibration/run-20260729
python3 tools/analyze_peg_time_calibration.py \
  obj/peg_time_calibration/run-20260729/records.jsonl \
  --json obj/peg_time_calibration/run-20260729/analysis.json \
  --markdown obj/peg_time_calibration/run-20260729/analysis.md
```

The uncommitted raw artifacts are under
`obj/peg_time_calibration/run-20260729/` and
`obj/peg_time_calibration/run-20260729-expansion/`; combined summaries are
`obj/peg_time_calibration/combined-analysis.json` and
`obj/peg_time_calibration/combined-analysis.md`. The raw JSONL and logs are
about 100 MB in total and remain ignored rather than committed.

## Conclusion

The useful effect in this sample is entering the 2-ply refinement at all:
when it disagreed with greedy, the direct judge strongly preferred it. Longer
budgets improved completion reliability on hard positions but produced no
additional move changes in 40 balanced observations. A further generic random
expansion is unlikely to be the most efficient next experiment. If calibration
continues, the next panel should be selected prospectively for candidate-work
difficulty (without looking at arm moves or oracle outcomes), then repeated
under lower system interference. Until such a panel reveals budget-sensitive
nominations and judged gains, leave live TimeManager policy unchanged.
