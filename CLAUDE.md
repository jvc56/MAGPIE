# CLAUDE.md — MAGPIE

MAGPIE (Macondo Accordant Game Program and Inference Engine) is a crossword game engine written in C99. It supports move generation, Monte Carlo simulation, exhaustive inference, autoplay, superleave generation, and endgame solving.

## Build

```bash
make magpie                    # dev build (ASAN/UBSAN enabled)
make magpie BUILD=release      # optimized build
make magpie_test               # build test binary
make magpie_test BUILD=release # release test binary
make clean                     # remove build artifacts
```

Build variables: `BOARD_DIM` (default 15), `RACK_SIZE` (default 7). Parallel make is automatic.

## Tests

```bash
./run u              # full test suite: release then dev, BOARD_DIM=15
./run u 21           # full test suite with BOARD_DIM=21
./run u move_gen     # run a specific test (by name)
./run v              # valgrind (leak check)
./run v move_gen     # valgrind on specific test
./run g              # gdb
./run g move_gen     # gdb on specific test
./run c              # coverage report (opens in browser)
./run s              # scan-build static analysis
./run r              # build and run release
```

Tests run in both release and dev mode. Dev mode has ASAN/UBSAN enabled, so it catches memory errors and undefined behavior. CI also runs tests with `BOARD_DIM=21`.

## CI Checks (the hard part)

CI runs on every push. Six jobs must all pass: **cppcheck**, **clang-tidy**, **clang-format**, **circular-dependencies**, **unit-tests**, **wasm-tests**.

The static analysis tools use **specific versions** that differ from what Homebrew/apt typically install. Local versions will produce different results. Here's what CI uses:

| Tool | CI Version | CI Platform |
|------|-----------|-------------|
| cppcheck | **2.17.1** (built from source) | Ubuntu 24.04 |
| clang-tidy | **clang-tidy-18** | Ubuntu 24.04 |
| clang-format | **clang-format-20** | Ubuntu 24.04 |

### Running checks locally

```bash
# Formatting (MOST COMMON FAILURE — always run before pushing)
python3 format.py              # check only — shows diffs
python3 format.py --write      # auto-fix in place
python3 format.py myfile.c     # check a single file

# cppcheck (downloads and builds 2.17.1 on first run, cached after)
./cppcheck.sh

# clang-tidy
./tidy.sh                      # uses 'clang-tidy' from PATH
./tidy.sh clang-tidy-18        # specify exact binary

# Circular dependencies
pip install networkx
python3 find_circ_deps.py
```

### Critical: format.py is NOT just clang-format

`format.py` enforces include ordering rules *before* running `clang-format-20`:
1. Self-header first (for `.c` files), followed by a blank line
2. All other includes in a contiguous block — no blank lines between includes
3. Then `clang-format-20` runs on the result

Running `clang-format` alone is **not sufficient**. Always use `python3 format.py --write`.

There is **no `.clang-format` config file** — clang-format-20 uses its built-in defaults. There is no `.clang-tidy` config file either — checks are specified inline in `tidy.sh`.

### What to do when you can't run the exact CI versions locally

