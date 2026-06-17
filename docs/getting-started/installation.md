# Installation & Build

> **Status:** outline — prose to be written.
> **Sources:** `setup.sh`, `Makefile`, `Makefile-wasm`, `CLAUDE.md`

## Clone the repository

<!-- NOTE: `git clone https://github.com/jvc56/MAGPIE.git && cd MAGPIE`. -->

## One-command setup

<!-- NOTE: `./setup.sh` — what it does end to end (download data, convert
     lexica, build). Point out it's the fastest way to a working `./bin/magpie`. -->

## Build targets

<!-- NOTE: table of `make` targets. Stress: dev build is -O0 + ASAN/UBSAN and
     SLOW; use BUILD=release for anything non-trivial. Parallel make is
     automatic. -->

| Command | Result |
| ------- | ------ |
| `make magpie` | Dev build (ASAN/UBSAN, slow) |
| `make magpie BUILD=release` | Optimized build |
| `make magpie BUILD=profile` | Profiling build |
| `make magpie BUILD=thread` | ThreadSanitizer build |
| `make magpie_test` | Test binary |
| `make clean` | Remove build artifacts |

## Compile-time constants

<!-- NOTE: BOARD_DIM (default 15) and RACK_SIZE (default 7) are baked in at
     compile time, e.g. `make magpie BUILD=release BOARD_DIM=21`. A binary only
     accepts board layouts matching its BOARD_DIM. CI tests both 15 and 21. -->

## Platform notes

<!-- NOTE: macOS / Linux specifics, compiler requirements (C99 + the two C11
     exceptions: _Atomic and static_assert), pthreads. -->

## Verifying the build

<!-- NOTE: `./bin/magpie` should start the async interactive REPL. -->
