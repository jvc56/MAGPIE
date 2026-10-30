# PEG status and shpeg: plan

How `status` (while a PEG runs) and `shpeg` (when it is done) show a PEG
solve. Done:
- the three-section layout: summary, the candidates being solved, ranking;
- the per-thread table of candidates solved at once;
- `THREADS` and `MOVES` in the solving tables;
- solved plays pinned below the live top 10;
- plays the cutoff flags on their last draw counted as solved.

What is left: three open choices about the layout, and the thread count,
which needs measuring first.

## Open choices

- **`shpeg` length.** `shpeg` lists every play; `status` shows the top 10
  and the solved plays below them. `shpeg` could do the same.
- **Label case.** The summary labels are capitalized (`Status`, `Phase`,
  ...), to match `Status`. The mockup's were lowercase.
- **`OUTCOMES`.** The ranking's outcomes column follows `-pegoutcomes`,
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
  and out. That may cost something or nothing. It has not been measured.

**Decision: keep it as is**, unless the evaluation below shows that cores +
1 busy threads is measurably slower than cores. Document the behavior in
the `-threads` help: "PEG uses this many pool threads, plus the calling
thread."

**Options, if the evaluation shows a cost:**
- **(a) Pool of `threads - 1` when `-threads` is at least the core count.**
  The default stops oversubscribing, and smaller counts keep their extra
  thread. `-threads` then means two things.
- **(b) Always a pool of `threads - 1`.** `-threads` is honest everywhere.
  A run below the core count loses the extra thread, which is up to about
  1/9 of the compute at `-threads 8` in phases that keep every thread busy.
- **(c) The caller only waits.** Busy threads are the same as (b), but the
  calling thread sits idle; there's no reason to prefer it.

### Evaluation plan

**The question:** with `-threads` equal to the core count, is a PEG solve
slower with cores + 1 busy threads than with cores?

**No code change is needed to measure it.** `-threads N` keeps N + 1
threads busy. So on this machine (12 logical CPUs):

| Run | Busy threads | Stands for |
|---|---|---|
| `-threads 12` | 13 | today's default |
| `-threads 11` | 12 | options (a) and (b) at the default |
| `-threads 10` | 11 | a check on noise: one fewer again |

Two other things differ by one between these runs, with no expected effect:
- the injection monitor's target (`core_target = n_threads`);
- the threads that zero the endgame transposition table.

**Positions.** Use positions that finish in seconds to a few minutes, so
each can be repeated:
- the 1- and 2-in-the-bag deficit positions in `docs/peg-vs-macondo.md`
  that finish in under 60 s: 18 of them, from 1 to 57 s;
- the `example_pegs/` 1-in-the-bag games that finish in under 2 minutes.

Together these cover all three parts of a solve:
- the first candidate solved alone, spread over every thread;
- the at-once phase, one candidate per thread, where every thread is busy;
- the tail, where idle threads join the remaining endgames as helpers.

**Procedure.**
- **Build:** `make release`, the production PGO build.
- **Settings:** `peg` at the default 3 plies, `-pegoutcomes false`. Time
  from the `Status` line of `shpeg`.
- **Serial:** one solve at a time, with nothing else running.
- **Repeats:** 3 runs of each position at each setting, interleaved: 12,
  11, 10, 12, 11, 10, and so on. This spreads thermal and background drift
  across the settings, which matters on this machine. The i7-1250U is a
  laptop chip with mixed performance and efficiency cores, and the extra
  thread may land on either kind.
- **Second machine:** if one is available, repeat on a desktop or cloud
  machine whose cores are all alike. A result that holds on only one kind
  of CPU decides little.
- **Answers:** for every run, compare the best play and the exact
  W/T/L counts with the `-threads 12` run. Only cut plays' bounds may
  differ.

**Measures.**
- **Per position:** the median time at each setting, and the ratios 12/11
  and 11/10.
- **Over all positions:** the median ratio and the total time.
- **Noise:** the spread between a position's three runs. A difference
  smaller than that is noise.

**Reading the result.**
- **`-threads 12` within noise of, or faster than, `-threads 11`:** the
  extra thread costs nothing at the default. Keep the current behavior and
  document it.
- **`-threads 12` consistently slower than `-threads 11`:** slower by more
  than the run-to-run spread, on most positions and on both machines. Then
  the extra thread costs time at the default. Adopt (a), and repeat the
  measurement with the change against `-threads 11` today, which should
  match it.
- **`-threads 11` slower than `-threads 10`:** this would mean more threads
  hurt even below oversubscription, from shared-table contention or memory
  bandwidth. That is a separate question; record it and don't act on it
  here.

**Follow-up: parallel games.** In autoplay, several games can solve PEGs at
once, each with its share of the threads. A share of 2 keeps 3 busy, so the
machine can be oversubscribed by much more than one thread. The thread
flags alone cannot show this, because each solve's share is computed
inside autoplay. Measure it only if the single-solve result shows a cost.
Use a scratch build in which the pool has `threads - 1` workers, and
compare games per hour with PEG on, over the same seed and game pairs.

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
