# Execution Modes

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/exec.c`, `src/impl/cmd_api.c`

## Script mode

<!-- NOTE: runs a single command then exits. Conditions: a command other than
     `set`/`cgp`, and no `-mode` specified. -->

## Interactive modes (REPL)

### Asynchronous (default)

<!-- NOTE: `-mode async`. Long-running commands can be polled (`status`/`sta`)
     and interrupted (`stop`/`sto`). Show the sim example from the README. -->

### Synchronous

<!-- NOTE: `-mode sync`. Blocks until done; recommended for programmatic clients
     to avoid per-command startup overhead. -->

## Library API

<!-- NOTE: every command has a `str_api_*` counterpart with signature
     `char* func(Config*, ErrorStack*, char* cmd)` — same parser, returns a
     string instead of printing. Currently no clients. -->

## Choosing a mode

<!-- NOTE: small decision table — humans → async; embedding → sync/library;
     one-shot scripts → script mode. -->
