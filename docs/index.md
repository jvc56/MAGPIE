---
hide:
  - navigation
  - toc
---

<h1 class="wordmark" id="magpie">MAGPIE</h1>

<p class="tagline"><span class="ic">M</span>acondo <span class="ic">A</span>ccordant <span class="ic">G</span>ame <span class="ic">P</span>rogram and <span class="ic">I</span>nference <span class="ic">E</span>ngine</p>

<!-- NOTE (hero): one or two sentences positioning MAGPIE — a fast crossword-game
     playing and analysis engine in C. Lead with what makes it notable
     (speed, exhaustive endgame/PEG, BAI-driven simulation). Keep it punchy. -->

[Get Started](getting-started/index.md){ .md-button }
[How It Works](algorithms/index.md){ .md-button }
[View on GitHub](https://github.com/jvc56/MAGPIE){ .md-button }

## What MAGPIE does

<!-- NOTE: short framing paragraph, then the feature grid below. Mirror the
     README feature list but link each to its deep page. -->

<div class="grid cards premium" markdown>

-   [**Fast move generation**<span class="tags"><span class="tag">shadow</span><span class="tag">gaddag</span><span class="tag">wordmap</span><span class="tag">rack info table</span></span>](algorithms/move-generation.md)
-   [**Monte Carlo simulation**<span class="tags"><span class="tag">top2 bai</span><span class="tag">round robin</span><span class="tag">inference-weighted<br>sampling</span></span>](algorithms/simulation-and-bai.md)
-   [**Exhaustive inference**<span class="tags"><span class="tag">complete enumeration</span><span class="tag">fuzzy equity margin</span><span class="tag">exchange-aware</span></span>](algorithms/inference.md)
-   [**Parallel endgame and pre-endgame**<span class="tags"><span class="tag">abdada negamax</span><span class="tag">greedy playout</span><span class="tag">up to 4-in-bag</span><span class="tag">pruned wordlist</span></span>](algorithms/endgame.md)
-   [**Autoplay Eval**<span class="tags"><span class="tag">paired games</span><span class="tag">divergent results</span><span class="tag">win% + spread</span></span>](user-guide/autoplay.md)
-   [**Superleave generation**<span class="tags"><span class="tag">self-play learning</span><span class="tag">forced rare racks</span><span class="tag">multi-generation</span></span>](user-guide/superleaves.md)

</div>

## Lineage

<!-- NOTE: brief — MAGPIE began as a C rewrite of Macondo and adopts concepts
     from wolges (shadow play, KWG/KLV). See About → Background for the full
     story. Link out; don't duplicate. -->

MAGPIE began as a C rewrite of [Macondo](https://github.com/domino14/macondo)
and adopts data structures and ideas from
[wolges](https://github.com/andy-k/wolges). See
[Background & Lineage](about/background.md) for the full story.

## Where to go next

<!-- NOTE: signpost the three reader paths: install/use, understand algorithms,
     hack on the code. -->

- **Use it** → [Getting Started](getting-started/index.md) · [User Guide](user-guide/index.md)
- **Understand it** → [Algorithms & Internals](algorithms/index.md) · [Data & Formats](formats/index.md)
- **Build on it** → [Developer Guide](developer-guide/index.md) · [Reference](reference/index.md)
