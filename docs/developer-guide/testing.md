# Testing & Validation

> **Status:** outline — prose to be written.
> **Sources:** `test/test.c`, `Makefile`
> **Related:** [Autoplay](../user-guide/autoplay.md)

## Unit tests

<!-- NOTE: `make magpie_test`, run by short key: `./bin/magpie_test movegen`.
     Test keys live in test/test.c. CI runs BOARD_DIM 15 and 21. On-demand tests
     are in on_demand_test_table[]. -->

## Validation methodology

<!-- NOTE: the project's two-track principle. -->

### Performance work

<!-- NOTE: must not change behavior; validate with autoplay / on-demand tests
     comparing before/after; profile with BUILD=profile + `sample <pid>` on
     macOS; measure the specific functions changed. -->

### Quality work

<!-- NOTE: validate with game pairs — two variants play identical draws,
     alternating first. Give the canonical autoplay invocation. -->

```text
set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 1
autoplay games 1000 -gp true -threads 8 -seed 42
```

### When speed *is* quality

<!-- NOTE: under a time limit (endgame/PEG benchmarks), faster code finds better
     answers; measure quality under a time budget. -->

## Endgame benchmarking & transposition tables

<!-- NOTE: when comparing two solver modes, the second unfairly benefits from the
     first's TT cache — use separate TTs or reset between solves. TT memory ≤ 50%
     RAM (0.5 for one, 0.25 each for two). -->
