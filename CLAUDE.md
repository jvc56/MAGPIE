# CLAUDE.md — MAGPIE

MAGPIE (Macondo Accordant Game Program and Inference Engine) is a crossword game engine written in C (primarily C11, using `_Atomic` for lock-free concurrency). It supports move generation, Monte Carlo simulation, exhaustive inference, autoplay, superleave generation, and endgame solving.

## Initial Setup (fresh clone)

After cloning, you must download data and convert lexica before anything will work:

```bash
./download_data.sh             # downloads data/ and testdata/ from MAGPIE-DATA repo
./convert_lexica.sh            # builds MAGPIE, converts .kwg → .txt → .wmp
```

`data/` and `testdata/` are gitignored. If tests fail with missing file errors, this is almost certainly the reason — run these scripts first.

## Build

```bash
make magpie                    # dev build (ASAN/UBSAN enabled)
make magpie BUILD=release      # optimized build
make magpie_test               # build test binary
make magpie_test BUILD=release # release test binary
make clean                     # remove build artifacts
```

Build variables: `BOARD_DIM` (default 15), `RACK_SIZE` (default 7). Parallel make is automatic.

**Use `BUILD=release` for anything that runs more than a few seconds.** The default dev build (`-O0` + ASAN/UBSAN) is extremely slow — reserve it for debugging crashes and memory errors.

## Tests

```bash
./run u              # full test suite: release then dev, BOARD_DIM=15
./run u 21           # full test suite with BOARD_DIM=21
./run u movegen      # run a specific test (by name)
./run v              # valgrind (leak check)
./run v move_gen     # valgrind on specific test
./run g              # gdb
./run g move_gen     # gdb on specific test
./run c              # coverage report (opens in browser)
./run s              # scan-build static analysis
./run r              # build and run release
```

Tests run in both release and dev mode. Dev mode has ASAN/UBSAN enabled, so it catches memory errors and undefined behavior. CI also runs tests with `BOARD_DIM=21`.

On-demand (non-CI) tests are registered in `test/test.c` in `on_demand_test_table[]`.

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

## C Standard

The codebase is primarily C99 but uses these C11 features:
- `_Atomic` / `<stdatomic.h>` with explicit memory ordering (thread_control, transposition_table, endgame, sim_results)
- `static_assert` for compile-time size validation (transposition_table.h, wmp_maker.c)

Do not introduce C features beyond C11 or C11 features not already used in the codebase without asking first.

## Development Methodology

Speed and correctness are both critical to MAGPIE. Keep performance work and quality work separate — mixing them makes it hard to attribute regressions or improvements.

### Performance work

When optimizing code, the goal is to make it faster **without changing behavior**:
- Validate correctness by running autoplay or writing temporary on-demand tests that compare outputs before and after the change.
- Profile to measure targeted impact on the functions you changed:
  - `make magpie BUILD=profile` — build with profiling support
  - On macOS, use `sample <pid>` to capture call stacks of a running process
- Focus measurements on the specific functions changed, not just wall-clock time of a full run.

### Quality work

When improving play quality (e.g., better simulation, inference, move selection), validate rigorously with **game pairs** — two engine variants play the same sequence of tile draws, alternating who goes first, to control for luck. Don't rely on small samples or single-game anecdotes.

**Using autoplay with game pairs** (interactive console or CLI):
```bash
# Configure two players with different strategies, then run game pairs
set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 1
autoplay games 1000 -gp true -threads 8 -seed 42
```
Key options: `-gp true` enables game pairs (each "game" becomes a pair with swapped sides, so `games 1000` plays 2000 total). `-s1`/`-s2` set player strategies. `-seed` makes runs reproducible. Always use `-wmp true` for speed (word maps vs. KWG traversal) and `-threads <N>` to use multiple cores.

**Using on-demand tests** for automated comparison:
Register a test in `test/test.c`'s `on_demand_test_table[]`, then run it with `./run u <test_name>`. This is useful for before/after comparisons when changing move selection or evaluation logic — the test can assert that results match or report divergence stats.

### When speed *is* quality

When a time limit is in effect (e.g., endgame benchmarks with a fixed search budget), faster code finds better solutions in the same time. Measuring quality under a time limit is a good way to test whether optimizations pay off in the use cases we care about.

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

## Debugging

- Dev build (`make magpie` with no BUILD flag) enables ASAN and UBSAN — most memory bugs surface immediately
- `./run g` launches gdb on the test binary
- `./run v` runs valgrind with full leak checking
- `./run c` generates an HTML coverage report
- Thread sanitizer: `make magpie BUILD=thread`

## Platform Notes

- macOS `awk` does not support `asorti` — use hardcoded arrays or `gawk` instead.
- CI runs on Ubuntu 24.04; macOS-specific behavior may differ.

## PR Workflow Tips

1. **Run `python3 format.py --write` before every push.** This is the #1 cause of CI failure.
2. **Run `./cppcheck.sh` locally** if you can. It builds cppcheck 2.17.1 from source on first run and caches it.
3. **Keep PRs focused.** Large PRs with multiple concerns tend to get closed and resubmitted. Split into smaller, mergeable units.
4. **Check `const` qualifiers proactively.** Add `const` to every pointer parameter and variable that doesn't need mutation.
5. **Include what you use.** Don't rely on transitive includes.
