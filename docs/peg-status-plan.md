# PEG status and shpeg: plan

How `status` (while a PEG runs) and `shpeg` (when it is done) show a PEG
solve. Done:
- the three-section layout: summary, the candidates being solved, ranking,
  in `status`, `shpeg` and `peg`, with `shpeg <number>` and `shpeg all`;
- the per-thread table of candidates solved at once;
- the solving tables' endgame columns, `Depth` and `Moves`, with a reason
  in `Depth` when there is no endgame to show;
- solved plays pinned below the top 10;
- plays the cutoff flags on their last draw counted as solved.

What is left: one open choice about the layout. The thread count was
measured: the current behavior stays (see Threads).

## Open choice

- **`Outcomes`.** The ranking's outcomes column follows `-pegoutcomes`,
  which defaults to on. The mockup had none.

## Threads: `-threads N` keeps N + 1 busy

**What happens.** For `-threads 8`, PEG starts a pool of 8 threads
(`peg_pool_create(n_threads, 0)`). The thread that called the solve then has
nothing to do but wait for the pool, so instead of sleeping it takes jobs
from the queue too (`pp_submit_and_wait`, "help-while-waiting"). Nine
threads do PEG work: the 8 pool threads and the caller. The scratch workers
are sized for that (`n_scratch = n_threads + 1`). This is why status shows
"9 candidates at once" and, for the first candidate, "9 running" draws.

**What it costs depends on spare cores.**
- **Fewer threads than cores.** The extra thread runs on an idle core and
  adds real work: `-threads 8` on a 12-CPU machine computes with 9.
- **As many threads as cores.** `-threads` defaults to the core count
  (`get_num_cores()`), so this is the common case. With 13 busy threads on
  12 CPUs, the extra thread adds no compute; the OS only switches threads in
  and out. Measured below: on this machine that costs nothing measurable.

**Decision: keep it as is**, and say so in the `-threads` help: "PEG uses
this many pool threads, plus the calling thread." Done.

**If a machine with uniform cores shows a cost**, the options were:
- **(a) Pool of `threads - 1` when `-threads` is at least the core count.**
  The default stops oversubscribing, and smaller counts keep their extra
  thread. `-threads` then means two things.
- **(b) Always a pool of `threads - 1`.** `-threads` is honest everywhere,
  but a run below the core count loses the extra thread.
- **(c) The caller only waits.** Busy threads are the same as (b), with the
  calling thread idle; no reason to prefer it.

### Evaluation (2026-10-30)

**Question:** with `-threads` equal to the core count, is a PEG solve
slower with cores + 1 busy threads than with cores? `-threads N` keeps
N + 1 busy, so no code change was needed: on this 12-CPU i7-1250U,
`-threads 12` (13 busy, today's default), `-threads 11` (12 busy, what (a)
or (b) would give) and `-threads 10` (11 busy, a noise check).

**Method.**
- `make release` (PGO), `peg` at the default 3 plies with
  `-pegoutcomes false`, timed from peg's `Status` line.
- 24 positions:
  - the 18 deficit positions from `docs/peg-vs-macondo.md` with 1 or 2 in
    the bag that finish in under 60 s;
  - the 6 `example_pegs/` 1-in-the-bag games that finish in under 2 min.
- One solve at a time, each in its own process, 3 runs per setting.
- The order rotated each repeat (12-11-10, 11-10-12, 10-12-11). It matters:
  the first solve of each position's block ran 3.6% faster than the other
  two, likely because the chip was cooler. The rotation gives each setting
  that slot once per position.
- 216 solves in 90 minutes. None hit the 10-minute cap.

**Results.**

| | `-threads 12` | `-threads 11` | `-threads 10` |
|---|---|---|---|
| Busy threads | 13 | 12 | 11 |
| Sum of per-position medians | 458.5 s | 456.6 s | 460.3 s |
| Relative to `-threads 11` | +0.4% | | +0.8% |

- **12 vs 11:**
  - median per-position ratio 1.000 (range 0.992 to 1.333);
  - `-threads 12` slower on 11 of 24 positions.
  - The fastest 12 run was slower than the slowest 11 run on only 2
    positions, by 0.1 s of a 50 s solve (0.2%).
  - The 1.333 is 0.4 s against 0.3 s, which is the timer's resolution.
- **11 vs 10:** median ratio 0.994. One more thread below the core count is
  worth under 1%.
- **Noise:**
  - On solves over 10 s, the three runs of a position spread by up to 18%,
    with a median of 5.5%.
  - Every gap between settings is smaller than that.
- **Answers:** identical in all 216 solves. Every run had the same best
  play, and every play solved in two runs had the same exact W/T/L.

**How much the test could see.** A check of scaling, `-threads 6` (7 busy)
against `-threads 12` (13 busy) on 4 positions, two runs at 6:

| Position | `-threads 6` | `-threads 12` | Speedup |
|---|---|---|---|
| g111791_t20 | 10.0, 10.8 s | 7.8 s | 1.33 |
| g105295_t16 | 28.7, 29.2 s | 23.0 s | 1.26 |
| g109781_t20 | 29.1, 29.1 s | 24.0 s | 1.21 |
| g116234_t17 | 68.5, 68.6 s | 55.3 s | 1.24 |

Nearly twice the threads buys about 1.25 times the speed. The chip has 2
performance cores (4 logical CPUs) and 8 efficiency cores, at 9 W base
power (29 W turbo), so threads near the top add little. On this machine
both a thread's gain and an extra thread's cost are small. A machine whose cores
are all alike, with more power, would test the question harder.

**Reading.** `-threads 12` is within noise of `-threads 11`, so per the
plan the current behavior stays. `-threads 11` is not slower than
`-threads 10`, so there is no separate contention question.

**Still open.**
- **A second machine.** None was available. If a desktop or cloud machine
  with uniform cores becomes available, rerun the method above there with
  `-threads` at its core count, one less, and two less. Adopt (a) only if
  that machine shows the core count consistently slower than one less.
- **Parallel games.** In autoplay, several games can solve PEGs at once,
  each with its share of the threads; a share of 2 keeps 3 busy. That
  follow-up was conditional on a single-solve cost, which did not appear.

**Where candidates are not one per thread**, which status should keep
saying plainly:
- **The tail.** When fewer candidates are left than threads, the count
  falls; in the g104730 run, this took the last 6 to 9 seconds.
- **Small fields.** A field smaller than the thread count (`peg only` with
  a few moves) has fewer at once.
- **The first candidate** is always solved alone, on every thread.
- **No at-once phase.** Candidates are solved one at a time, each over
  every thread, in any stage without the early cutoff: with `-threads 1`,
  and in a time-limited solve's (`-pegtlim`) cascade stages before its
  last. A complete solve always has the cutoff.
- **Resumed solves.** Checkpointed candidates finish at once. After a stop
  or deadline, queued candidates are skipped.
