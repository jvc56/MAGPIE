# Annotating Games

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/gameplay.c`, `src/impl/gcg.c`, `src/impl/get_gcg.c`
> **Related:** [GCG format](../formats/gcg.md), [CGP format](../formats/cgp.md)

<!-- NOTE: this is the most common interactive workflow. Adapt the README
     "Annotating a game" and "Analyzing a game from xtables/woogles" sections,
     but expand. -->

## Starting and configuring a game

<!-- NOTE: `set -lex`, `new`, player names (`p1`/`p2`), `switchnames`/`sw`. -->

## Making plays

<!-- NOTE: set rack (`r`), generate (`g`), commit (`c <n>`), top-commit (`t`).
     Explain what `t` commits (top sim move if available else top static). -->

## Challenges

<!-- NOTE: `challenge`/`chal`, `unchallenge`/`unchal` — phony removal vs.
     challenge bonus, and how unchallenge works without affecting later moves. -->

## Importing games

<!-- NOTE: `load <id>` from woogles/xtables; the `load` command's input types
     (see `help load`). -->

## Navigating

<!-- NOTE: `goto start|end|<n>`, combining with gsim/infer at each position. -->

## Exporting

<!-- NOTE: `export`/`e` → GCG, default filename behavior. -->
