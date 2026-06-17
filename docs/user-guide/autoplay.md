# Autoplay & Game Pairs

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/autoplay.c`, `src/impl/gameplay.c`
> **Related:** [Testing & Validation](../developer-guide/testing.md)

## Running self-play

<!-- NOTE: `autoplay games <n>` with per-player config (`-l1`/`-l2`, `-s1`/`-s2`,
     `-r1`/`-r2`, `-leaves`, `-threads`). Show the README CSW21 example. -->

## Game pairs

<!-- NOTE: `-gp true` — two variants play identical draws, alternating who goes
     first, so `games 1000` is 2000 games. Drastically reduces variance. Always
     recommended for comparisons. See `help gp`. -->

## Comparing lexica

<!-- NOTE: convert two word lists (`convert text2kwg`, `convert text2wordmap`),
     then `autoplay -l1 ... -l2 ...`. Reproduce the README CSW50 vs CSW60
     example. -->

## Output and progress

<!-- NOTE: `-hr true` human-readable, `-pfreq`, interpreting win%/spread with
     paired error bars. -->
