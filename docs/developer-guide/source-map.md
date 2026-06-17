# Source Map

> **Status:** outline — prose to be written.
> **Sources:** `src/`, `cmd/`, `test/`, `wasmentry/`

## Top-level layout

<!-- NOTE: orient a new contributor. Describe each tree. -->

| Path | Contents |
| ---- | -------- |
| `src/def/` | Constants and plain-data type definitions (`*_defs.h`) |
| `src/ent/` | Entities: board, game, kwg, klv, wmp, equity, letter distribution |
| `src/impl/` | Algorithms: move gen, sim, inference, endgame, peg, autoplay, convert |
| `src/str/` | String formatting / output |
| `src/util/` | Shared utilities (I/O, etc.) |
| `src/compat/` | Platform/compiler compatibility shims |
| `cmd/` | Executable entry point |
| `wasmentry/` | WebAssembly bindings |
| `test/` | Unit and on-demand tests |

## Where constants live

<!-- NOTE: enum constants at file scope; shared constants in src/def/*_defs.h;
     private ones in a file's top-of-file anonymous enum. From CLAUDE.md. -->

## Finding things

<!-- NOTE: pointer pattern — a feature's command is wired in config.c/exec.c,
     its algorithm in impl/, its data types in ent/ + def/, its tests in test/. -->
