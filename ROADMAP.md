# MAGPIE Roadmap: Superhuman Scrabble Engine + Analysis Tool

## Vision

A browser-based Scrabble analysis tool and opponent powered by MAGPIE's C/WASM engine, with an interface rivaling the best chess GUIs (Lichess, En Croissant), LLM-narrated reasoning, learned evaluation, and intelligent time management. Built incrementally by one person on nights and weekends, leaning heavily on AI coding agents.

## Prior Art in This Repo

Before planning, take stock of what already exists:

**CLI analysis tool** — MAGPIE already has a complete game analysis workflow via the command-string API (`cmd_api.h`). The canonical pattern is:

```
Magpie *mp = magpie_create("./data");
magpie_run_sync(mp, "set -lex CSW21");
magpie_run_sync(mp, "load 54515");        // load from Woogles game ID, GCG, or URL
magpie_run_sync(mp, "goto 10");            // jump to turn 10
magpie_run_sync(mp, "g");                  // generate moves (hotkey)
magpie_run_sync(mp, "sim 1000");           // Monte Carlo sim, 1000 iterations
magpie_run_sync(mp, "shinference");        // show inference results
magpie_run_sync(mp, "next");               // step forward
magpie_run_sync(mp, "previous");           // step back
magpie_run_sync(mp, "endgame");            // solve endgame
magpie_run_sync(mp, "export annotated.gcg"); // save
char *output = magpie_get_last_command_output(mp);  // parse text result
```

All GUI work should follow this exact pattern: command strings in, text parsing out. The WASM layer (`wasmentry/api.c`) wraps the same `magpie_run_sync` interface. **Do not bypass the command API** for the web GUI — it's the tested, maintained contract.

**qtpie + Letterbox (Qt GUI branches)** — The `qtpie-oct2025` branch has ~14,500 lines across 105 commits. Two Qt C++ apps:
- `qtpie/`: Board rendering, rack view, game history panel, blank designation dialog, board renderer with colors — a substantial Scrabble GUI.
- `letterbox/`: Word study tool with alphagram boxes, completion stats, hover-aware text browser, sidebar with blank tables and anagram equations.

The qtpie `magpie_wrapper.h` takes a different approach from the command API — it reaches directly into `Game*`, `Board*`, `Config*` structs for things like `magpie_get_bonus_square()`, `magpie_board_get_letter_at()`, `magpie_validate_move()`, `magpie_play_move()`. This is appropriate for a native C++ GUI linked against the library but **not the right pattern for WASM**. For the web GUI, stick to command strings — the WASM boundary makes struct-level access impractical and the command API is the stable interface.

However, the qtpie code is valuable design reference for: board color schemes, tile rendering, game history layout, blank designation UX, and the overall information architecture.

**PEG solver (`jvc56/peg-solver` branch)** — Pre-EndGame solver, much more mature than a WIP sketch. 20+ commits covering: 1-bag and 2-bag support, parallelized pass evaluation, killer draws heuristic, aspiration windows, length-tiered move selection, bingo threat detection, per-thread solvers, early cutoff pruning, and a benchmark harness with multiple test positions. This is close to production-ready.

**Simmed inference (`sim_infer` branch)** — Inference with zero positional args using game history, renamed equity margin variables, config tests. This allows sim to use inferred opponent rack distributions instead of uniform random draws.

**Other notable branches**: `libmagpie` (library packaging), `bai`/`bai2`/`bai_ids` (Block-And-Infer algorithm iterations), `lockless-hashing` (lock-free transposition table), `lazy-smp-endgame`, various Claude-authored optimization branches.

## Architecture

