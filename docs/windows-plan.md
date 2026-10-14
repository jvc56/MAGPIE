# Windows port plan

Status: proposal, October 2026. Nothing in this document has been implemented yet.

## Summary

- **Toolchain.** Build `magpie.exe` with MinGW-w64, using the clang-based
  [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) with the UCRT runtime.
  It cross-compiles from Linux and macOS and is linked statically, so the result
  is one self-contained `.exe` with no DLLs to install. Native MSVC is out of
  scope.
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
- **Testing without a Windows machine.** Five layers, all of which run in CI:
  1. The portability lint.
  2. A MinGW cross-compile with `-Werror` on every push.
  3. The cross-compiled test binary run under Wine on Linux.
  4. The same `.exe` run on GitHub's hosted `windows-latest` runners, which are
     real Windows.
  5. Contract tests for each compat module, run on every OS.

  Nobody on the team needs a Windows machine.
- **For users.** They get one `.exe`. Double-clicking it opens a console that:
  - finds or downloads its data;
  - stays open long enough for any error to be read;
  - runs the existing REPL, including `contribute`.

  `contribute` (the birdtest worker) is probably the main reason Windows users
  will download it. People who only want to play or analyze can already use the
  browser build.

## 1. Constraints this plan is built around

1. **Non-developers download one `.exe` and open it.** No compiler, no bash, no
   `download_data.sh`, no WSL. Today a double-clicked `magpie` with no `data/`
   next to it prints `(error 154) file 'standard15' not found`, exits with
   status 0, and its window closes at once.
2. **MAGPIE's developers do not have Windows machines.** Every Windows check
   must run in CI or on Linux/macOS, and real Windows runs must be automated.
3. **`src/` outside `src/compat` stays platform-agnostic.** Anything that does
   not work on Windows goes behind a compat wrapper. Section 4 lists the one
   deliberate exception, the GCG regex, and the reason for it.

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
| `rename()` over an existing file | `io_util.c:691`, `wmp.h:1003`, `word_info_table.c:322`, `contribute_tables.c:246`, `config.c:11189` | Fails once the target exists, so every atomic rewrite after the first one fails. |
| Data path list split on `:` | `data_filepaths.c:117,190,227`, `contribute_tables.c:666`, `xs_bits.c:291`, `convert.c:375` | `-path C:\magpie\data` is split into `C` and `\magpie\data`. |
| No `.gitattributes` | repo root | Git for Windows checks text files out as CRLF by default. contribute hashes layouts, letter distributions and win-pct byte for byte, so a Windows clone fails every data check. |
| `%lu`/`%ld` used for `uint64_t` | `klv_csv.c:53` (a data file), `autoplay_results.c`, `sim_string.c`, `inference_string.c`, `autoplay.c:1398,1405` | `long` is 32 bits on Windows, so counts are printed truncated. |
| `long` file offsets | `fseek_or_die`, `ftell` in `io_util.c`, `kwg.h:148`, `word_info_table.c:379`; 32-bit `st_size` in `cfile.c`, `rack_info_table.h` | The CSW24 RIT is 1.89 GB, about 260 MB below 2^31. A larger RIT would break. |
| Main-thread stack | linker default | 2 MB on MinGW (1 MB on MSVC), compared with 8 MB on Linux. PEG, endgame and BAI recurse on the calling thread. |
| PEG worker stacks | `peg_pool.c:346`, 64 MiB each | Linux commits stack lazily. Windows may commit it up front unless the thread is created with `STACK_SIZE_PARAM_IS_A_RESERVATION`. |
| Console | ANSI colors (`bonus_square.h`, `heat_map.h`, `game_string.c`) and UTF-8 tiles | Shows escape-code garbage and mojibake unless VT processing and the UTF-8 code page are turned on. |

One bug found during the audit affects every platform:
`cpthread_cond_timedwait_loop` (`src/compat/cpthread.h:64`) tests
`while (!done)`, which checks the pointer instead of `*done`. Fix it on its own,
separate from the port.

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
`find_circ_deps.py`. It parses includes and greps for the banned identifiers.
It starts with an allowlist of today's violations, and each PR in section 7
removes entries until the allowlist is empty. Reviewers then don't have to
check the rule by hand.

