# Building & CI

> **Status:** outline — prose to be written.
> **Sources:** `Makefile`, `format.py`, `cppcheck.sh`, `tidy.sh`, `find_circ_deps.py`, `.github/`
> **See also:** [CLAUDE.md](https://github.com/jvc56/MAGPIE/blob/main/CLAUDE.md)

## Build modes

<!-- NOTE: recap the make targets and BUILD modes (link to Installation). Stress
     dev = ASAN/UBSAN + -O0; use release for anything slow. -->

## The six CI jobs

<!-- NOTE: every push runs all six; all must pass. List with the local command
     to reproduce each. Note the pinned tool versions (cppcheck 2.17.1,
     clang-tidy-18, clang-format-20 on Ubuntu 24.04). -->

| Job | Reproduce locally |
| --- | ----------------- |
| clang-format | `python3 format.py --write` |
| cppcheck | `./cppcheck.sh` |
| clang-tidy | `./tidy.sh` |
| circular-dependencies | `python3 find_circ_deps.py` |
| unit-tests | `./bin/magpie_test <key>` |
| wasm-tests | (see [WebAssembly Build](wasm.md)) |

## format.py is not just clang-format

<!-- NOTE: it reorders includes (self-header first, blank line, then contiguous
     others) before running clang-format-20. Run it before every push. -->

## Common CI failures

<!-- NOTE: the pitfalls from CLAUDE.md — const pointers (cppcheck #1), include-
     what-you-use, include-block formatting, designated-initializer gaps,
     resource leaks on error paths, no debug prints. -->
