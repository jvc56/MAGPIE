# CLAUDE.md — MAGPIE

MAGPIE (Macondo Accordant Game Program and Inference Engine) is a crossword game engine written in C (primarily C99 with a small set of C11 features). It supports move generation, Monte Carlo simulation, exhaustive inference, autoplay, superleave generation, and endgame solving.

## Initial Setup (fresh clone)

```bash
./download_data.sh             # downloads data/ and testdata/ from MAGPIE-DATA repo
./convert_lexica.sh            # builds MAGPIE, converts .kwg → .txt → .wmp
```

`data/` and `testdata/` are gitignored. If tests fail with missing file errors, run these scripts.

## Build

```bash
make magpie                    # dev build (ASAN/UBSAN enabled)
make magpie BUILD=release      # optimized build
make magpie_test               # build test binary
make magpie_test BUILD=release # release test binary
make magpie BUILD=profile      # profiling build
make magpie BUILD=thread       # thread sanitizer build
make clean                     # remove build artifacts
```

Build variables: `BOARD_DIM` (default 15), `RACK_SIZE` (default 7). Parallel make is automatic.

**Use `BUILD=release` for anything that runs more than a few seconds.** The dev build (`-O0` + ASAN/UBSAN) is extremely slow — reserve it for debugging crashes and memory errors.

## Tests

```bash
./run u              # full test suite: release then dev, BOARD_DIM=15
./run u 21           # full test suite with BOARD_DIM=21
./run u movegen      # run a specific test (by name)
./run v movegen      # valgrind on specific test
./run g movegen      # gdb on specific test
./run c              # coverage report
./run s              # scan-build static analysis
./run r              # build and run release
```

Tests run in both release and dev mode. CI also runs with `BOARD_DIM=21`. On-demand (non-CI) tests are registered in `test/test.c` in `on_demand_test_table[]`.

## CI Checks (the hard part)

CI runs on every push. Six jobs must all pass: **cppcheck**, **clang-tidy**, **clang-format**, **circular-dependencies**, **unit-tests**, **wasm-tests**.

The static analysis tools use **specific versions** that differ from what Homebrew/apt typically install:

| Tool | CI Version | CI Platform |
|------|-----------|-------------|
| cppcheck | **2.17.1** (built from source) | Ubuntu 24.04 |
| clang-tidy | **clang-tidy-18** | Ubuntu 24.04 |
| clang-format | **clang-format-20** | Ubuntu 24.04 |

### Running checks locally

```bash
python3 format.py              # check only — shows diffs
python3 format.py --write      # auto-fix in place (ALWAYS run before pushing)
./cppcheck.sh                  # builds cppcheck 2.17.1 on first run, cached after
./tidy.sh                      # clang-tidy (uses 'clang-tidy' from PATH)
./tidy.sh clang-tidy-18        # specify exact binary
python3 find_circ_deps.py      # circular dependencies (requires: pip install networkx)
```

### format.py is NOT just clang-format

`format.py` enforces include ordering *before* running `clang-format-20`:
1. Self-header first (for `.c` files), followed by a blank line
2. All other includes in a contiguous block — no blank lines between includes
3. Then `clang-format-20` runs on the result

Running `clang-format` alone is **not sufficient**. There is no `.clang-format` or `.clang-tidy` config file — defaults and inline flags are used.

## C Standard

Primarily C99 with these C11 exceptions:
- `_Atomic` / `<stdatomic.h>` with explicit memory ordering (thread_control, transposition_table, endgame, sim_results)
- `static_assert` for compile-time size validation (transposition_table.h, wmp_maker.c)

Do not introduce C11+ features not already used in the codebase without asking first.

## Naming

Avoid single-character or overly terse variable names.
- Prefer `row` and `col` over `i` and `j`
- Loop variables: use `foo_idx` (e.g., `player_idx`, `tile_idx`)
- For letters/tiles, `ml` (machine letter) is the idiomatic name
- No ambiguous single characters: `p` could mean player, ply, or pointer

## Development Methodology

Speed and correctness are both critical. Keep performance work and quality work separate — mixing them makes it hard to attribute regressions or improvements.

### Performance work

Make it faster **without changing behavior**:
- Validate with autoplay or temporary on-demand tests that compare outputs before and after.
- Profile to measure targeted impact: `BUILD=profile`, or `sample <pid>` on macOS.
- Focus on the specific functions changed, not just wall-clock time.

### Quality work

Validate with **game pairs** — two engine variants play the same tile draws, alternating who goes first, to control for luck.

```bash
set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 1
autoplay games 1000 -gp true -threads 8 -seed 42
```

`-gp true` enables game pairs (`games 1000` plays 2000 total). `-seed` makes runs reproducible. Always use `-wmp true` and `-threads <N>`.

For automated comparison, register an on-demand test in `test/test.c`'s `on_demand_test_table[]` and run with `./run u <test_name>`.

### When speed *is* quality

Under a time limit (e.g., endgame benchmarks), faster code finds better solutions. Measuring quality under a time budget validates that optimizations pay off in practice.

### Endgame benchmarking and transposition tables

When comparing two endgame solver modes, beware of TT contamination: the second solver benefits unfairly from the first's cache. Either use two separate TTs or reset between solves.

**Memory limits:** total TT memory must not exceed 50% of system memory. Use fraction `0.5` for a single TT, or `0.25` each when using two.

## Common Pitfalls

### 1. constVariablePointer / constParameterPointer (cppcheck)
The #1 CI failure. Use `const` on every pointer parameter and variable that doesn't need mutation. Suppress false positives with `// cppcheck-suppress constVariablePointer`.

### 2. misc-include-cleaner (clang-tidy)
Every symbol must come from a directly `#include`d header — transitive includes don't count.

### 3. Include block formatting
Includes must be contiguous. No code, comments, or blank lines between them. The only allowed blank line is after the self-header in `.c` files.

### 4. Struct initialization with designated initializers
Omitted fields get zero-initialized, which may differ from intended defaults. Audit all call sites when adding struct fields.

### 5. Don't call pthread_mutex_destroy / pthread_cond_destroy
The project's mutex/cond implementations don't dynamically allocate, so destroy calls are unnecessary. LLMs consistently add these — don't.

### 6. Remove debug prints before submitting
No `printf`/`fprintf` in production code. Use the status print mechanism or remove entirely.

### 7. Keep tests in test files
Test-only code belongs in `test/*.c`, not in `src/`.

### 8. Equity is millipoints, not points
`Equity` (`int32_t`) stores **millipoints** (1 point = 1000). A score of 42 is `42000`. Use `int_to_equity()` / `equity_to_int()` / `double_to_equity()` — don't do manual arithmetic.

### 9. Resource leaks on error paths
Ensure all resources acquired before an error point are released. cppcheck catches missing `fclose`/`free`.

## Keep PRs focused
Large PRs with multiple concerns tend to get closed and resubmitted. Split into smaller, mergeable units.
