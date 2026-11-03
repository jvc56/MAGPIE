# Three-second inference clock pilot

90 game pairs (180 games), 18 concurrent single-compute-thread workers, completed in 49.245 seconds. All workers exited successfully. Both players simulated with the same three-second game clock, including inference and move selection. CSW24 lexicon and leave values, RIT, WIT, bag cycling, 15 candidates and two plies were enabled.

The inference player used static T8, margin 10000, per-rack movegen for one through three unknown tiles and census from four. Exchange inference was allowed only with at most 30 tiles in the pre-exchange bag. Both players reserved 300 ms for emergencies; inference was skipped below 300 ms of remaining search time. The previous three-second inference eligibility threshold would have disabled inference entirely at this clock and was corrected.

| Player | Mean time | p95 time | Maximum time | Mean simulation iterations/game |
|---|---:|---:|---:|---:|
| Softmax | 2.436929 s | 2.574183 s | 2.611293 s | 49,651 |
| No inference | 2.378277 s | 2.537317 s | 2.629014 s | 55,455 |

There were zero clock overruns, deadline stops, static fallbacks or emergency passes. Softmax used 58.7 ms (2.47%) more time per game on average. Both players left some clock unused because games ended before the allocation was exhausted. This pilot supports comparable time use, not exact equality or a guarantee against rare overruns.

Softmax attempted 9.806 inference calls per game and completed 9.678 inference-assisted searches per game. Mean logged inference time was 31.831 ms and maximum was 680.723 ms. It skipped inference for low time on 0.961 turns per game. Softmax performed 10.5% fewer simulation iterations per game, reflecting the opportunity cost being tested.

The 90-pair throughput extrapolates to about 3 h 48 min for 25,000 pairs, subject to sustained machine load and game lengths. This is a timing pilot, not evidence of playing strength: softmax scored 48.33% with a paired normal 95% interval of 43.57–53.09%.

Raw logs, summary, binary hash, settings and reproduction driver are in `data/benchmarks/clock-pilot-3s-20261103/` (ignored benchmark artifacts). Each worker runs `bin/magpie_test infmatch:5:3000:SEED:18:0`, with seeds 2026110300 through 2026110317. The harness aborts before playing a move if accumulated selection time exceeds three seconds. Optimized build and portability check passed.
