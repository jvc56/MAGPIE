---
name: cycle-profile
description: Cycle-profile MAGPIE (or any native binary) on macOS with xctrace's CPU Profiler, and attribute its cycles to functions, inlined functions and source lines, with checks that catch misattribution. Also finds what the cycles wait on with CPU counters, sampling the branches that mispredict and the loads that miss. Use for any profiling, for before/after comparisons of an optimization, for picking optimization targets, and before quoting profile numbers in a PR. Use its scripts instead of writing new ones.
---

# Cycle profiling on macOS

Profile numbers decide which optimizations get tried and get quoted in PRs, so they must be right. An ad-hoc parser of `atos` output once shifted whole batches of addresses onto their neighbours' functions. It produced plausible profiles, varied between runs of the same work, and was quoted in a PR before anyone noticed. The scripts here check against that, and the rules below catch the rest.

Scripts, in `.claude/skills/cycle-profile/scripts/` from the repository root:
- `dsym.sh`: makes a binary's dSYM and refuses a stale one.
- `record.sh`: records one command and exports its samples.
- `counters_template.py`: writes a template that records CPU Counters in one of Instruments' guided modes.
- `xct.py`: attributes the samples, with these subcommands:
  - `self`: self cycles by innermost inlined function;
  - `compare`: base vs new, over one or more pairs;
  - `lines`: cycles by source line inside a function;
  - `callers`: the inline chains a function's cycles sit in;
  - `contains`: share of cycles with a function on the stack, inlined or not;
  - `inclusive`: real frames only;
  - `check`: looks up every address one at a time;
  - `bottlenecks`: CPU Counters' breakdown of the CPU's issue slots;
  - `pmi`: CPU Counters' samples of mispredicts or misses, by function, line and instruction.

  Run `xct.py` with no arguments for usage.

On Linux, use `perf record` and `perf report`, as AGENTS.md describes; these scripts are for macOS.

## 1. Build and keep symbols

1. Build a release binary: `make magpie_test BUILD=no_pgo_release` (or `magpie`). Release builds carry debug information and align functions to 64 bytes.
2. Copy each binary you'll profile to its own name, such as `mt_base` or `mt_new`.
3. Run `dsym.sh mt_new` right away, before any other build in that worktree.

dsymutil reads the debug information from the object files the binary was linked from, and a later build overwrites them. A dSYM made after that still has the binary's UUID, but it can name the wrong function or line for any code that changed. `dsym.sh` treats any dsymutil warning as stale, because a fresh build gives none.

`xct.py` matches the dSYM to the profiled binary by UUID and refuses a mismatch.

## 2. Record fixed work

- **Run the same fixed work in every profile you'll compare.** A different amount of work changes every share.
- **Single-threaded runs with a fixed amount of work** repeat exactly, so their total cycles are comparable. Examples:
  - endgame: `egspeedbench:cgp=FILE,plies=5,max=10,threads=1,tag=x`;
  - sims: the CLI on one thread with a fixed iteration count and no stopping condition, recorded with `--stdin`. For example, a command file of:
    - `set -lex CSW24 -wmp true -rit true -threads 1 -plies 2 -numplays 8 -it 4000 -scond none -seed 7 -hr true`;
    - `load testdata/gcgs/success_long_game.gcg`;
    - `goto N`, `gen` and `sim` for each N of 6, 10, 14, 18, 22, 26 and 30;
    - `quit`.

    That takes about 5 seconds; raise `-it` for more samples.
- **Time-limited runs are not fixed work.** `simbench` gives each turn 2 seconds, so a faster build runs more iterations, and so does a run that happens to go faster. Compare its shares and its iters/s, never its total cycles. `SIMBENCH_THREADS=1` makes it single-threaded; `record.sh` passes its environment to the command.
- **Multithreaded runs** can vary in work from run to run, for example threads sharing a transposition table. `pegspeedbench1` uses 9 threads, and its totals vary by ±2%.
  - Compare their shares, never their totals, over several runs.
  - For exact pairs, build both sides from a local, uncommitted copy of the benchmark set to one thread. Don't commit benchmark tweaks or add environment variables for them.
- **Even fixed single-threaded work** varies by 1–2% in total cycles from run to run when it is memory-bound: a miss costs more cycles at a higher clock.
  - **Alternate the order** (base, new, new, base) and **run at least two pairs**.
  - Trust a function's own change only when it repeats in every pair.
- **Don't build or run anything heavy while recording.**
- **Record:** `record.sh OUT -- ./mt_new ARGS` from the directory the command needs. MAGPIE needs `data/` there.
  - For the CLI fed on stdin, use `record.sh OUT --stdin CMDFILE -- ./magpie`.
  - The command's output goes to `OUT.out`, for comparing the builds' results.