```
┌─────────────────────────────────────────────────┐
│  React/TypeScript Frontend (Vite)               │
│  ┌───────────┐ ┌──────────┐ ┌────────────────┐ │
│  │ Board +   │ │ Analysis │ │ Variation Tree │ │
│  │ Rack UI   │ │ Panels   │ │ + Annotations  │ │
│  └─────┬─────┘ └────┬─────┘ └───────┬────────┘ │
│        └─────────────┼───────────────┘          │
│                      │ TypeScript API layer      │
│  ┌───────────────────┴──────────────────────┐   │
│  │  WASM Bridge (wraps wasm-worker.js)      │   │
│  │  Command queue, result parsing, events   │   │
│  └───────────────────┬──────────────────────┘   │
└──────────────────────┼──────────────────────────┘
                       │ postMessage / SharedArrayBuffer
┌──────────────────────┼──────────────────────────┐
│  MAGPIE WASM Worker (Emscripten + pthreads)     │
│  ┌──────────┐ ┌─────┐ ┌─────────┐ ┌──────────┐ │
│  │ MoveGen  │ │ Sim │ │Endgame  │ │Inference │ │
│  │          │ │     │ │  + PEG  │ │          │ │
│  └──────────┘ └─────┘ └─────────┘ └──────────┘ │
└─────────────────────────────────────────────────┘
                       │ (optional, for LLM features)
┌──────────────────────┼──────────────────────────┐
│  Lightweight Backend (explanation, NNUE serve)   │
│  Claude API for narrated analysis                │
└─────────────────────────────────────────────────┘
```

### Command API as the Universal Interface

The web GUI interacts with the engine **exclusively** through command strings, matching the CLI pattern:

```typescript
// TypeScript bridge — mirrors the CLI workflow exactly
const engine = await MagpieEngine.create('/data');
await engine.run('set -lex CSW21 -wmp true');
await engine.run('load 54515');           // Woogles game ID
await engine.run('goto 10');              // navigate to turn 10
const moves = await engine.run('g');      // generate moves
const sim = await engine.runAsync('sim 2000', {
  onStatus: (s) => updateProgress(s),     // poll wasm_get_status()
  onStop: () => engine.stop(),            // wasm_stop_command()
});
const output = engine.getOutput();        // wasm_get_output()
```

Under the hood this wraps `wasm-worker.js` postMessage. The async variant spawns via `wasm_run_command_async()` and polls `wasm_get_thread_status()` (0=uninitialized, 1=started, 2=user_interrupt, 3=finished) with `wasm_get_status()` for progress text.

### Key Command Reference for GUI

From the CLI's command registry in `config.c`:

| Command | Hotkey | GUI Use |
|---------|--------|---------|
| `set -lex CSW21 -wmp true` | — | Initial configuration |
| `load <gcg_path_or_woogles_id>` | — | Load a game for review |
| `new` | — | Start fresh game |
| `cgp <board> <racks> <turn> <scores>` | — | Set arbitrary position |
| `next` / `previous` | — | Step through turns |
| `goto <turn_or_keyword>` | — | Jump to turn ("start", "end", or number) |
| `rack <tiles>` / `rrack` | `r` | Set/randomize rack |
| `generate` | `g` | Generate all legal moves |
| `simulate [iters]` | `sim` | Monte Carlo simulation |
| `gsimulate [iters]` | `gs` | Generate + simulate in one call |
| `endgame` | — | Solve endgame (bag empty) |
| `infer [args...]` | — | Infer opponent rack |
| `commit <move_num>` | — | Play a move |
| `tcommit` | `t` | Commit top sim/static move |
| `shmoves [top_k]` | — | Show move list |
| `shgame` | `s` | Show game state |
| `shinference` | — | Show inference results |
| `shendgame` | — | Show endgame results |
| `heatmap <type>` | — | Tile placement heat map |
| `export [filename]` | `e` | Save as GCG |
| `note <text>` / `cnote <text>` | — | Annotate turn/move |

Key settings for the GUI to configure:
- `-numplays <N>` (default 100) — moves to generate
- `-plies <N>` (default 5) — simulation depth
- `-iterations <N>` — max sim iterations
- `-scondition <pct>` — stopping condition percentile
- `-threads <N>` — worker thread count
- `-hr true` — human-readable output (always on for GUI parsing)
- `-sinfer true/false` — sim with inference-weighted draws
- `-wmp true` — use word maps (always on for performance)
- `-ttfraction <0-1>` — transposition table memory budget

