---
name: cycle-profile
description: Cycle-profile MAGPIE (or any native binary) on macOS with xctrace's CPU Profiler, and attribute its cycles to functions, inlined functions and source lines, with checks that catch misattribution. Also finds what the cycles wait on with CPU counters, sampling the branches that mispredict and the loads that miss, and profiles Metal GPU work (how busy the GPU really is, and GPU time by kernel) with Metal System Trace. Use for any profiling, for before/after comparisons of an optimization, for picking optimization targets, and before quoting profile numbers in a PR. Use its scripts instead of writing new ones.
---

# Cycle profiling on macOS

Profile numbers decide which optimizations get tried and get quoted in PRs, so they must be right. An ad-hoc parser of `atos` output once shifted whole batches of addresses onto their neighbours' functions. It produced plausible profiles, varied between runs of the same work, and was quoted in a PR before anyone noticed. The scripts here check against that, and the rules below catch the rest.

Scripts, in `.claude/skills/cycle-profile/scripts/` from the repository root:
- `dsym.sh`: makes a binary's dSYM and refuses a stale one.
- `record.sh`: records one command and exports its samples.
- `counters_template.py`: writes a template that records CPU Counters in one of Instruments' guided modes.
- `metal_template.py`: writes a Metal System Trace template with the shader profiler on.
- `gpu.py`: GPU busy time (`busy`) and GPU time by kernel (`shaders`) from a Metal System Trace; see section 6.
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

If dsymutil says "no debug symbols in executable", the build has no `-g`, or it links with LTO and the linker deleted the object holding the debug information. Main's Makefile does both (`-gdwarf-4` and `-object_path_lto`), but branches cut before it may not. On such a branch, override both on the command line rather than editing the Makefile:

```
make magpie_test BUILD=no_pgo_release -j \
  cflags.no_pgo_release="-O3 -flto -march=native -DNDEBUG -Wall -Wno-trigraphs -g" \
  cflags.test_no_pgo_release="-O3 -flto -march=native -Wall -Wno-trigraphs -g" \
  ldflags.no_pgo_release="-pthread -flto -Wl,-object_path_lto,$PWD/lto.o"
```

Copy the flags from the Makefile's own lines, adding only `-g` and `-object_path_lto`, so the code is the one you mean to profile. Touch the sources first if they were built without `-g`. `lto.o` is overwritten by the next such link, so run `dsym.sh` before it.

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
- **Disk space:** while it records, xctrace writes a raw trace to the user temp folder (`getconf DARWIN_USER_TEMP_DIR`), as `instruments*.ktrace`. A CPU Counters recording's can pass 25 GB. xctrace deletes it once the `.trace` is saved, but not after a failed or interrupted recording, and leftovers once filled this disk. `record.sh` deletes any its recording leaves, stops xctrace if the script is interrupted, and refuses to start with less than 20 GB free there.
  - Delete a recording's `.trace` once its numbers are in a PR; CPU Counters bundles run to gigabytes too.

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

## 6. GPU work (Metal and MPSGraph)

When a program runs work on the GPU, find out first whether the GPU or the CPU limits it. Time spent inside a synchronous GPU call can't tell you: MPSGraph's `runWithMTLCommandQueue` blocks while the CPU encodes the graph, while the call waits behind others, and while the GPU runs. In MAGPIE's cached value net, slots looked busy all of the time by that measure while the CPU was also doing 8 cores of work, and either could have been the limit.

1. **How busy the GPU really is.** Record the fixed work with the stock template, then take the union of the process's GPU intervals:
   ```
   C=.claude/skills/cycle-profile/scripts
   $C/record.sh OUT --template 'Metal System Trace' -- ./mt_new ARGS
   python3 $C/gpu.py busy OUT.gpu.xml mt_new --skip 3
   ```
   `--skip` leaves out the start, when MPSGraph compiles each new shape; a first call can take hundreds of milliseconds.
   - Near 100% of the window: the GPU limits throughput, and cutting CPU work can gain at most the idle share. Cut the GPU's work instead.
   - Well below: the GPU waits on the CPU; the gaps' sizes say whether it waits on every call (small, regular gaps) or on stalls.
2. **What the GPU spends its time on.** MPSGraph puts a whole graph in one compute encoder, so encoder labels don't name kernels. Record with the shader profiler on:
   ```
   python3 $C/metal_template.py shaders.tracetemplate
   $C/record.sh OUT --template shaders.tracetemplate -- ./mt_new ARGS
   python3 $C/gpu.py shaders OUT.shaders.xml mt_new --skip 3
   ```
   It gives each kernel's share, such as `gemm_*` for matrix multiplies, `sdpa_*` for fused attention and `reduce_*` for layer norms and softmax sums. The shares are what count: the profiler covers only part of the GPU time, and kernels overlap. A kernel's name can show it took a slow path; attention on sequence lengths that are not a multiple of 16 runs `sdpa_tile_fwd_16x16x16_doEdgeCheck`, otherwise `..._noEdgeCheck`.
3. **The CPU side** is an ordinary cycle profile (sections 1–3) of the same run. With many threads, compare shares, and compare total cycles only for the same fixed work. MPSGraph's own CPU work shows as `<other: MetalPerformanceShadersGraph>`, `MPSNDArray`, `MPSCore` and `libobjc`.
4. **Shapes.** MPSGraph compiles a graph for each new combination of input shapes. An input dimension that varies from call to call, such as a count of candidates, makes it compile over and over; GPU time per call jumps several times, and nothing else in the profile looks wrong. Round every varying dimension up to a step.
5. **GPU counters** (occupancy, limiters, utilizations). `metal_template.py OUT --counters 'Performance Limiters'` selects a counter set; `--list` shows the sets and their numbers, which come from Instruments' own table. A trace shows the set it used in `xctrace export --input OUT.trace --toc` as `Counter Set:`. The values land in `metal-gpu-counter-intervals` (and `gpu-counter-value`), one row per counter per sample; `gpu-counter-info` names the counters sampled.
   - On an M5 Max (AGXAcceleratorG17X) with Xcode 27.2, every set is accepted (the trace says `Counter Set: Performance Limiters`), but xctrace samples only "RT Unit Active", whose value stays 0. Check `gpu-counter-info` before reading anything into the values; on this Mac, use the shader breakdown instead.
   - The script also sets `counterdevice` to the GPU's IORegistry id (`ioreg -r -c IOAccelerator -d 1`), as Instruments' Game Performance template does; the stock Metal System Trace template leaves it 0. On the M5 Max, the sampled counters were the same either way.
6. **Confirm by timing**, as for CPU changes: alternate the builds, and expect run-to-run noise of ±10% on a laptop GPU, which drifts down as it warms.

