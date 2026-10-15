# Windows port: next steps once CI passes

What to do once the `ci` run for #814 (`windows-port` at `0a3b953e`) passes. The run is
https://github.com/jvc56/MAGPIE/actions/runs/38651046488. Any step that uses CI time,
changes what is published, or rewrites pushed history waits for the maintainer's go-ahead.

## 1. Check the passes are real

Read the logs, not just the green ticks:

- The native Windows jobs (x64 on `windows-latest`, arm64 on `windows-11-arm`) ran the
  full suite. Their skip lines are only the expected ones: symlinks without privilege,
  and per-process file-size limits.
- The Wine job ran every test.
- In the `ap_rit` shard, the CI step that runs `data fetch`:
  - downloaded the data from MAGPIE-DATA;
  - wrote the tarball's symlinks as copies;
  - built a wordmap on first use;
  - found the data already there on its second run.
- The clang-tidy pass over the Windows code in `src/compat` ran in `windows-cross-compile`.
- The `windows-data` job saved the symlink-free data cache.

## 2. Run the release workflow once by hand

`release-windows.yml`'s first-run smoke test only runs on a tag or a manual run, so it
has not run yet. The test, for both x64 and arm64:

1. starts in an empty folder;
2. runs `data fetch`;
3. runs a scripted session: set a lexicon (building its wordmap), generate, simulate,
   infer, solve an endgame, and print the builders.

A manual run builds and tests but does not publish.

## 3. Take the measurements the plan leaves to real Windows

- **Speed.** One timing run of `tablebench`/autoplay on `windows-latest` and
  `windows-11-arm`, to check nothing is unexpectedly slow. That includes the per-thread
  move generator lookup (`get_movegen`), which goes through library calls under the
  Windows pthreads library (winpthreads).
- **What Wine already shows** (14 October). Static autoplay of 20,000 CSW21 games
  (with the wordmap, same seed and portable target, the same results on both):

  | Threads | Linux | `magpie.exe` under Wine 9.0 |
  |---|---|---|
  | 1 | 20.1–21.1 s | 19.9–20.3 s |
  | 8 | 4.1–4.2 s | 5.0–5.1 s |

  Single-threaded speed is the same. With 8 threads Wine is about 20% slower, which
  points to cross-thread overhead (locks, waits, thread-local lookups), not the move
  generator itself. Wine emulates Windows' locking, so only real Windows can say whether
  that 20% is there too.
- **PEG worker stacks.** A pre-endgame test at the runner's full thread count. Each PEG
  worker thread asks for a 64 MB stack, and Windows might reserve all of that memory up
  front. If it does, the fix is the plan's stage 2 for `cpthread`: native Windows threads
  that reserve their stacks rather than committing them.

## 4. Tidy the PR for review

- Update #814's description with the CI results.
- Ask whether the `command_test` race fix should move out of the `data fetch` commit
  (`1fd703ce`) into its own commit. Doing so needs a force-push.
- Move #814 from draft to ready, when the maintainer says so.

## 5. Hand back what needs a person

- **A checklist for one or two Windows volunteers:**
  - the SmartScreen prompt on first run;
  - double-clicking the `.exe` in Explorer;
  - how the console looks on Windows 10 (conhost) compared with Windows Terminal;
  - running under a user name with non-ASCII characters.
- **The README screenshot.** The plan wants a screenshot of the SmartScreen step for the
  README's Windows section, which needs a real Windows machine.

## 6. Follow-ups left out of this PR

- **Precache without a temporary file.** On Windows, files precached in memory are
  served through a temporary file, the plan's fallback. The plan's preferred fix is to
  hand loaders the bytes directly.
- **`data fetch` for test data.** Making it cover test data as well would let it replace
  `download_data.sh` on Linux and macOS, and in the CI data-cache flow.
- **More than 64 CPUs.** On Windows, using more than 64 logical CPUs needs threads spread
  across processor groups.
- **Native Windows threads.** Only if the measurements in step 3 show they are needed.

## 7. Clean up the CI queue

With the maintainer's OK:

- cancel this session's stale queued runs: the old `win-01` … `win-06` stacked PR
  branches, and the superseded `windows-port` push at `c8ecca32`;
- offer the same for the PEG sessions' superseded pushes
  (`claude/peg1cont-results-vm*`, `claude/peg1-continuation-check`).

## If a job fails

1. Read the job's log.
2. Fix the cause locally.
3. Reproduce it under Wine where possible: `make windows_wine_test TESTS=<test>`.
4. Push all the fixes together in one batch, to keep CI runs down.
