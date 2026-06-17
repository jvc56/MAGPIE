# Developer Guide

> **Status:** outline — prose to be written.

<!-- NOTE: for contributors. Pull authoritative content from CLAUDE.md (build,
     CI, style, pitfalls) but rewrite for an external audience. Don't duplicate
     CLAUDE.md verbatim — link to it as the source of truth for agent/dev rules
     where appropriate. -->

## Start here

<div class="grid cards" markdown>

-   :material-map: **[Source Map](source-map.md)** — how `src/` is laid out.
-   :material-cog: **[Building & CI](building-and-ci.md)** — make targets and the six CI jobs.
-   :material-test-tube: **[Testing & Validation](testing.md)** — unit tests and game-pair methodology.
-   :material-format-paint: **[Style & Conventions](style.md)** — C standard, naming, pitfalls.
-   :material-web: **[WebAssembly Build](wasm.md)** — Emscripten + pthreads.
-   :material-source-pull: **[Contributing](contributing.md)** — focused PRs.

</div>

## Development methodology

<!-- NOTE: the key principle from CLAUDE.md — keep performance work and quality
     work separate. Performance: validate no behavior change (autoplay /
     on-demand tests, profile builds). Quality: validate with game pairs. When
     speed *is* quality (time-limited endgame/PEG benchmarks). -->
