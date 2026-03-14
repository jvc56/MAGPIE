# CLAUDE.md - MAGPIE Project Guide

MAGPIE (Macondo Accordant Game Program and Inference Engine) is a high-performance Scrabble engine written in C99.

## Build

```bash
make magpie                    # Dev build (debug + sanitizers)
make magpie BUILD=release      # Release build (-O3, -flto, -march=native)
make magpie_test               # Build unit tests
make magpie_test BUILD=release # Build unit tests in release mode
```

Build options via `BUILD=`: `dev` (default), `release`, `thread`, `vlg`, `cov`, `profile`, `test_release`, `dll_dev`, `dll_release`

Compile-time config: `BOARD_DIM=15` (or 21), `RACK_SIZE=7`

WebAssembly: `make -f Makefile-wasm magpie_wasm`

## Test

```bash
./run u              # Run all unit tests (release then dev)
./run u 21           # Run tests with BOARD_DIM=21
./run c              # Coverage report
./run g [test_name]  # Debug single test with gdb
./run v [test_name]  # Valgrind memory check
```

## Lint / Format / Static Analysis

```bash
python3 ./format.py          # Check clang-format-20 formatting
./cppcheck.sh                # Run cppcheck static analysis
./tidy.sh                    # Run clang-tidy
python3 find_circ_deps.py    # Check for circular header dependencies
./run s                      # Static analysis with scan-build
```

## Project Structure

```
src/
  ent/     - Entity/data structure headers (Board, Move, Game, Rack, etc.)
  impl/    - Implementation files (core algorithms)
  def/     - Type definitions and enums (_defs.h files)
  util/    - Utility functions (string, math, I/O)
  str/     - String/command execution
  compat/  - Platform abstractions (threading, time, endian)
cmd/       - Main entry point (magpie.c)
test/      - Unit tests (89 test files)
wasmentry/ - WebAssembly entry point and browser integration
data/      - Lexicon, letter distributions, layouts, strategy data
docs/      - Design documents
```

## Coding Conventions

- C99 standard with POSIX C 2008 extensions
- Strict warnings: `-Wall -Wextra -Wshadow -Wstrict-prototypes -Werror`
- Header guards: `#ifndef FILENAME_H` / `#define FILENAME_H` / `#endif`
- Format with clang-format-20
- Corresponding .c/.h include placed first in .c files
- No empty lines within include blocks
- No circular header dependencies
- Typedef structs for all data structures
- Sanitizers (ASAN, UBSAN) enabled in dev builds

## CI Checks

All of these must pass: cppcheck, clang-tidy, clang-format, circular dependency check, unit tests, WASM tests.

## Data

Data files are downloaded separately via `./download_data.sh` into `data/` (lexica, letter distributions, board layouts, strategy tables). Not checked into the repo.
