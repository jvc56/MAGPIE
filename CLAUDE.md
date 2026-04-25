# CLAUDE.md — MAGPIE

Crossword game engine in C (C99 with some C11). Move generation, Monte Carlo simulation, inference, autoplay, superleave generation, endgame solving.

## Initial Setup (fresh clone)

```bash
./download_data.sh             # downloads data/ and testdata/ from MAGPIE-DATA repo
./convert_lexica.sh            # converts .kwg → .txt → .wmp (builds MAGPIE first)
```

`data/` and `testdata/` are gitignored. Missing file errors = run these scripts.

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

`BOARD_DIM` (default 15), `RACK_SIZE` (default 7). Parallel make is automatic. **Use `BUILD=release` for anything beyond a few seconds** — dev is `-O0` + ASAN/UBSAN, very slow.

## Tests

`./bin/magpie_test movegen` — test names are short keys in `test/test.c` (`movegen`, `sim`, `autoplay`, etc.). CI runs both `BOARD_DIM=15` and `21`. Non-CI tests are in `on_demand_test_table[]`.

## CI Checks (the hard part)

CI runs on every push. Six jobs must all pass: **cppcheck**, **clang-tidy**, **clang-format**, **circular-dependencies**, **unit-tests**, **wasm-tests**.

CI uses **cppcheck 2.17.1**, **clang-tidy-18**, **clang-format-20** (Ubuntu 24.04). Local versions will differ.

```bash
python3 format.py --write      # ALWAYS run before pushing
./cppcheck.sh                  # builds cppcheck 2.17.1 on first run, cached after
./tidy.sh                      # clang-tidy (or: ./tidy.sh clang-tidy-18)
python3 find_circ_deps.py      # circular dependencies (requires: pip install networkx)
```

**format.py is NOT just clang-format** — it reorders includes (self-header first, blank line, then all others contiguous) before running `clang-format-20`. No config files.

## C Standard

C99 with two C11 exceptions: `_Atomic`/`<stdatomic.h>` and `static_assert`. Do not introduce other C11+ features without asking.

## Style

Always use `{}` braces for `if`, `else if`, and `else` blocks, even when the body is a single statement. No exceptions.

**No forward declarations** — Never forward-declare a struct that is already defined in another module's header. Use `#include` to bring in that header instead. Forward declarations are only acceptable when the struct or function is defined in the same file.

## Naming

Avoid single-character or terse variable names.
- Prefer `row`/`col` over `i`/`j`
- Loop variables: use `foo_idx` (e.g., `player_idx`, `tile_idx`)
- For letters/tiles, `ml` (machine letter) is the idiomatic name
- No ambiguous single characters: `p` could mean player, ply, or pointer

## Development Methodology

Keep performance work and quality work separate.

### Performance work

Make it faster **without changing behavior**. Validate with autoplay or on-demand tests comparing before/after. Profile with `BUILD=profile` and `sample <pid>` on macOS. Measure the specific functions changed, not just wall-clock time.

### Quality work

Validate with **game pairs** — two variants play identical tile draws, alternating who goes first.

```bash
set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 1
autoplay games 1000 -gp true -threads 8 -seed 42
```

`-gp true` = game pairs (`games 1000` = 2000 total). Always use `-wmp true` and `-threads <N>`.

For automated comparison, register an on-demand test in `on_demand_test_table[]`.

### When speed *is* quality

Under a time limit (e.g., endgame benchmarks), faster code finds better solutions. Measuring quality under a time budget validates that optimizations pay off.

### Endgame benchmarking and transposition tables

When comparing two solver modes, the second benefits unfairly from the first's TT cache. Use separate TTs or reset between solves. Total TT memory must not exceed 50% of system RAM (`0.5` for one TT, `0.25` each for two).

## Common Pitfalls

1. **const pointers (cppcheck)** — The #1 CI failure. `const` every pointer that doesn't need mutation. False positive? `// cppcheck-suppress constVariablePointer`
2. **Include what you use (clang-tidy)** — Every symbol needs a direct `#include`. Transitive includes don't count.
3. **Include block formatting** — Contiguous, no blank lines except after the self-header in `.c` files.
4. **Designated initializers** — Omitted struct fields zero-initialize, which may differ from intended defaults. Audit call sites when adding fields.
5. **No pthread_mutex_destroy / pthread_cond_destroy** — Project mutexes don't dynamically allocate. LLMs always add these — don't.
6. **No debug prints** — No `printf`/`fprintf` in production code. Use the status print mechanism.
7. **Tests in test files** — Test-only code belongs in `test/*.c`, not `src/`.
8. **Equity is millipoints** — `Equity` (`int32_t`) = millipoints (42 points = `42000`). Use `int_to_equity()` / `equity_to_int()` / `double_to_equity()`.
9. **Resource leaks on error paths** — Release all resources before error returns. cppcheck catches missing `fclose`/`free`.

## Keep PRs focused
Large PRs with multiple concerns get closed. Split into smaller, focused units.