---

## Phase 1: Web GUI Foundation (Weeks 1–6)

**Goal:** A usable, beautiful board viewer and analysis tool. The highest-value work because it makes everything else visible and testable.

### 1A. WASM Bridge Layer (Week 1)

TypeScript module wrapping `wasm-worker.js`:

- **Command queue** with promise-based async interface. Commands are sequential (MAGPIE is single-command-at-a-time), so the bridge queues and serializes.
- **Output parsers** for each command type. The `src/str/` module defines every output format — `move_string.c`, `sim_string.c`, `endgame_string.c`, `inference_string.c`, `game_string.c`. An agent can read these files and write corresponding TypeScript parsers.
- **Status/progress callbacks** from the worker's polling mechanism. During sim/endgame, poll `wasm_get_status()` on a timer and parse the progress text (iteration count, nodes/sec, current best).
- **Lifecycle management**: WASM module load, data file fetching (lexica .wmp, leave values .klv), SharedArrayBuffer/COOP/COEP header requirements for pthreads.

Extremely vibe-codeable. The output formats are text-based and regular.

### 1B. Board and Rack Rendering (Weeks 1–2)

Reference the qtpie branch for design decisions (colors, tile shapes, premium square styling) but implement in SVG/React:

- **SVG board**: 15×15 grid with premium square colors. qtpie's `colors.h`/`colors.cpp` and `board_renderer.cpp` have the color scheme already worked out.
- **Tile rendering**: letter + subscript point value. Blank designation display (lowercase or indicator). qtpie's `board_view.cpp` handles this.
- **Rack component** below the board. Reference qtpie's `rack_view.cpp` for layout.
- **Move highlighting**: candidate move tiles in distinct color, hover to preview placement.
- **Responsive layout**: board scales to viewport. Two-panel: board left, analysis right.

Dark mode from day one. Aim for something between Lichess's clarity and Woogles' information density.

### 1C. Move List and Equity Panel (Week 2–3)

- **Sorted move table**: position, word, score, equity, leave value, win%. Clicking highlights on board.
- **Equity bar**: visual indicator of relative standing.
- **Score and bag tracker**: running scores, tiles remaining, unseen tile distribution.

Parse output from `shmoves` and `shgame` commands.

### 1D. Game Navigation (Week 3–4)

- **GCG import/export**: `load <path>` and `export <path>`. For WASM, file content goes through the virtual filesystem (`fileproxy.c` handles this).
- **Turn-by-turn navigation**: `next`, `previous`, `goto <n>`. Parse `shgame` output after each step.
- **Woogles game loading**: `load <game_id>` — MAGPIE already fetches from Woogles/xtables URLs.
- **Notation panel**: move history from `shgame` output, styled like qtpie's `game_history_panel.cpp`.
- **Position setup via CGP**: text field accepting CGP strings → `cgp <string>`.

### 1E. Simulation Integration (Week 4–5)

- **Analyze button**: runs `gs 2000` (generate + simulate) async. Poll status for live progress.
- **Live convergence display**: parse status messages for per-move equity estimates as iterations accumulate.
- **Heat map overlay**: `heatmap` command → parse output → render as board overlay with opacity-mapped colors.
- **Stop/resume**: `wasm_stop_command()` for cooperative cancellation.

### 1F. Endgame Viewer (Week 5–6)

- **Endgame solve trigger**: `endgame` command when bag is empty (auto-detect from game state).
- **PV line display**: parse `shendgame` for the principal variation with minimax score.
- **Variation tree**: collapsible tree of endgame lines. Start simple — indented move list with branching.
- **Iterative deepening progress**: poll status during solve for current depth and best score.

