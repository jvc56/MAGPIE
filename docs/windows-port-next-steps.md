# Windows port: next steps once CI passes

What to do once the `ci` run for #814 passes. It passed at `dc7972ee`:
https://github.com/jvc56/MAGPIE/actions/runs/38668791241. The run before it, at
`0a3b953e` (https://github.com/jvc56/MAGPIE/actions/runs/38651046488), failed only in
`windows-native-tests`, on writing through a symlink whose target didn't exist yet, which
`dc7972ee` fixes. Any step that uses CI time, changes what is published, or rewrites pushed
history waits for the maintainer's go-ahead.

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

### What the first volunteer check found (15 October)

One volunteer ran the runbook on Windows 10 22H2 (build 19045), x64, i7-1165G7 (4 cores,
8 threads), 7.7 GB of memory, Windows Defender. They tested `0a3b953e` (run `38651046488`),
then re-ran steps 2–4 on `dc7972ee` (run `38668791241`).

- **Passed:** download, `data fetch` (with its byte checks), an ordinary session,
  autoplay determinism (443300 turns in every run, and a CSW21 wordmap byte-identical to
  Linux's), PEG, `stop` from a pipe, a non-ASCII folder, and the `compat` contract tests.
- **Speed:** 20,000-game autoplay took about 22 s on 1 thread and 11 s on 4. A 4-core
  laptop can't settle the 8-thread question in step 3.
- **PEG memory:**

  | Threads | Peak private | Peak working set |
  |---|---|---|
  | 1 | 469 MB | 406 MB |
  | 4 | 1923 MB | 1358 MB |

  The working set grows by about 317 MB per added thread, close to Linux's 330 MB. The
  committed (private) memory grows by about 485 MB per thread, so Windows does seem to
  commit memory up front. That is still for the runner measurement in step 3 to settle.
- **Console:** on Windows 10's console window (conhost, Consolas), the default board was
  unreadable: the fullwidth column letters, the "alt" squares and the `➤` marker drew as
  missing-glyph boxes. `boardtiles primary`, `boardcolumns ascii` and
  `onturnmarker ascii` fixed it. MAGPIE now picks those itself in a console window outside
  Windows Terminal and VS Code (see `cterm_shows_wide_glyphs`).
- **Double-click:** the banner, the offer to download data, and **Press Enter to close
  this window.** all behaved. Line editing (arrow keys) and `stop` worked.
- **Not covered:** the SmartScreen prompt. Twice the extracted `.exe` lost the zip's
  Mark of the Web, so Windows never checked it. Windows Terminal (not installed) and a
  non-ASCII user name were not covered either.

## 6. Follow-ups left out of this PR

- **Precache without a temporary file.** On Windows, files precached in memory are
  served through a temporary file, the plan's fallback. The plan's preferred fix is to
  hand loaders the bytes directly.
- **`data fetch` for test data.** Making it cover test data as well would let it replace
  `download_data.sh` on Linux and macOS, and in the CI data-cache flow.
- **More than 64 CPUs.** On Windows, using more than 64 logical CPUs needs threads spread
  across processor groups.
- **Native Windows threads.** Only if the measurements in step 3 show they are needed.
- **Font-aware board glyphs.** The console check guesses from the terminal, not the font:
  a console window with a font that has fullwidth forms (MS Gothic, say, on Japanese
  Windows) gets the narrow glyphs too. And on Windows 11, a program that Windows hands to
  Windows Terminal as the default terminal isn't given `WT_SESSION`, so it gets them
  as well. Both still draw a readable board.

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
