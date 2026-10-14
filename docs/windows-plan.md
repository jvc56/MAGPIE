# Windows port plan

Status: proposal, October 2026. Nothing in this document has been implemented
yet. The maintainer decisions are settled; section 9 records them.

## Summary

- **Toolchain.** Build `magpie.exe` with MinGW-w64, using the clang-based
  [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) with the UCRT runtime.
  It cross-compiles from Linux and macOS and is linked statically, so the result
  is one self-contained `.exe` with no DLLs to install. Builds ship for x64 and
  arm64 from the first release. Native MSVC is out of scope.
- **The hard part.** Making it compile is small. With MinGW, only eight source
  files outside the vendored ones fail to compile: they use `mmap`, `glob`,
  POSIX regex, `fmemopen`, `getline`, two-argument `mkdir`, `poll`/`pipe`, and
  `lstat`/`readlink`. The bigger risk is code that compiles but gives wrong
  results on Windows:
  - Every binary lexicon is opened in text mode.
  - Every CRLF text file fails a size check.
  - `rename` will not replace an existing file.
  - The data path list is split on `:`, which also appears in `C:\`.
- **Code layout.** Every platform difference goes into `src/compat`, and a new
  CI lint enforces that. Outside `src/compat`, code may include only ISO C
  headers (plus `<stdatomic.h>`). This extends a rule `src/compat/cfile.h`
  already states: "nothing outside src/compat tests platform macros".
- **Testing without a Windows machine.** Six layers, all of which run in CI:
  1. The portability lint.
  2. A MinGW cross-compile with `-Werror` on every push.
  3. Contract tests for each compat module, run on every OS.
  4. The cross-compiled test binary run under Wine on Linux.
  5. The same `.exe` files run on GitHub's hosted Windows runners (x64 and
     arm64), which are real Windows.
  6. A smoke test of each release `.exe` in an empty directory.

  Nobody on the team needs a Windows machine.
- **For users.** They get one `.exe`. Double-clicking it opens a console that:
  - finds its data, or downloads it with a built-in command;
  - stays open long enough for any error to be read;
  - runs the same REPL as on Linux and macOS.

  Windows users use MAGPIE for the same things Linux and macOS users do:
  analysis, simulation, inference, endgames, autoplay, and `contribute` if they
  choose to. There is no Windows-specific mode or feature set.

## 1. Constraints this plan is built around

1. **Non-developers download one `.exe` and open it.** No compiler, no bash, no
   `download_data.sh`, no WSL. Today a double-clicked `magpie` with no `data/`
   next to it prints `(error 154) file 'standard15' not found`, exits with
   status 0, and its window closes at once.
2. **MAGPIE's developers do not have Windows machines.** Every Windows check
   must run in CI or on Linux/macOS, and real Windows runs must be automated.
3. **Windows is a full platform, not a special case.** Every command behaves
   on Windows as it does on Linux and macOS, and the release gates (section 5)
   cover ordinary use first: loading a lexicon, generating moves, simulating,
   solving an endgame. `contribute` is one command among many.
4. **`src/` outside `src/compat` stays platform-agnostic.** Anything that does
   not work on Windows goes behind a compat wrapper. Section 3.4 covers the GCG
   regex, which is removed rather than wrapped.

## 2. What we found

### 2.1 Trial cross-compile

Every `.c` file in `src`, `cmd`, `tools` and `test` was compiled with
`x86_64-w64-mingw32-gcc` 13 (Ubuntu 24.04 package) using `-fsyntax-only`.
Appendix B has the reproduction steps.

- **First pass: 140 of 203 files fail.** Almost all of them fail for one reason:
  `src/ent/rack_info_table.h` includes `<sys/mman.h>`, and nearly every file
  includes that header.
- **Second pass: empty stand-ins for the missing headers.** This shows what is
  really missing. Outside `test/`, the only failures are:

| File | Missing on Windows |
|---|---|
| `src/ent/rack_info_table.h` | `mmap`, `munmap`, `madvise`, `PROT_*`, `MAP_*` |
| `src/compat/atomic_file.h` | `lstat`, `readlink`, `S_ISLNK`, `fchmod`, `O_CLOEXEC` |
| `src/compat/async_command_control.h` | `pipe`, `poll`, `struct pollfd` |
| `src/compat/linenoise.c` | `termios`, `ioctl(TIOCGWINSZ)` |
| `src/ent/data_filepaths.c` | `glob`, `globfree`, `glob_t` |
| `src/impl/gcg.c` | `regcomp`, `regexec`, `regerror`, `regfree` |
| `src/util/fileproxy.c` | `fmemopen` |
| `src/util/io_util.c` | `getline` |
| `src/impl/config.c` | `mkdir(path, mode)` (MinGW's `mkdir` takes one argument) |

Everything else compiles under MinGW, because MinGW ships POSIX emulation:
- winpthreads for `pthread_*`;
- `clock_gettime` and `nanosleep`;
- `<dirent.h>`, `<unistd.h>` and `strcasecmp`.

That emulation works, but it is still platform-specific code sitting outside
compat. Section 3 moves it behind wrappers anyway, so that MSVC or a native
Win32 backend stays possible and the lint has a simple rule to enforce.

### 2.2 Code that compiles but behaves wrongly

These are the real risks. None of them shows up as a compile error.

| Problem | Where | Effect on Windows |
|---|---|---|
| Binary data opened in text mode | `src/util/fileproxy.c:59` opens every data file with `"r"`; KWG, KLV2, WMP, RIT, WIT and DAWG all load through it | `\r\n` becomes `\n` and byte `0x1A` ends the file early. Lexica load corrupted or truncated. |
| Binary written through a text-mode file descriptor | `atomic_file.h` calls `open()` without `O_BINARY`, then `wmp.h:981` and `word_info_table.c:300` call `fdopen(fd, "wb")` | Probably corrupts WMP and WIT writes (each `\n` becomes `\r\n`). Not yet confirmed on Windows. |
| File size compared with bytes read | `io_util.c:463-478` | In text mode every CRLF file reads short, which is reported as "error while reading file". MAGPIE writes its own text files with `"w"`, so on Windows they are CRLF. It then cannot read back `settings.txt`, `contribute.txt`, the contribute manifest (fails silently, so eviction stops), win-pct tables or GCGs. |
| `rename()` over an existing file | `io_util.c:691`, `wmp.h:1003`, `word_info_table.c:322`, `contribute_tables.c:246`, `config.c:11189` | Fails once the target exists, so every atomic rewrite after the first one fails. `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` alone is not enough: it also fails while any handle has the target open without `FILE_SHARE_DELETE`, which the CRT's `fopen` never passes, and the RIT stays mapped for the life of the process. |
| Data path list split on `:` | `data_filepaths.c:117,190,227`, `contribute_tables.c:666`, `xs_bits.c:291`, `convert.c:375` | `-path C:\magpie\data` is split into `C` and `\magpie\data`. |
| No `.gitattributes` | repo root | Git for Windows checks tracked text files out as CRLF by default. This does not reach the data: `data/` and `testdata/` are gitignored and come from tarballs. It affects tracked fixtures and scripts that Windows CI jobs check out. |
| Symlinks in the data set | 23 links, e.g. `data/lexica/CSW24_super21.klv2 -> CSW21_super21.klv2`, `data/strategy/*_super21.pat` | Windows can't create them without developer mode or elevation. Extraction and cache restore either fail or leave the files missing. |
| `%lu`/`%ld` used for `uint64_t` | `klv_csv.c:53` (a data file), `autoplay_results.c`, `sim_string.c`, `inference_string.c`, `autoplay.c:1398,1405` | `long` is 32 bits on Windows, so counts are printed truncated. |
| `long` file offsets | `fseek_or_die`, `ftell` in `io_util.c`, `kwg.h:148`, `word_info_table.c:379`; 32-bit `st_size` in `cfile.c`, `rack_info_table.h` | The CSW24 RIT is 1.89 GB, about 260 MB below 2^31. A larger RIT would break. |
| Main-thread stack | linker default | 2 MB on MinGW (1 MB on MSVC), compared with 8 MB on Linux. PEG, endgame and BAI recurse on the calling thread. |
| PEG worker stacks | `peg_pool.c:346`, 64 MiB each | Linux commits stack lazily. Windows may commit it up front unless the thread is created with `STACK_SIZE_PARAM_IS_A_RESERVATION`. |
| Console | ANSI colors (`bonus_square.h`, `heat_map.h`, `game_string.c`) and UTF-8 tiles | Shows escape-code garbage and mojibake unless VT processing and the UTF-8 code page are turned on. |

Two bugs found during the audit affect every platform. Fix each on its own,
separate from the port.

- `cpthread_cond_timedwait_loop` (`src/compat/cpthread.h:64`) tests
  `while (!done)`, which checks the pointer instead of `*done`. Its only callers
  are five sites in `bai_test.c` and `sim_test.c`, which therefore never wait.
  Fixing it may expose hangs or flakiness the broken wait was hiding.
- `reorder_pair_cmp` (`src/impl/kwg_maker.c:904`) compares only `key`, so
  entries with the same key and different `head` tie. The greedy pass that
  follows keeps the first equally sized superset in each run, so the KWG layout
  depends on how the C library's `qsort` orders ties, which the C standard
  leaves unspecified and which differs between glibc and UCRT. KLV builders go
  through the KWG maker, so this alone would make Windows-built KLVs fail
  contribute's hash checks. Breaking ties on `head` ascending reproduces a
  stable sort's output exactly. The other `qsort` comparators need the same
  audit (`peg_rank_cmp` returns 0 on equal keys, for example).

The derived-table builders (KWG, WMP, WIT, RIT) do no floating-point arithmetic
(`double` in `wmp_maker.c` means double-blank), so floating point is not a
source of cross-platform byte differences there.

A search for other 64-bit `long` hazards (`__builtin_*l`, `1UL <<`, `strtoul`
on 64-bit values, `clock()`, `rand()`, `long double`) found none. Seeds are
parsed with `strtoull`.

### 2.3 Already in place

- `src/compat/chttp.c` has a WinHTTP backend, but it has never been compiled or
  run on Windows.
- `src/compat/cfile.c` has Windows branches for file locks, `cfile_open`,
  `localtime` and free disk space.
- `src/compat/memory_info.h` and `src/compat/endian_conv.h` already handle
  Windows.
- `portable_release` (`-march=nehalem`) exists for distributed binaries. A
  Windows build should use the same target.

## 3. The compat boundary

### 3.1 The rule

Outside `src/compat`:
- **Includes:** only ISO C99 headers plus `<stdatomic.h>`. No `<unistd.h>`,
  `<sys/*.h>`, `<pthread.h>`, `<dirent.h>`, `<fcntl.h>`, `<strings.h>`,
  `<glob.h>`, `<regex.h>`, `<windows.h>`, and so on.
- **Platform macros:** no tests of `_WIN32`, `__APPLE__`, `__linux__`,
  `__wasm__` or `__EMSCRIPTEN__`.
- **Banned calls:** POSIX functions with a compat wrapper may not be called
  directly. The list includes `rename`, `access`, `stat`, `mkdir`, `opendir`,
  `isatty`, `fileno`, `getline`, `strcasecmp`, `gettimeofday`, `nanosleep`,
  `localtime`, `popen`, `fdopen`, `open`, `close` and `pthread_*`.

**Enforcement:** a new `check_portability.py` runs in CI next to
`find_circ_deps.py`. It parses includes and matches banned identifiers as call
tokens, skipping comments and strings, rather than plain grep: names like
`open`, `close`, `stat`, `access` and `rename` also occur as struct fields and
inside other names. It starts with an allowlist of today's violations, and each
PR in section 7 removes entries until the allowlist is empty. Reviewers then
don't have to check the rule by hand.

Wrappers are `static inline`, or plain functions on cold paths. On Linux and
macOS they compile to exactly the calls made today, so there is no performance
cost there. One warm path does touch them: `get_movegen`
(`src/impl/move_gen.c:141-142`) calls `cpthread_once` and
`cpthread_getspecific` on every call, which under winpthreads are library calls
rather than glibc's inline fast path. The `cpthread` PR measures it on Windows.

### 3.2 Compat modules

| Module | Replaces | POSIX backend | Windows backend |
|---|---|---|---|
| `cfile` (extend) | `rename`, `access`, `stat`/`S_ISDIR`, `mkdir`, `fdopen`, `ftell`/`fseek`, `getline`, and the reader-side `fopen` of every data file | Today's calls, plus `fseeko`/`ftello` | Readers open with `CreateFileW(..., FILE_SHARE_READ \| FILE_SHARE_WRITE \| FILE_SHARE_DELETE)` then `_open_osfhandle` + `_fdopen`, so an open reader does not block a replace. Commit with `SetFileInformationByHandle(FileRenameInfoEx, FILE_RENAME_FLAG_POSIX_SEMANTICS \| FILE_RENAME_FLAG_REPLACE_IF_EXISTS)`, which gives POSIX rename semantics on NTFS; fall back to `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING \| MOVEFILE_WRITE_THROUGH)` on other file systems. A mapped file may still refuse replacement; L4 confirms, and callers treat "in use" as retry-later (section 8). Also `_waccess`, `_wstat64`, `_wmkdir`, `_fseeki64`/`_ftelli64`; a portable `getline` written in plain C |
| `cfile` (fold in `atomic_file.h`) | `lstat`, `readlink`, `mkstemp`, `fchmod`, `open` | Today's code | `CreateFileW` for the temp file, with `_O_BINARY`; resolve the target with `GetFinalPathNameByHandleW`; `MoveFileExW` to commit |
| `cdir` (new) | `opendir`/`readdir`, `glob` | `dirent` | `FindFirstFileW`/`FindNextFileW` |
| `cmap` (new) | `mmap`, `munmap`, `madvise` in `rack_info_table.h` | `mmap(PROT_READ, MAP_PRIVATE)` + `madvise` | `CreateFileMappingW` + `MapViewOfFile` (FILE_MAP_READ); `madvise` becomes a no-op |
| `cmemfile` (new) | `fmemopen` in `fileproxy.c` | `fmemopen` | Recommended: change the precache API to hand loaders a byte span rather than a `FILE *` (wasm benefits too). Fallback: a delete-on-close temp file (`"w+bD"`). |
| `cpath` (new) | `:` path lists, `/`-only basename/dirname, executable directory | `:` separator; `readlink("/proc/self/exe")` / `_NSGetExecutablePath` | `;` separator (`:` still accepted when it is not a drive letter); `GetModuleFileNameW`; `/` and `\` both treated as separators |
| `cpthread` (extend) | raw `pthread_*` and `PTHREAD_MUTEX_INITIALIZER` in `io_util.c`, `fileproxy.c`, `peg_pool.c`, `move_gen.c`, `bai_sched_stats.c`; `cpthread_defs.h` moves into compat | pthreads | Stage 1: winpthreads, linked statically. Stage 2, only if PEG stack commit or performance requires it: native SRWLOCK / CONDITION_VARIABLE / `_beginthreadex` with `STACK_SIZE_PARAM_IS_A_RESERVATION`. Adds `CPTHREAD_MUTEX_INITIALIZER`, and a `cpthread_cond_timedwait` that returns the timeout as a status instead of calling `log_fatal` (the PEG watchdog needs that). |
| `ctime` (extend) | `gettimeofday` (`config.c`), `nanosleep` (`peg.c:2818`), `localtime` | Today's calls | `QueryPerformanceCounter`, `timespec_get`, `Sleep`, `localtime_s` |
| `cterm` (new) | `isatty(fileno())` (`exec.c:353`, `game_string.c:50`); console setup; "launched by double-click" detection; a hook `log_fatal` calls before `abort()` | `isatty`; the hook is a no-op | `_isatty`; at startup, `SetConsoleOutputCP(CP_UTF8)`, `SetConsoleCP(CP_UTF8)` and `ENABLE_VIRTUAL_TERMINAL_PROCESSING`; `GetConsoleProcessList` to detect a console the process owns. `abort()` skips `atexit`, so the wait-before-close after a fatal error runs from the `log_fatal` hook. |
| `async_command_control` (rewrite) | `pipe` + `poll` on stdin | Unchanged | One stdin reader thread for the life of the process feeds a queue, and both the REPL and async control read lines from it. The main thread waits on one event for "input ready or finished". A per-command reader like today's cannot work: a thread blocked in `fgets` or `ReadFile` can't be woken, so joining it after the command hangs, and leaving it running swallows the user's next REPL line. This changes the input flow in `exec.c` (`execute_command_async_queueing` creates and joins the input thread per command), not just the compat backend. A console handle signals on mouse and focus events too, so waiting on it directly is unreliable. |
| `linenoise` (Windows branch) | `termios` line editing | Unchanged | `ReadConsoleW` in console cooked mode, converted to UTF-8; plain `fgets` when stdin is not a console. Cooked mode already provides arrow-key history, F7 and editing, so a raw-mode port is not needed. `fgets` on a console can return NULs for non-ASCII UTF-8 input on older Windows builds, and racks and words in non-English lexica need it. |
| `cstring` (or plain C in `string_util.c`) | `strcasecmp`, `strncasecmp` | — | MAGPIE only compares ASCII, so a 10-line portable version can replace both everywhere. |

### 3.3 Fixed in shared code, no compat needed

- **Binary mode everywhere.** `fileproxy.c` opens with `"rb"`. Every data-file
  open names `b` explicitly. Text readers keep tolerating `\r`, as
  `getline_ignore_carriage_return` already does.
  - `get_string_from_file_handle` reads in binary mode and drops any
    `\r` that comes before a `\n`, instead of comparing `ftell` with `fread`.
  - Writers use `"wb"`, so files MAGPIE writes are byte-identical on every OS.
    That matters because contribute and birdtest hash them.
- **`.gitattributes`.** `* text=auto eol=lf`, with binary fixture extensions
  marked `binary`. This is hygiene for tracked files that Windows CI jobs check
  out; the data itself is gitignored and arrives in tarballs.
- **Deterministic sorts.** Every `qsort` comparator breaks ties on a stable
  field, starting with `reorder_pair_cmp` in `kwg_maker.c` (section 2.2), so
  output never depends on the C library's sort.
- **Format specifiers.** `%lu`/`%ld` on 64-bit values become `PRIu64` and
  `%zu`. Add `__attribute__((format(printf, ...)))`, through a compat macro, to
  `get_formatted_string`, `log_*` and `error_stack_push`'s formatter, so
  `-Wformat` catches the next one on Linux. Expect the attribute to turn up a
  wave of fixes on Linux, so it gets its own PR.
- **Delete dead code.** `get_process_output` and `popen_or_die` have no
  callers. `<complex.h>` in `math_util.h` is unused.
- **Main-thread stack.** Link the Windows build with
  `-Wl,--stack,8388608` to match Linux.

### 3.4 GCG regex

`gcg.c` uses POSIX `regcomp`/`regexec` with about 25 fixed patterns that look
like `>([^[:space:]]+):[[:space:]]+...`. Windows has no `<regex.h>`. Two
options were considered.

- **(a) Chosen.** Replace the patterns with a small hand-written GCG line
  tokenizer, in plain C with no platform dependency. This removes the dependency
  instead of wrapping it. To prove the replacement correct on Linux, keep the
  regex path for one PR, behind a test-only build, and have a test run both
  tokenizers on every line of every GCG in `testdata/` plus a set of edge cases.
  The test asserts identical tokens and captures, then the regex path is
  deleted.
- **(b) Rejected.** Vendor a POSIX regex implementation (musl's, MIT, about
  3,500 lines) as `src/compat/cregex.c`, and use it on every platform so
  behaviour is identical everywhere.

Why (a):
- **Less code to own.** The tokenizer is a few hundred lines of plain C. A
  vendored regex is about 3,500 lines of someone else's code that would have to
  pass cppcheck and clang-tidy, or be excluded from them.
- **No second implementation.** A vendored regex only behaves exactly like
  Linux's if Linux uses it too, which replaces a working system library for
  every user.
- **Provably equivalent.** The differential test shows on Linux, before regex
  is removed, that the tokenizer produces the same tokens and captures over
  every GCG in `testdata/`.
- **Speed is not a factor.** GCG parsing is not a hot path.

## 4. Toolchain and build

- **Compiler: llvm-mingw, pinned version, UCRT.**
  - It uses the same compiler family as `make release` (clang PGO).
  - It targets UCRT, which handles C99 `printf` (`%zu`, `%lld`), UTF-8 locales
    and `_set_fmode`. Ubuntu's `gcc-mingw-w64` targets the older msvcrt.
  - It targets both x86_64 and aarch64 Windows from the same toolchain, and
    both ship from the first release.
  - It runs on Linux, macOS, or in a pinned Docker image.
- **Makefile.**
  - Detect a Windows target from `$(CC) -dumpmachine` containing `mingw`.
  - For that target:
    - output `bin/magpie.exe`;
    - link `-static -lwinhttp` (plus whatever else compat ends up needing);
    - drop `-ldl`;
    - add `-Wl,--stack,8388608`;
    - use `-municode` only if `wmain` is adopted.
  - New targets:
    - `make windows` runs `make magpie BUILD=portable_release
      CC=x86_64-w64-mingw32-clang` inside the pinned llvm-mingw image, so any
      developer can build the `.exe` locally.
    - `make windows_test` does the same for `magpie_test.exe`.
    - `WINDOWS_ARCH=aarch64` selects `aarch64-w64-mingw32-clang` for either
      target.
- **Manifest and resources** (embedded with `windres`):
  - `activeCodePage = UTF-8`, so narrow `fopen` and paths handle non-ASCII user
    names.
  - `longPathAware = true`.
  - A version resource and an icon.
  - With the UTF-8 code page, the narrow-character API works and compat does
    not need `wchar_t` plumbing beyond what chttp already does. It requires
    Windows 10 1903 or later.
- **CPU target.** `portable_release` (nehalem, the same ISA as x86-64-v2), as
  for contributor binaries. The arm64 build uses
  `PORTABLE_MARCH=armv8-a`, a baseline every Windows on ARM device meets; a
  later PR can raise it after measuring. birdtest compares derived-file SHA-256s by build
  target. The Windows build must produce byte-identical WMP/RIT/WIT for the
  same target, and section 5 tests that.
- **PGO.** Not for the first release. llvm-mingw supports `-fprofile-instr-*`,
  and a later PR can train on a `windows-latest` runner, or under Wine if the
  timings turn out representative.
- **Static analysis.** `tidy.sh` already sets `-U_WIN32`. Add a second
  clang-tidy pass, and a cppcheck pass with `-D_WIN32`, over `src/compat` only,
  so the Windows branches get the same linting as the POSIX ones.

## 5. Testing without a Windows machine

Each layer catches a different class of bug. All of them run in GitHub Actions,
where Windows runners are free for public repositories.

| Layer | Where it runs | What it catches | Cost |
|---|---|---|---|
| **L0** Portability lint (`check_portability.py`) | Linux, the `lint-fast` job | POSIX includes and calls leaking out of compat | Seconds |
| **L1** Cross-compile `magpie.exe` and `magpie_test.exe` with `-Werror`, for x64 and arm64 | Linux, llvm-mingw container | Compile and link breaks on Windows, on every push | About 2 minutes per architecture |
| **L2** Compat contract tests (`test/compat_test.c`), using fixtures with `\r\n` and `0x1A` bytes | Every OS that runs tests | That each compat function behaves the same everywhere; listed below | Seconds |
| **L3** x64 `magpie_test.exe` under Wine | Linux (`wine64` from Ubuntu 24.04) | Most runtime issues: text mode, path parsing, threads, WinHTTP plumbing. Reproducible locally with `make windows_test && wine bin/magpie_test.exe <test>`. Starts as `compat_test` only, as soon as the first Windows backend lands, and grows to the full suite once the POSIX-only test tricks are ported. | Like a dev shard |
| **L4** Native Windows: download the L1 `.exe` artifacts, restore the data cache, run the test suite (no-PGO shards): x64 on `windows-latest`, arm64 on `windows-11-arm` | Real Windows Server 2022/2025 and Windows 11 on ARM | What Wine gets wrong: file-sharing and delete semantics on open or mapped files, Defender interference, real WinHTTP/Schannel TLS, stack commit, console behaviour | Like the no-PGO release job |
| **L5** Release smoke test: run each release `.exe` in an empty directory on its native runner | Real Windows | The first-run path: `data fetch` into an empty directory, a WMP built on first use, then a scripted session of ordinary commands (set a lexicon, generate moves, sim, infer, endgame), plus `contribute` against a birdtest staging or stub server and `builders` hashes | About 1 minute |

**Contract tests in L2:**
- binary round-trip;
- rename-over-existing;
- an atomic file write while a reader holds the target open;
- path lists with drive letters;
- listing directories;
- mapping a file and reading it back;
- `cterm` with stdin redirected.

**Windows runner setup.**
- The data cache is saved on Linux. Restoring it on a Windows runner needs
  `enableCrossOsArchive: true` in `actions/cache`, and the data's symlinks must
  become copies (section 6.2) or the restore leaves files missing.
- `log_fatal` ends in `abort()`, and an unattended abort can raise a Windows
  Error Reporting dialog that hangs the job. The test binary calls
  `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)` and
  `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)` at startup.

**Hashes.** `builderhash` already pins the SHA-256 of WMP, RIT, WIT and KLV
built from `testdata/`. Running it in L3 and L4 directly checks that Windows
builds produce bytes birdtest will accept.

**Tests that use POSIX-only test tricks** need a Windows equivalent or an
explicit skip through a test-side helper. They are not deleted. Today they are:
- `SIGALRM`/`alarm`/`setjmp` in `test/test_util.c`;
- `setrlimit(RLIMIT_FSIZE)`/`SIGXFSZ` in `word_map_embedding_test.c` and
  `contribute_tables_test.c`;
- `mkfifo` in `command_test.c`;
- `symlink`, `chmod`, `mkdtemp`, `utime` and hard-coded `/tmp`;
- the glibc-only `"e"` fopen flag in about 30 test sites, which UCRT rejects with
  an abort.

Planned replacements:
- The timeout becomes a watchdog thread.
- Disk-full injection becomes a test hook in the writers, which also makes
  those tests portable to macOS.
- Temp directories go through a compat helper.

**Not covered by CI:**
- SmartScreen and antivirus reactions to the downloaded file;
- double-click behaviour in Explorer;
- how the console looks on Windows 10 conhost compared with Windows Terminal.

Before the first public release, ask one or two Windows-using volunteers to
run a checklist covering everyday use. A Windows 11 VM image from Microsoft
works too, if anyone has an x86 machine with virtualization.

## 6. What the user gets

### 6.1 First run

Double-clicking `magpie.exe`:

1. **Console.** `cterm` turns on UTF-8 and VT colours. If the process owns its
   console (it was double-clicked, not run from a terminal), it prints a
   welcome banner. It also waits for Enter before closing after a fatal error
   (through the `cterm` hook in `log_fatal`, since `abort()` skips `atexit`), so
   the window doesn't vanish. Otherwise a no-argument launch opens the normal
   REPL, exactly as on Linux and macOS.
2. **Data.** MAGPIE looks for `data/` in the working directory, then next to
   the executable, then in `%LOCALAPPDATA%\MAGPIE\data`. The working directory
   stays first so Linux and macOS behaviour does not change (a stray
   `bin/data` must not shadow `./data`). The executable-relative location is
   new; it comes from `cpath_executable_dir` and helps when the `.exe` is run
   from a terminal elsewhere. When launched from Explorer, the working
   directory is already the `.exe`'s folder.
3. **Missing data.** It offers to run `data fetch` (see 6.2) rather than failing
   on `standard15`.
4. **WMP.** No `.wmp` is downloaded. On the first use of a lexicon with no
   `.wmp`, MAGPIE builds it locally, with a progress message, and then loads it.
   contribute's derived-table code already does this. MAGPIE runs with
   `-wmp false` and prints one line saying why when:
   - there isn't room, using the disk check that code already has;
   - the lexicon has no WMP support. `convert_lexica.sh` skips OSPS ("Polish
     not supported") and every `_super` lexicon ("21x21 doesn't work with
     wmp"), and first use must skip the same ones;
   - the data directory that holds the lexicon is not writable.
   - Today `-wmp` defaults to true and a missing `.wmp` is an error, so this
     changes behaviour on every platform.
   - The PR that adds it measures how long the build takes for CSW24 on a
     `windows-latest` runner. The progress message has to set expectations
     accordingly.

### 6.2 Getting data onto the machine

`download_data.sh` needs bash, curl and tar, which Windows users won't have.
Decision: a built-in `data fetch` command.

How it works:
- It downloads a small JSON manifest (file, URL, size, SHA-256) and then each
  file, through `chttp`, which already has WinHTTP.
- Fetching files one at a time avoids writing tar and gzip extraction.
- It verifies the hashes contribute already checks.
- It also replaces the bash script on Linux and macOS, so the code path is
  exercised constantly rather than only on Windows. That includes `testdata`
  and the CI data-cache flow, which today run `download_data.sh`.
- The base set for one lexicon is about 10 MB (CSW24 KWG + KLV2, layouts,
  letter distributions, win-pct).

What it needs that doesn't exist yet:
- **A hosting change in MAGPIE-DATA.** `download_data.sh` downloads versioned
  tarballs from MAGPIE-DATA, some split into chunks (`.tgz.aa`, `.ab`, ...).
  A per-file manifest means MAGPIE-DATA also publishes individual files and the
  manifest. That work happens in the other repo, before the `data fetch` PR.
- **Aliases instead of symlinks.** The data set has 23 symlinks (section 2.2).
  The manifest lists them as aliases of their targets, and `data fetch` writes
  a copy on Windows. Loaders must not depend on a file being a link.

Rejected:
- **A release zip** (`magpie.exe` + `data/`). Users must unzip it, and every
  data update means a new download.
- **Data embedded in the `.exe`** through `precache_file_data`. The binary grows
  by about 10 MB per lexicon, and every data update needs a new binary.

### 6.3 Release

- **Workflow.** A `release-windows.yml` workflow triggered by tag:
  1. L1 builds `magpie-<version>-windows-x64.exe` and
     `magpie-<version>-windows-arm64.exe`;
  2. L4 and L5 run against those exact files;
  3. the workflow attaches both files and their `.sha256` to a GitHub Release.
- **No code signing.** Windows will show the SmartScreen warning on first
  launch ("Windows protected your PC", then More info, then Run anyway).
  - The README's Windows section and the release notes show that step with a
    screenshot, and say how to check the file against its `.sha256`.
  - Unsigned executables that open network connections draw occasional Defender
    false positives. If one is reported, submit the release file to Microsoft's
    malware analysis portal as a false positive.
  - Signing can be added later without changing anything else in this plan.

## 7. PR sequence

Each PR is small and focused (per AGENTS.md), keeps Linux behaviour unchanged,
and shrinks the L0 allowlist. Sizes are relative: S < M < L; nothing is L.

| # | PR | Size | Verified by |
|---|---|---|---|
| 0 | This plan | — | Review |
| 1 | Fix `cpthread_cond_timedwait_loop` (`!done` should be `!*done`); delete `get_process_output`/`popen_or_die` and the unused `<complex.h>` | S | `sim` and `bai` tests, run repeatedly: the broken wait may have been hiding hangs or flakiness |
| 2 | Deterministic sorts: tie-break `reorder_pair_cmp` on `head`, and audit the other `qsort` comparators for ties | S | `builderhash` unchanged |
| 3 | `check_portability.py` with today's allowlist, added to `lint-fast`; cross-compile job (L1, x64 and arm64), required for every file that already compiles and reporting the rest | S | CI |
| 4 | Binary and text I/O: `"rb"`/`"wb"` everywhere, CRLF-tolerant `get_string_from_file_handle`, `.gitattributes` | S | L2 binary round-trip fixtures on Linux; `builderhash` unchanged |
| 5 | printf format fixes plus the format attribute on MAGPIE's own formatters | M | `-Wformat` clean on Linux and under L1 |
| 6 | 64-bit file offsets via `cfile` | S | L2; full Linux suite |
| 7 | `cfile`: rename, access, stat, mkdir, getline; share-delete readers; fold `atomic_file.h` into `cfile` | M | L2 contracts; full Linux suite |
| 8 | `cdir` and `cpath`: opendir, glob, path-list separator, executable directory | M | L2 contracts; full Linux suite |
| 9 | Wine job running `compat_test` only (L3, first stage), required | S | L3 green |
| 10 | `cmap` for the RIT mmap; byte-span precache in place of `fmemopen` | M | `ap_rit`, `wordmapapi`, `fileproxy`, wasm tests; `compat_test` under Wine |
| 11 | Hand-written GCG tokenizer, with the differential test against POSIX regex, then the regex removed | M | `gcg`, `historyreplay`, differential test |
| 12 | `cpthread`/`ctime`/`cterm`/`cstring`: no raw pthreads, timing calls or `isatty` outside compat; `cpthread_defs.h` moves into compat | M | TSAN build (`BUILD=thread`), full suite; `get_movegen` timing |
| 13 | Console: one process-lifetime stdin reader shared by the REPL and async control (changes `exec.c`), its Windows backend, `ReadConsoleW` line input, VT and UTF-8 setup | M | `command` test on Linux and under Wine; manual check on L4 |
| 14 | Makefile Windows target for x64 and arm64, `make windows`, manifest and resources. **L1 becomes fully required** and the L0 allowlist is empty. | S | L1 green |
| 15 | Portable variants of the POSIX-only test tricks; full Wine suite (L3) required | M | L3 green |
| 16 | Native Windows jobs (L4, x64 and arm64) required. Fix whatever it finds: open-file delete semantics, stack commit, TLS. | M | L4 green |
| 17 | First-run UX: executable-relative data, double-click detection (banner and wait-before-close only), `data fetch`. Needs the MAGPIE-DATA manifest first (section 6.2). | M | L5 smoke test |
| 18 | Build a missing `.wmp` on first use, falling back to `-wmp false` when there isn't disk space, the lexicon has no WMP support, or the data directory isn't writable | M | Linux suite plus L4/L5 timing |
| 19 | `release-windows.yml` (x64 and arm64, unsigned) and the README's Windows section | S | Tagged prerelease |

**Ordering.**
- PRs 1–8, 10–12 change shared code and add Windows backends inside
  `src/compat`. They need no Windows machine, but they are not Linux-only:
  their Windows branches are compiled by L1 from PR 3 on, and run under Wine
  from PR 9 on, so no Windows code waits until PR 16 to execute.
- PRs 4, 6 and 7 fix bugs that would corrupt data or lose writes on Windows,
  so they come early.
- PR 14 is the first point where the whole Windows binary should build cleanly.

## 8. Risks and unknowns

- **Wine is not Windows.** It is more permissive about deleting or renaming
  files that are open or mapped, and about stack commit. That is why L4 runs on
  real Windows and is required.
- **Deleting open or mapped files.** Windows cannot delete or replace a file
  that is open without `FILE_SHARE_DELETE`. The `cfile` design (section 3.2)
  opens readers with it and renames with POSIX semantics, but a mapped file
  (the RIT) may still refuse, and POSIX semantics need NTFS. Contribute's
  eviction (`contribute_tables.c:543`) and RIT replacement have to handle "in
  use" by retrying later or skipping, not by failing.
- **Byte-identical derived files.** The builders do no floating-point
  arithmetic, and PR 2 removes the `qsort` tie dependence. If `builderhash`
  still differs under L3/L4, look next for uninitialized struct padding written
  to disk or another library-dependent order, not the compiler.
- **More than 64 logical CPUs.** `GetSystemInfo` (`memory_info.h`) reports at
  most one processor group (64 logical CPUs), and on Windows 10 a process's
  threads stay in one group. On larger machines, autoplay and contribute use
  at most 64 threads unless compat assigns threads across groups.
- **PEG stack commit.** Thirty-two workers with 64 MiB stacks, committed up
  front, would need about 2 GB of commit charge. Stage 2 of `cpthread`
  (reservation-only stacks) fixes it. L4 should include a PEG test at the
  runner's full thread count.
- **Performance.** Performance on Windows is not measured by this plan. After
  L4 runs, run the existing `tablebench`/autoplay timings there once, to make
  sure nothing (winpthreads mutexes, `QueryPerformanceCounter`) is
  pathologically slow.
- **Unsigned binaries.** Some users will stop at the SmartScreen warning, and
  antivirus false positives are more likely without a signature. The README
  instructions reduce the first; the second is handled per report.
- **arm64 performance.** Code tuned on x86 may have hot paths that are slow on
  ARM (for example, missing fast paths behind `__has_builtin`). Measure on
  `windows-11-arm` once L4 runs.
- **Unicode paths.** These depend on the UTF-8 manifest code page, which needs
  Windows 10 1903 or later. Older Windows is unsupported.

## 9. Decisions

Settled by the maintainer in October 2026:

1. **Data delivery:** a built-in `data fetch` command (section 6.2).
2. **Missing `.wmp`:** build it on first use (section 6.1). Fall back to
   `-wmp false` only when there isn't disk space. (Review note: the fallback
   must also cover lexica with no WMP support, OSPS and `_super`, and a
   read-only data directory; section 6.1 includes them.)
3. **GCG parsing:** a hand-written tokenizer, proven against POSIX regex by a
   differential test (section 3.4).
4. **Code signing:** none. The README explains the SmartScreen step (section
   6.3).
5. **Use case and double-click:** Windows users use MAGPIE the same way Linux
   and macOS users do. A double-click opens the normal REPL, and `contribute`
   is an ordinary opt-in command, not a Windows focus.
6. **Windows on ARM:** arm64 ships alongside x64 from the first release.

## Appendix A: call sites outside `src/compat`

From the audit at commit `d310de76`. Line numbers drift; PR 3's lint is the
living version of this list.

- **`<unistd.h>`**: `wmp.h`, `rack_info_table.h`, `word_info_table.c`,
  `data_filepaths.c`, `game_history.c`, `autoplay_results.c`, `exec.c`,
  `get_gcg.c`, `game_string.c`, `fileproxy.c`
- **`access()`**: `data_filepaths.c:194,195,205`, `fileproxy.c:145`,
  `autoplay_results.c:1188,1306`, `game_history.c:477`, `exec.c:243`,
  `get_gcg.c:262,268`
- **`stat`/`S_ISDIR`/`mkdir`**: `io_util.c:591,659`, `convert.c:384`,
  `config.c:4819-4820`
- **`opendir`/`readdir`**: `io_util.c:545-763` (and `DIR *` in `io_util.h:396`,
  which leaks `<dirent.h>` into almost every file)
- **`glob`**: `data_filepaths.c:233-246`
- **`mmap`/`munmap`/`madvise`/`open`/`fstat`**: `rack_info_table.h:391-791`
- **`fdopen`/`close`**: `wmp.h:981,984`, `word_info_table.c:300,303`
- **`rename`**: `io_util.c:691`, `wmp.h:1003`, `word_info_table.c:322`,
  `contribute_tables.c:246`, `config.c:11189`
- **`fmemopen`**: `fileproxy.c:49`
- **`getline`/`ssize_t`**: `io_util.c:218-220`, `klv_csv.c:199`,
  `convert.c:103,231`
- **regex**: `gcg.c:24-346`
- **`strcasecmp`/`strncasecmp`**: `string_util.c:464,516`
- **Raw pthreads / `PTHREAD_MUTEX_INITIALIZER`**: `io_util.c:26-37`,
  `fileproxy.c:27`, `peg_pool.c:280-289`, `move_gen.c:66`,
  `bai_sched_stats.c:11`, `def/cpthread_defs.h`
- **Time**: `config.c:4824-4828` (`gettimeofday`, `localtime`),
  `peg.c:2817` (`nanosleep`), `io_util.c:181` (`localtime`)
- **`isatty(fileno())`**: `exec.c:353`, `game_string.c:50`
- **`:` path lists**: `data_filepaths.c:117,190,227`,
  `contribute_tables.c:666`, `xs_bits.c:291`, `convert.c:375`
- **`/`-only path parsing**: `io_util.c:641`, `get_gcg.c:61`
- **Text-mode reads of binary data**: `fileproxy.c:59`, which every binary
  loader goes through
- **`%lu`/`%ld` on 64-bit values**: `klv_csv.c:53`,
  `autoplay_results.c:335-501`, `autoplay.c:1398,1405`,
  `sim_string.c:60-286`, `inference_string.c:84-119`, `heat_map.h:193`,
  `config.c:1072`, `io_util.c:312-329`
- **Platform macros**: `def/wmp_defs.h:44` (`__APPLE__ && __aarch64__`)
- **`qsort` comparators that tie**: `kwg_maker.c:904` (`reorder_pair_cmp`),
  `peg.c:2191` (`peg_rank_cmp`); the other call sites still need the audit

## Appendix B: reproducing the trial cross-compile

```bash
docker run --rm -v "$PWD":/src:ro -w /src ubuntu:24.04 bash -c '
  apt-get update -qq && apt-get install -y -qq gcc-mingw-w64-x86-64 >/dev/null
  for f in $(find src cmd tools -name "*.c"); do
    x86_64-w64-mingw32-gcc-posix -std=gnu11 -fsyntax-only \
      -DBOARD_DIM=15 -DRACK_SIZE=7 -DMAGPIE_BUILD_TARGET="\"default\"" \
      -Werror=implicit-function-declaration "$f" 2>&1 | grep "error:"
  done | sort -u'
```

To see past the first missing header, add `-isystem <dir>`, where `<dir>`
contains empty stand-ins for `sys/mman.h`, `glob.h`, `regex.h`, `termios.h`,
`sys/ioctl.h`, `sys/poll.h`, `sys/statvfs.h`, `sys/file.h` and
`sys/resource.h`.