### Phase 1 Tech Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| Framework | React 19 + TypeScript | Massive AI training corpus = agents write it fluently |
| Build | Vite | Fast, good WASM/worker support |
| Styling | Tailwind CSS | Utility-first, agents handle it well, dark mode built-in |
| State | Zustand | Minimal boilerplate, good for command-response patterns |
| Board rendering | SVG (React components) | Crisp at all sizes, easy hit testing, animatable |
| Deployment | Static hosting (Vercel/Netlify) | WASM is fully client-side, no server needed |

**Why not Qt/native?** The qtpie work is substantial, but a web app means zero install friction, works on any OS, and the WASM engine already exists. The qtpie code is invaluable as design reference, and some of the Letterbox word study features could migrate to the web GUI later.

---

## Phase 2: Intelligence Layer (Weeks 7–14)

**Goal:** Merge WIP branches, add LLM narration, begin NNUE training pipeline.

### 2A. Merge Simmed Inference (Week 7–8)

The `sim_infer` branch adds inference using game history — no positional args needed, just `infer` after loading a game. This is essential for accurate opponent modeling.

GUI integration:
- **Inferred rack display**: parse `shinference` output → colored bar chart of opponent tile probabilities beside the bag tracker.
- **Sim with inference toggle**: `set -sinfer true/false` lets user switch between uniform random draws and inference-weighted draws.
- **Before/after comparison**: run sim twice (with and without `-sinfer`) and show side-by-side equity columns.

### 2B. Pre-Endgame Solver (Week 8–10)

The `jvc56/peg-solver` branch is already substantial: 1-bag and 2-bag support, parallel pass evaluation, killer draws heuristic, aspiration windows, bingo threat detection, benchmark harness. Integration work:

- **New command exposure**: PEG solver likely needs a command entry point in `config.c` (may already exist in the branch). Follow the existing `endgame` command pattern.
- **GUI trigger**: when bag ≤ 2 tiles, offer "Pre-endgame analysis" button alongside normal sim.
- **Outcome distribution display**: for each candidate move, show win%/loss%/draw% across all possible draws — not just expected equity. This is the killer feature no public tool exposes well.
- **Outcome matrix**: per-move table of possible draws (with probabilities) and resulting endgame scores. Color-code rows by outcome.

### 2C. LLM-Narrated Analysis (Week 10–11)

Pipe structured engine output through Claude's API:

- **Move comparison**: "FAQIR (8D, 52 pts) scores 11 more than FAIR but leaves FQ vs QI. Sim shows FAIR wins 2.3% more because the Q is easier to play with the I next turn."
- **Endgame narration**: "Forced win by 8. The key is ZAP at 15A first — blocks opponent's only out-play while scoring enough to maintain the lead."
- **Strategic commentary**: lane openings, hot spots, tile tracking implications.

Architecture: frontend collects structured analysis (top moves, sim stats, endgame PV) and sends as a prompt to Claude API. Streaming response in a chat-like panel. Cache by CGP hash. On-demand "explain this" button, not automatic.

**Cost**: ~$5–10/month for active personal use with caching.

### 2D. NNUE-Style Evaluation: Training Pipeline (Week 11–14)

**Eval target:** approximate simulation equity — the expected point spread after N plies of Monte Carlo sim. This lets the engine skip expensive simulation for routine positions.

**Feature set:**

```
Board features (per square, 225 total):
  - Tile occupancy: empty, A-Z, blank-as-letter (one-hot or embedding)
  - Premium square type and occupancy status

Derived board features:
  - Open triple lane count
  - Bingo line availability (rows/columns with 7+ open squares adjacent to tiles)
  - Board openness score

Rack features:
  - 27-dim tile count vector (A-Z + blank)
  - KLV leave value (scalar — strong hint from existing leave tables)
  - Vowel/consonant ratio
  - Duplicate tile count

Game state:
  - Score differential (millipoints)
  - Tiles in bag
  - Unseen tile distribution (27-dim)
  - Turn number
```