If you don't have the exact tool versions installed, at minimum:
- Always run `python3 format.py --write` (requires `clang-format-20`)
- Mark pointers and parameters `const` wherever possible (the #1 cppcheck failure)
- Include headers directly for every symbol you use (the #1 clang-tidy failure)
- Keep include blocks contiguous with no non-include lines mixed in

## Code Conventions

### File organization
- `src/def/` — type definitions, constants, struct definitions
- `src/ent/` — entity implementations (core data structures)
- `src/impl/` — algorithms and business logic
- `src/str/` — string formatting and serialization
- `src/util/` — general utilities (I/O, math, string)
- `src/compat/` — platform compatibility, vendored code (linenoise). **Excluded from formatting and linting.**
- `test/` — unit tests (`*_test.c` / `*_test.h` pairs)
- `cmd/` — entry point (`magpie.c`)

### Naming
- Files: `snake_case.c/h`
- Types/Structs: `PascalCase`
- Functions: `entity_action_verb()` (e.g., `game_create`, `board_get_square`)
- Constants/Macros: `UPPER_SNAKE_CASE`
- Enum values: `UPPER_SNAKE_CASE` with a type prefix

### Include order (enforced by format.py)
For `.c` files:
```c
#include "myfile.h"

#include "other_local_headers.h"
#include <stdlib.h>
#include <string.h>
```
Self-header first, blank line, then everything else in one contiguous block.

### Style rules
- C99 standard (`-std=c99`)
- No magic numbers in general, but this clang-tidy check is disabled
- Use project abstractions (e.g., `Timer` from `ctime.h`, threading wrappers from `cpthread.h`) instead of raw POSIX types
- Header guards use `#ifndef FILENAME_H` / `#define FILENAME_H` / `#endif`

## Common Pitfalls (from PR history)

These are recurring issues that have caused CI failures and review feedback. Pay close attention.

### 1. constVariablePointer / constParameterPointer (cppcheck)
The single most common CI failure. cppcheck flags every pointer that could be `const` but isn't. When writing new code or modifying functions:
- Use `const` on pointer parameters whenever the pointed-to data isn't modified
- Use `const` on local pointer variables when the target isn't modified
- If it's a false positive, suppress inline: `// cppcheck-suppress constVariablePointer`

### 2. misc-include-cleaner (clang-tidy)
The most common clang-tidy failure. Every symbol you use must come from a directly `#include`d header — transitive includes don't count. If you use `malloc`, include `<stdlib.h>` directly even if it's already pulled in by another header.

### 3. Include block formatting
Includes must be contiguous. No code, comments, or blank lines between the first and last `#include`. The only allowed blank line is after the self-header in `.c` files.

### 4. Struct initialization with designated initializers
When adding fields to existing structs, callers using designated initializers get zero-initialized values for omitted fields. This can silently differ from intended defaults and cause subtle bugs. Audit all call sites when adding struct fields.

### 5. Don't call pthread_mutex_destroy / pthread_cond_destroy unnecessarily
The project's POSIX mutex/cond implementations don't dynamically allocate, so `*_destroy` calls are unnecessary. LLMs consistently try to add these — don't.

### 6. Remove debug prints before submitting
Diagnostic `printf`/`fprintf` statements should not appear in production code. Move diagnostic output to the "status" print mechanism or remove it entirely.

### 7. Keep tests in test files
Test-only assertions and checks belong in `test/*.c` files, not in `src/` headers or source files.

### 8. Resource leaks on error paths
cppcheck catches missing `fclose` / `free` on error return paths. When writing error handling, ensure all resources acquired before the error point are released.

## Suppressing false positives

```c
// cppcheck-suppress constVariablePointer
char *ptr = get_ptr();

// NOLINTNEXTLINE(cert-env33-c)
system(cmd);

// clang-format off
// ... code exempt from formatting ...
// clang-format on
```

## Debugging

- Dev build (`make magpie` with no BUILD flag) enables ASAN and UBSAN — most memory bugs surface immediately
- `./run g` launches gdb on the test binary
- `./run v` runs valgrind with full leak checking
- `./run c` generates an HTML coverage report
- Thread sanitizer: `make magpie BUILD=thread`

## PR Workflow Tips

1. **Run `python3 format.py --write` before every push.** This is the #1 cause of CI failure.
2. **Run `./cppcheck.sh` locally** if you can. It builds cppcheck 2.17.1 from source on first run and caches it.
3. **Keep PRs focused.** Large PRs with multiple concerns tend to get closed and resubmitted. Split into smaller, mergeable units.
4. **Check `const` qualifiers proactively.** Add `const` to every pointer parameter and variable that doesn't need mutation.
5. **Include what you use.** Don't rely on transitive includes.
6. **CI runs on Ubuntu 24.04.** macOS-specific behavior may differ — test on Linux if possible.
