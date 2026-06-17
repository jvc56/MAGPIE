# Algorithms & Internals

> **Status:** outline — prose to be written.

<!-- NOTE: this is the flagship "how it works" section — the depth that
     distinguishes MAGPIE's docs from thin engine READMEs. Each page should
     explain the algorithm, cite the source files and any white papers, and use
     a diagram (Mermaid) or formula (KaTeX) where it earns its place.

     Suggested per-page shape:
       1. Problem — what this subsystem computes and why it's hard.
       2. Approach — the algorithm/data structure, with a diagram.
       3. Implementation — key files, data layout, hot paths.
       4. References — papers and upstream projects. -->

## Reading order

<!-- NOTE: recommend a path. Word graphs underpin move generation; move
     generation + leaves/equity underpin simulation; endgame underpins PEG. -->

1. [Architecture](architecture.md) — the subsystem map.
2. [Word Graphs](word-graphs.md) → [Move Generation](move-generation.md) →
   [Leaves & Equity](leaves-and-equity.md)
3. [Simulation & BAI](simulation-and-bai.md) → [Inference](inference.md)
4. [Endgame](endgame.md) → [Pre-Endgame](pre-endgame.md) ·
   [Word Pruning](word-pruning.md)

## Conventions used in this section

!!! note "Equity is millipoints"
    Throughout the code, `Equity` (`int32_t`) is measured in **millipoints**:
    42 points is `42000`. See [Leaves & Equity](leaves-and-equity.md) and
    `src/def/equity_defs.h`.

<!-- NOTE: also note the "machine letter" (`ml`) convention, board coordinates,
     and that BOARD_DIM/RACK_SIZE are compile-time constants. -->