**Training data generation** via MAGPIE autoplay:

```bash
set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 1
autoplay games 100000 -threads 8 -seed 42
```

For each position, record (CGP, rack, best_move_equity, win_probability) in flat binary. 100K games × ~12 positions/game ≈ 1.2M samples per run.

**Network (start simple):**

```
Input (~300–500 dims) → Dense(512) → ReLU → Dense(256) → ReLU → Dense(128) → ReLU
  → equity_head: Dense(1) linear (millipoint spread)
  → win_head: Dense(1) sigmoid (win probability)
```

No conv layers initially — board features are sparse, premium geometry is fixed. MLP trained on enough data captures the important patterns.

**Training:** PyTorch → quantize to int8 → minimal C forward pass (<500 lines, no library deps) for WASM inference.

**Validation:** game pairs — NNUE-augmented vs. pure static equity, same tile draws:
```bash
set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 1
autoplay games 1000 -gp true -threads 8 -seed 42
```

### 2E. Clock Management (Week 12–14)

Rule-based first, learned later:

```
time_budget(position) = base_time × urgency × complexity

urgency:
  early game (turns 1–4): 0.5    (positions are forgiving)
  mid game (turns 5–10):  1.0
  pre-endgame (bag ≤ 7):  1.5    (critical decisions)
  endgame (bag = 0):      2.0    (solve if time permits)
  time pressure (<1 min): 0.3    (emergency)

complexity:
  equity spread of top 3 > 20 pts: 0.5  (clear best, don't overthink)
  equity spread of top 3 < 5 pts:  1.5  (close, sim more)
  open board:                      1.2  (tactical volatility)
  opponent bingo-likely leave:     1.3  (defensive considerations)
```

Implementation: a `TimeManager` that sets `-iterations` and `-tlim` based on the budget, monitors elapsed time, and calls `magpie_stop_current_command()` when budget exhausted.

---

## Phase 3: Polish and Advanced Features (Months 4–6)

**Goal:** Lean on improved AI coding capabilities (Claude 5.0 and beyond).

### 3A. Interactive Preendgame Explorer

The crown jewel analysis feature:

- **Outcome tree visualization**: expand any PEG position into a tree of possible draws and resulting endgames. Color-code branches by win/loss.
- **"What if" exploration**: click a draw → engine shows resulting endgame with full solve.
- **Probability-weighted outcome table**: per-move sorted table of draws with probabilities and scores.

Requires tight integration between PEG search and the GUI's tree component. UX design is subtle — information density without overwhelm. Good candidate for future agents.

### 3B. Study Mode

- **Puzzle generation**: extract interesting positions from autoplay (non-obvious best moves, equity swings, endgame puzzles).
- **Spaced repetition**: track user performance, resurface missed positions.
- **Opening explorer**: aggregate autoplay move statistics into a browsable opening book.

### 3C. Live Play Interface

- **Play against MAGPIE**: `new` → `rrack` → human plays → `tcommit` for engine. Configurable difficulty via `-numplays`, `-iterations` limits.
- **Real-time clock**: timed games with clock management model.
- **Post-game review**: `load` the just-finished game → auto-annotate every move with equity loss via `gsim` at each turn.

### 3D. Letterbox Word Study (Port from Qt)

The `letterbox/` Qt app has valuable features worth bringing to the web:
- Alphagram study with completion tracking
- Blank equation display (single and double blanks)
- Hover-aware word exploration with sidebar details

These can become a "Study" tab in the web GUI.

### 3E. NNUE v2: Richer Architecture

If the MLP plateaus:
- **Board CNN**: 15×15 input channels → 3×3 conv → pooled features → concat with rack/game → MLP head.
- **Rack attention**: 7 rack tiles as sequence through tiny transformer (QU synergy, duplicate penalty).
- **Inference-weighted training**: use inferred opponent racks during data generation.

### 3F. Collaborative Features