## 3. Attribute

```
X=.claude/skills/cycle-profile/scripts/xct.py
python3 $X self OUT.xml mt_new.dSYM --top 25
python3 $X compare mt_base.dSYM mt_new.dSYM b1.xml:n1.xml b2.xml:n2.xml
python3 $X lines FUNC OUT.xml mt_new.dSYM
python3 $X callers FUNC OUT.xml mt_new.dSYM --depth 4
python3 $X contains OUT.xml mt_new.dSYM game_gen_classic_cross_set
```

`compare` gives each function's change as a share of its pair's base total, so the changes add up to the total's change. With several pairs it also prints the net of the functions that move the same way in every pair. That net is the figure to quote; one pair's movers are not.

## 4. Find the cause with CPU counters

A cycle profile shows where the time goes, not why: a line's cycles sit on the instruction that waits, which may be after a mispredicted branch or on the consumer of a load that missed. Guessing the cause from the location has led to fixes that couldn't pay. In MAGPIE's sims, a prefetch aimed at lookups that looked memory-bound made sims slower: the counters then showed that mispredicted branches cost about twice as much as all memory and execution stalls together.

Apple silicon's counters say why. xctrace has no option to choose a counter mode, so `counters_template.py` writes a copy of Xcode's CPU Counters template set to one of Instruments' guided modes:

```
C=.claude/skills/cycle-profile/scripts
python3 $C/counters_template.py --list
python3 $C/counters_template.py bottlenecks bottlenecks.tracetemplate
python3 $C/counters_template.py discarded_sampling discarded.tracetemplate
python3 $C/counters_template.py l1d_miss_sampling l1d.tracetemplate
```

`--list` shows the modes this chip supports, and the events each one samples.

1. **Bottlenecks first.** Record with `record.sh OUT --template bottlenecks.tracetemplate -- ...`, then run `xct.py bottlenecks OUT.xml`. It splits the CPU's issue slots into four shares:
   - useful work;
   - discarded work, mostly from mispredicted branches;
   - processing stalls, including waits for memory;
   - delivery stalls, waiting for instructions.

   This says which kind of fix can pay. MAGPIE's fixed sim job on an M2 was 47% useful, 30% discarded, 14% processing and 9% delivery.
2. **Then sample the cause.** Record with the `discarded` or `l1d` template, then run `xct.py pmi OUT.xml mt_new.dSYM`. Each sample stands for 10,000 of its counter's events. Its address is the instruction that caused them: the mispredicted branch itself, or the load that missed. That is the code to change, not where the cycles piled up.
   - Counters are numbered in the mode's trigger order, as `--list` prints it. For `discarded_sampling` they are memory-order flushes, then all mispredicted branches, then conditional ones; for `l1d_miss_sampling`, load misses, store misses, then TLB misses.
   - The samples have no backtraces; use a cycle profile's `callers` for context.
3. **Check a fix with the same sampling.** For fixed work, a counter's sample count measures its events, so comparing the two builds' counts shows whether a change removed mispredicts or only moved them. A branch-free rewrite of MAGPIE's most-mispredicted branch removed that branch's mispredicts, but the new loops mispredicted in its place: the total fell only 4.5%, and timing showed no clear gain.

## 5. Check before you believe or quote a number

- **Repeat profiles must agree.** Two profiles of the same binary and work should agree within about 0.2 points on the big functions. Small inlined helpers can move 0.5 points between runs.
  - If the big ones disagree, suspect the tooling before the code: run `xct.py check` on both. It looks up every address one at a time, about 5 minutes for 5,000 addresses.
- **Built-in checks:**
  - `xct.py` refuses a dSYM whose UUID doesn't match.
  - Every run spot-checks its batched lookups against one-address lookups, including each batch's first address, where the old parsing bug struck.
- **Location isn't cause.** A line's cycles sit on the instruction that waits: a load's consumer, or the branch after a slow compare. A hot line can be a cache miss or store-forwarding stall, not a branch mispredict. Find the cause with CPU counters (section 4) before deciding what the cost is.
- **`sample`** misattributes stalls; use xctrace.
- **`inclusive`** uses xctrace's own frame names, which don't include inlined functions. An inlined function shows up there as part of its caller. Use `contains` for inlined functions.
- **Code layout:** moving code shifts the alignment of hot loops, worth ±0.5% on its own. Release builds align functions, but loops inside them still move.
- **Confirm a speedup by timing** the same fixed work in alternating order and checking identical output, nodes or results. A profile shows where the time went; the timing shows the gain.
- **When quoting a profile in a PR,** name the workload, the pairs, and whether the shares come from fixed work.