Wrappers are `static inline`, or plain functions on cold paths. On Linux and
macOS they compile to exactly the calls made today, so there is no performance
cost. No hot path (move generation, shadow, endgame search) touches any of
them.

### 3.2 Compat modules

| Module | Replaces | POSIX backend | Windows backend |
|---|---|---|---|
| `cfile` (extend) | `rename`, `access`, `stat`/`S_ISDIR`, `mkdir`, `fdopen`, `ftell`/`fseek`, `getline` | Today's calls, plus `fseeko`/`ftello` | `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING \| MOVEFILE_WRITE_THROUGH)`, `_waccess`, `_wstat64`, `_wmkdir`, `_fseeki64`/`_ftelli64`; a portable `getline` written in plain C |
| `cfile` (fold in `atomic_file.h`) | `lstat`, `readlink`, `mkstemp`, `fchmod`, `open` | Today's code | `CreateFileW` for the temp file, with `_O_BINARY`; resolve the target with `GetFinalPathNameByHandleW`; `MoveFileExW` to commit |
| `cdir` (new) | `opendir`/`readdir`, `glob` | `dirent` | `FindFirstFileW`/`FindNextFileW` |
| `cmap` (new) | `mmap`, `munmap`, `madvise` in `rack_info_table.h` | `mmap(PROT_READ, MAP_PRIVATE)` + `madvise` | `CreateFileMappingW` + `MapViewOfFile` (FILE_MAP_READ); `madvise` becomes a no-op |
| `cmemfile` (new) | `fmemopen` in `fileproxy.c` | `fmemopen` | Recommended: change the precache API to hand loaders a byte span rather than a `FILE *` (wasm benefits too). Fallback: a delete-on-close temp file (`"w+bD"`). |
| `cpath` (new) | `:` path lists, `/`-only basename/dirname, executable directory | `:` separator; `readlink("/proc/self/exe")` / `_NSGetExecutablePath` | `;` separator (`:` still accepted when it is not a drive letter); `GetModuleFileNameW`; `/` and `\` both treated as separators |
| `cpthread` (extend) | raw `pthread_*` and `PTHREAD_MUTEX_INITIALIZER` in `io_util.c`, `fileproxy.c`, `peg_pool.c`, `move_gen.c`, `bai_sched_stats.c`; `cpthread_defs.h` moves into compat | pthreads | Stage 1: winpthreads, linked statically. Stage 2, only if PEG stack commit or performance requires it: native SRWLOCK / CONDITION_VARIABLE / `_beginthreadex` with `STACK_SIZE_PARAM_IS_A_RESERVATION`. Adds `CPTHREAD_MUTEX_INITIALIZER`, and a `cpthread_cond_timedwait` that returns the timeout as a status instead of calling `log_fatal` (the PEG watchdog needs that). |
| `ctime` (extend) | `gettimeofday` (`config.c`), `nanosleep` (`peg.c:2818`), `localtime` | Today's calls | `QueryPerformanceCounter`, `timespec_get`, `Sleep`, `localtime_s` |
| `cterm` (new) | `isatty(fileno())` (`exec.c:353`, `game_string.c:50`); console setup; "launched by double-click" detection | `isatty` | `_isatty`; at startup, `SetConsoleOutputCP(CP_UTF8)` and `ENABLE_VIRTUAL_TERMINAL_PROCESSING`; `GetConsoleProcessList` to detect a console the process owns |
| `async_command_control` (rewrite) | `pipe` + `poll` on stdin | Unchanged | A reader thread feeds a queue; the main thread waits on one event for "input ready or finished". A console handle signals on mouse and focus events too, so waiting on it directly is unreliable. |
| `linenoise` (Windows branch) | `termios` line editing | Unchanged | Plain `fgets` in console cooked mode. Windows' console already provides arrow-key history, F7 and editing, so a raw-mode port is not needed. |
| `cstring` (or plain C in `string_util.c`) | `strcasecmp`, `strncasecmp` | — | MAGPIE only compares ASCII, so a 10-line portable version can replace both everywhere. |

### 3.3 Fixed in shared code, no compat needed

- **Binary mode everywhere.** `fileproxy.c` opens with `"rb"`. Every data-file
  open names `b` explicitly. Text readers keep tolerating `\r`, as
  `getline_ignore_carriage_return` already does.
  - `get_string_from_file_handle` reads in binary mode and drops any
    `\r` that comes before a `\n`, instead of comparing `ftell` with `fread`.
  - Writers use `"wb"`, so files MAGPIE writes are byte-identical on every OS.
    That matters because contribute and birdtest hash them.
- **`.gitattributes`.** `* text=auto eol=lf`, with `data/**` and `testdata/**`
  marked `-text`.
- **Format specifiers.** `%lu`/`%ld` on 64-bit values become `PRIu64` and
  `%zu`. Add `__attribute__((format(printf, ...)))`, through a compat macro, to
  `get_formatted_string`, `log_*` and `error_stack_push`'s formatter, so
  `-Wformat` catches the next one on Linux.
- **Delete dead code.** `get_process_output` and `popen_or_die` have no
  callers. `<complex.h>` in `math_util.h` is unused.
- **Main-thread stack.** Link the Windows build with
  `-Wl,--stack,8388608` to match Linux.

### 3.4 The one exception: GCG regex

`gcg.c` uses POSIX `regcomp`/`regexec` with about 25 fixed patterns that look
like `>([^[:space:]]+):[[:space:]]+...`. Windows has no `<regex.h>`. There are
two options:

- **(a) Recommended.** Replace the patterns with a small hand-written GCG line
  tokenizer, in plain C with no platform dependency. This removes the dependency
  instead of wrapping it. To prove the replacement correct on Linux, keep the
  regex path for one PR, behind a test-only build, and have a test run both
  tokenizers on every line of every GCG in `testdata/` plus a set of edge cases.
  The test asserts identical tokens and captures, then the regex path is
  deleted.
- **(b)** Vendor a POSIX regex implementation (musl's, MIT, about 3,500 lines)
  as `src/compat/cregex.c`, and use it on every platform so behaviour is
  identical everywhere.

(a) is less code to own, and the differential test makes the swap provable.

## 4. Toolchain and build

- **Compiler: llvm-mingw, pinned version, UCRT.**
  - It uses the same compiler family as `make release` (clang PGO).
  - It targets UCRT, which handles C99 `printf` (`%zu`, `%lld`), UTF-8 locales
    and `_set_fmode`. Ubuntu's `gcc-mingw-w64` targets the older msvcrt.
  - It can target both x86_64 and aarch64 Windows.
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
- **Manifest and resources** (embedded with `windres`):
  - `activeCodePage = UTF-8`, so narrow `fopen` and paths handle non-ASCII user
    names.
  - `longPathAware = true`.
  - A version resource and an icon.
  - With the UTF-8 code page, the narrow-character API works and compat does
    not need `wchar_t` plumbing beyond what chttp already does. It requires
    Windows 10 1903 or later.
- **CPU target.** `portable_release` (nehalem, the same ISA as x86-64-v2), as
  for contributor binaries. birdtest compares derived-file SHA-256s by build
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
| **L1** Cross-compile `magpie.exe` and `magpie_test.exe` with `-Werror` | Linux, llvm-mingw container | Compile and link breaks on Windows, on every push | About 2 minutes |
| **L2** Compat contract tests (`test/compat_test.c`), using fixtures with `\r\n` and `0x1A` bytes | Every OS that runs tests | That each compat function behaves the same everywhere; listed below | Seconds |
| **L3** `magpie_test.exe` under Wine | Linux (`wine64` from Ubuntu 24.04) | Most runtime issues: text mode, path parsing, threads, WinHTTP plumbing. Reproducible locally with `make windows_test && wine bin/magpie_test.exe <test>`. | Like a dev shard |
| **L4** Native Windows: download the L1 `.exe` artifact on `windows-latest`, restore the data cache, run the test suite (no-PGO shards) | Real Windows Server 2022/2025 | What Wine gets wrong: file-sharing and delete semantics on open or mapped files, Defender interference, real WinHTTP/Schannel TLS, stack commit, console behaviour | Like the no-PGO release job |
| **L5** Release smoke test: run the release `.exe` in an empty directory on `windows-latest` | Real Windows | The first-run path: data download or discovery, `contribute` against a birdtest staging or stub server, `builders` hashes | About 1 minute |

**Contract tests in L2:**
- binary round-trip;
- rename-over-existing;
- an atomic file write while a reader holds the target open;
- path lists with drive letters;
- listing directories;
- mapping a file and reading it back;
- `cterm` with stdin redirected.

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

Before the first public release, ask one or two volunteers (for example
birdtest contributors) to run a checklist. A Windows 11 VM image from Microsoft
works too, if anyone has an x86 machine with virtualization.

## 6. What the user gets

### 6.1 First run

Double-clicking `magpie.exe`:

1. **Console.** `cterm` turns on UTF-8 and VT colours. If the process owns its
   console (it was double-clicked, not run from a terminal), it prints a
   welcome banner. It also waits for Enter before closing after a fatal error or
   `exit`, so the window doesn't vanish.
2. **Data.** MAGPIE looks for `data/` next to the executable, then in the
   working directory, then in `%LOCALAPPDATA%\MAGPIE\data`. The first location
   is new; it comes from `cpath_executable_dir`. When launched from Explorer,
   the working directory is already the `.exe`'s folder.
3. **Missing data.** It offers to download it (see 6.2) rather than failing on
   `standard15`.
4. **WMP.** No `.wmp` is downloaded. On the first use of a lexicon, MAGPIE
   either builds the `.wmp` locally (contribute's derived-table code already
   does this) or falls back to `-wmp false` with a one-line notice. Today `-wmp`
   defaults to true and fails when the file is missing.

### 6.2 Getting data onto the machine

`download_data.sh` needs bash, curl and tar, which Windows users won't have.
Options, in order of preference:

- **(A) Recommended.** A built-in `data fetch` command.
  - It downloads a small JSON manifest (file, URL, size, SHA-256) and then each
    file, through `chttp`, which already has WinHTTP.
  - Fetching files one at a time avoids writing tar and gzip extraction.
  - It verifies the hashes contribute already checks.
  - It also replaces the bash script on Linux and macOS, so the code path is
    exercised constantly rather than only on Windows.
  - The base set for one lexicon is about 10 MB (CSW24 KWG + KLV2, layouts,
    letter distributions, win-pct).
- **(B)** Release a zip: `magpie.exe` + `data/`. It is simple and works
  offline, but users must unzip it, and updates mean downloading a new zip.
- **(C)** Embed the base data in the `.exe` and feed it to the existing
  `precache_file_data` path, which the wasm build already uses. It gives one
  file that works offline, but the `.exe` grows by about 10 MB per lexicon and
  any data update needs a new binary.

### 6.3 Release

- **Workflow.** A `release-windows.yml` workflow triggered by tag:
  1. L1 builds `magpie-<version>-windows-x64.exe` (and arm64 later);
  2. L4 and L5 run against that exact file;
  3. the workflow attaches the file and its `.sha256` to a GitHub Release.
- **Code signing.**
  - Without it, Windows shows the SmartScreen warning ("Windows protected your
    PC", then More info, then Run anyway). Unsigned executables that open
    network connections also draw occasional Defender false positives.
  - Azure Trusted Signing (about $10/month) is the cheapest signing that builds
    SmartScreen reputation. If the team decides not to pay, the download page
    should show the More info step with a screenshot.

## 7. PR sequence

Each PR is small and focused (per AGENTS.md), keeps Linux behaviour unchanged,
and shrinks the L0 allowlist. Sizes are relative: S < M < L.

| # | PR | Size | Verified by |
|---|---|---|---|
| 0 | This plan | — | Review |
| 1 | Fix `cpthread_cond_timedwait_loop` (`!done` should be `!*done`); delete `get_process_output`/`popen_or_die` and the unused `<complex.h>` | S | Existing tests |
| 2 | `check_portability.py` with today's allowlist, added to `lint-fast`; cross-compile job (L1) marked `continue-on-error` and reporting the error count | S | CI |
| 3 | Binary and text I/O: `"rb"`/`"wb"` everywhere, CRLF-tolerant `get_string_from_file_handle`, `.gitattributes`, printf format fixes plus the format attribute, 64-bit offsets via `cfile` | M | L2 binary round-trip fixtures on Linux; `builderhash` unchanged |
| 4 | `cfile`/`cdir`/`cpath`: replace rename, access, stat, mkdir, opendir, glob, getline; fold `atomic_file.h` into `cfile`; path-list separator | L | L2 contracts; full Linux suite |
| 5 | `cmap` for the RIT mmap; byte-span precache in place of `fmemopen` | M | `ap_rit`, `wordmapapi`, `fileproxy`, wasm tests |
| 6 | Hand-written GCG tokenizer, with the differential test against POSIX regex, then the regex removed | M | `gcg`, `historyreplay`, differential test |
| 7 | `cpthread`/`ctime`/`cterm`/`cstring`: no raw pthreads, timing calls or `isatty` outside compat; `cpthread_defs.h` moves into compat | M | TSAN build (`BUILD=thread`), full suite |
| 8 | Console: Windows backend for `async_command_control`, linenoise falling back to `fgets`, VT and UTF-8 setup | M | `command` test under Wine; manual check on L4 |
| 9 | Makefile Windows target, `make windows`, manifest and resources. **L1 becomes required** and the L0 allowlist is empty. | S | L1 green |
| 10 | Portable variants of the POSIX-only test tricks; Wine job (L3) required | M | L3 green |
| 11 | Native Windows job (L4) required. Fix whatever it finds: open-file delete semantics, stack commit, TLS. | M | L4 green |
| 12 | First-run UX: executable-relative data, double-click detection, `data fetch` (if option A), WMP fallback | M | L5 smoke test |
| 13 | `release-windows.yml`, plus code signing if adopted | S | Tagged prerelease |

**Ordering.**
- PRs 1–8 change only Linux code paths and land without any Windows
  infrastructure.
- PR 3 and PR 4 fix bugs that would corrupt data or lose writes on Windows, so
  they come first.
- PR 9 is the first point where a Windows binary should build cleanly.

## 8. Risks and unknowns

- **Wine is not Windows.** It is more permissive about deleting or renaming
  files that are open or mapped, and about stack commit. That is why L4 runs on
  real Windows and is required.
- **Deleting open or mapped files.** Windows cannot delete or replace a file
  that is open or mapped without `FILE_SHARE_DELETE`. Contribute's eviction
  (`contribute_tables.c:543`) and RIT replacement have to handle "in use" by
  retrying later or skipping, not by failing.
- **Floating-point reproducibility.** It differs between Linux GCC builds and
  Windows clang builds. If `builderhash` differs under L3/L4, birdtest must
  record the compiler alongside the build target, or the reference must be
  built by the same toolchain. The test will show which.
- **PEG stack commit.** Thirty-two workers with 64 MiB stacks, committed up
  front, would need about 2 GB of commit charge. Stage 2 of `cpthread`
  (reservation-only stacks) fixes it. L4 should include a PEG test at the
  runner's full thread count.
- **Performance.** Performance on Windows is not measured by this plan. After
  L4 runs, run the existing `tablebench`/autoplay timings there once, to make
  sure nothing (winpthreads mutexes, `QueryPerformanceCounter`) is
  pathologically slow.
- **Unicode paths.** These depend on the UTF-8 manifest code page, which needs
  Windows 10 1903 or later. Older Windows is unsupported.

## 9. Decisions for maintainers

1. **Data delivery.** Built-in `data fetch` (A, recommended), a release zip
   (B), or data embedded in the `.exe` (C)? Section 6.2.
2. **WMP on first use.** Build it automatically, or fall back to `-wmp false`?
   Either changes what a missing `.wmp` does today.
3. **GCG parsing.** Hand-written tokenizer (recommended) or vendored regex?
   Section 3.4.
4. **Code signing.** Is about $10/month for Azure Trusted Signing acceptable?
5. **What a double-click should do.** Should a no-argument launch on Windows
   offer a "contribute now" path, given that is the likely main use?
6. **Windows on ARM.** Ship an arm64 build from day one (llvm-mingw makes it
   nearly free, and a `windows-11-arm` runner exists), or wait?

## Appendix A: call sites outside `src/compat`

From the audit at commit `d310de76`. Line numbers drift; PR 2's lint is the
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