- **Shareable analysis links**: CGP in URL query params, analysis client-side.
- **Annotation system**: text notes on positions, exportable as annotated GCG.
- **Multiplayer analysis**: two players review a game together via WebRTC or WebSocket.

---

## Feasibility and Scheduling

### What AI Agents Handle Well Today

- React/TypeScript UI components (board, move lists, tree views)
- TypeScript output parsers from reading `src/str/*_string.c` format specs
- Tailwind styling and dark mode
- PyTorch training pipelines
- Small, well-scoped C changes with careful review

### What Needs Your Brain

- PEG solver algorithm correctness (subtle mathematical invariants in the branch)
- NNUE feature engineering (Scrabble domain expertise no model matches)
- Clock management policy tuning (playtesting and intuition)
- C engine architecture decisions (disciplined C99 + strict CI)
- Merging WIP branches that touch core engine code

### What to Save for Better Models (3+ Months)

- Complex interactive tree UIs (preendgame outcome explorer)
- NNUE architecture search and training curve analysis
- End-to-end Playwright tests for the full GUI
- SharedArrayBuffer zero-copy optimizations if postMessage bottlenecks
- Porting Letterbox word study features to web

### Rough Calendar

| Period | Focus | Agent-Assisted? |
|--------|-------|-----------------|
| **Weeks 1–2** | WASM bridge + board rendering (reference qtpie) | Heavy vibe-coding |
| **Weeks 3–4** | Move list, GCG/Woogles loading, game nav | Heavy vibe-coding |
| **Weeks 5–6** | Sim integration, endgame viewer | Moderate (UI) + careful review (engine) |
| **Weeks 7–8** | Merge `sim_infer` branch | Mostly manual, agent for tests |
| **Weeks 8–10** | Merge `jvc56/peg-solver` + GUI | Mostly manual, agent for GUI |
| **Weeks 10–11** | LLM narration integration | Heavy vibe-coding |
| **Weeks 11–14** | NNUE training pipeline + data gen | Agent for boilerplate, manual for features |
| **Weeks 12–14** | Clock management (rule-based) | Moderate |
| **Month 4+** | PEG explorer, study mode, Letterbox port, NNUE v2 | Better agents expected |

### Risk Mitigation

- **Follow the command API pattern.** Don't create a second API surface. If a command doesn't expose what the GUI needs, add a new command or setting to `config.c` — don't reach past the API.
- **Keep frontend decoupled from engine.** The WASM bridge is the contract boundary. If you ever want native backend or server-side analysis, the frontend shouldn't care.
- **Validate NNUE incrementally.** Start tiny. If it even slightly improves endgame move ordering (reducing nodes), that's worth shipping before bigger architectures.
- **Don't rewrite qtpie — learn from it.** The Qt code has 105 commits of design iteration. Mine it for UX decisions, color schemes, and edge cases.

---

## First Weekend Kickoff Checklist

1. `mkdir magpie-gui && cd magpie-gui && npm create vite@latest . -- --template react-ts`
2. Build WASM: `make -f Makefile-wasm magpie_wasm` → copy `magpie_wasm.mjs` + `wasm-worker.js` into `public/`
3. Copy required data files into `public/data/` (lexicon .wmp, leave values .klv, letter distributions)
4. Write `src/engine/bridge.ts` — async wrapper around the worker, matching CLI patterns:
   ```typescript
   await engine.run('set -lex CSW21 -wmp true');
   await engine.run('cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AQRTUYZ/ 0/0 0');
   await engine.run('g');
   const moves = engine.getOutput();  // parse this
   ```
5. Write `src/engine/parsers.ts` — read `src/str/move_string.c` and write a parser for the move list format
6. Write `src/components/Board.tsx` — 15×15 SVG grid, reference qtpie's `colors.cpp` for premium square colors
7. Wire it up: load WASM → generate → render moves on board
8. Deploy to Vercel (with proper COOP/COEP headers for SharedArrayBuffer)

Everything after that is iteration.
