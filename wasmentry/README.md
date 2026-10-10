# Magpie for the web (beta)

A client-only interface for playing against the computer, reviewing games,
kibitz, Monte Carlo sims, pre-endgame (PEG), and endgame analysis. Open `wasmentry/` on a server configured below.
The existing `test-worker.html` remains available for diagnostics.

## Build and run

Install Emscripten (tested with 4.0.16) and activate its environment, then:

```sh
./download_data.sh
make -f Makefile-wasm magpie_wasm
python3 wasmentry/serve_preview.py --directory .
```

Open <http://localhost:8000/wasmentry/>. The production build uses Closure
Compiler and requires Java; `CLOSURE=0` skips it for local development.
The browser must support WebAssembly threads and SharedArrayBuffer. HTTPS
(or localhost) and cross-origin isolation are required. The UI uses the server's
COOP/COEP headers when present. On an ordinary static host it installs the
same-origin `isolation-worker.js`, reloads once, and then starts the WASM engine.
That service worker only adds isolation headers: it does not cache assets,
change authentication, or run analysis. Keep both isolation files in the bundle.
If service workers are unavailable, configure the server headers below. Embedded
previews must be opened in their own tab to enable cross-origin isolation.

## Use

- **Game** contains GCG open/save/copy and CGP paste/copy. **Edit position**
  contains examples, recent positions, rack/score inputs and board editing.
- Edit racks/scores or enable **Edit board**: click a square and type, use Shift
  for a blank, Space to switch direction, arrows to move, Backspace to erase.
  **Undo edit** restores the previous position. Choose a tile owner when entering
  tiles, or use **Assign owner** on an existing tile. Player 1 uses Qt gold and
  Player 2 green; unknown owners are neutral. Blanks use Qt’s outlined letter
  with no score subscript. **On turn** chooses the player color for rack/preview
  tiles; it does not exchange the racks or scores. Ownership survives undo and
  recent-position recall, but CGP import/export cannot encode it.
- **Kibitz** ranks up to 100 candidates by static equity. **Sim** compares up
  to 100 candidates (default 50) with win percentage, equity and rollout counts.
- Exactly one search is shown alongside Kibitz: **Sim** with more than four
  tiles, **PEG** with 1–4, and **Endgame** with an empty
  bag; the remaining opponent tiles can then be deduced from the inventory.
- Select a candidate to preview its tiles. Edits clear analysis for the old
  position. **Stop** / Escape cancels the current search and retains partial
  results. Letter keys are reserved for tile entry; use the analysis buttons
  to start searches. On wide screens, global navigation moves into sidebars
  alongside the position and analysis panels.
- **Settings** replaces the game view with Appearance, Lexicon, Board and Analysis categories,
  following the TUI's settings organization. **Back to game** / Escape returns
  to the same position, candidate preview and results. Premium labels default
  to hidden; their preference is saved in this browser. The Appearance category
  offers Dark, Light and Browser / system themes, with the selection saved locally. **Search settings**
  opens the Analysis category directly. During a search, its controls are
  disabled while navigation and board preferences remain available.
  **Lexicon** chooses CSW24 or NWL23 for the current position. Changing it
  preserves the board, racks and scores and clears previous analysis. The
  current lexicon in the game header opens this category directly.
- Searches default to 10 seconds and at most four analysis threads. The thread
  setting allows up to the browser-reported logical CPU count, capped at 32.
  Sims have a 500-iteration minimum before statistical stopping; time limits and manual
  stops can end them earlier. Time-limited PEG/endgame results can be incomplete;
  **Details** exposes search notes, depth, status and engine diagnostics.

**New game** defaults to **Play Magpie**, with **Record a game** and
**Watch computers** alongside it. Player names default to **You** and **Magpie**.
Names and time are immediately available; **Game options** holds first player,
human side, thinking time, overtime and challenge rules. Watch delay appears
only for computer-vs-computer games. The scoreboard keeps each player’s name,
score, clock and turn indicator in a fixed place.
Record mode uses manual racks. Play and Watch draw/refill both racks using the
native engine, select moves using static equity or the applicable sim/solver,
and preserve complete history. Setup includes names, first player, human side,
thinking time, watch delay, clocks, overtime and challenge rules. Pause stops
the clock and computer; Watch also has a single-turn step. The human rack stays
visible while the computer thinks. Analysis is hidden during Play until revealed.
Void rejects phonies; other challenge rules automatically remove invalid human
plays, as in the current TUI (the computer only plays legal words). **Game → Open GCG** accepts pasted game text, a dropped file, or an explicitly
chosen file. Paste and drop bypass the native file chooser. A file read that
stalls for 10 seconds reports an error and lets you retry or paste instead.
File reading, decoding and preliminary validation run in a dedicated, short-lived
worker, terminated on completion, failure or timeout. Native GCG parsing stays in
the existing engine worker. This keeps file preparation off the UI thread; it
cannot unblock a stalled operating-system file chooser.
**Open game → Loading diagnostics → Copy log** captures chooser requests,
selection/cancellation, focus changes, loader stages and engine import progress.
The bounded log is stored locally across reloads and browser restarts, and also
appears in the console under `[Magpie GCG]`. It excludes filenames and game text;
errors may include engine diagnostic details. Nothing is uploaded automatically.
A chooser request with no selection/cancellation or loader messages locates the
stall before the application receives a file; it cannot diagnose the native
picker's internal state.
Files stay on the device.

Move entry follows the Qt and TUI workflows:

- Click a square and type. Existing tiles are skipped; Space changes direction,
  Shift+letter forces a blank, Backspace removes the last pending tile, Escape
  recalls tiles, and Enter commits. A blank is used automatically when the
  natural letter is exhausted.
- Drag rack tiles onto the board with mouse, pen or touch, or tap a rack tile then
  a square. Dropping a blank opens a letter picker. Pending tiles can move on the
  board or return to the rack. The rack can be shuffled or sorted.
- The Move field accepts `8H TRAIN`, `H8 TRAIN`, `ex AE`, and `pass`. The engine
  previews the score and validates/scores commits. A phony requires explicit
  confirmation to record; the Notes & challenge panel can challenge it off.
- An analysis candidate has a **Play selected move** action. History updates,
  ownership, scores, racks and GCG serialization use the native engine.
- **Edit this move** opens the recorded move and rack for replacement. Playing
  from an earlier position asks before replacing later events. **Undo game
  change** restores the previous complete record (up to 20 changes). Notes can
  be edited without discarding later events.

**Save GCG** downloads the complete current record, regardless of which event is
selected. Raw board/score edits remain separate analysis experiments; Copy CGP
exports those edits. Return to a recorded board before committing a move. Loading
a CGP, recent position or example closes the record. Unsupported lexica and
malformed files leave the current game intact.

Review uses a two-player history table, first/previous/next/last navigation and
optional hidden upcoming moves. Each analyzed position retains its results for
this session. Auto-analyze and Analyze game run the appropriate sim/solver;
Stop review keeps completed results. Play/Watch from here branch from a selected
position, with the previous complete record available through Undo game change.
Save GCG and Copy GCG include the complete game, including time penalties.
Resignations and forfeits use a `Magpie finished:` GCG description marker;
browser import preserves their final overtime totals on incomplete boards.

Simulation tables include per-ply score averages, standard errors, rollout
counts, and highlighted best metrics. Continue sim combines independent new
rollout batches with the saved sample counts, means and uncertainty (rather
than retaining native worker search objects across positions). PEG shows
win/tie/loss counts and search fidelity. Endgames show outcome, spread, gain
and clickable continuation previews. Played-move rank/loss is shown when the
move is in the candidate list and spoilers are enabled. Analysis snapshots and
clocks stay in the current page session; GCG carries moves, notes and scores.

The preview supports the standard 15×15 English board, CSW24 and NWL23.
Inference, bulk engine autoplay and multilingual tile entry are not exposed.
Analysis runs entirely on the device; recent CGPs are stored in localStorage.
Lexicon and engine assets are downloaded from the same server when needed.

## Package and deploy

```sh
python3 wasmentry/package_preview.py /tmp/magpie-preview
python3 /tmp/magpie-preview/serve_preview.py
```

The **Test WASM Build** workflow produces a `magpie-wasm-preview` artifact on
pull requests, including drafts. Download and extract it, then run the included
`serve_preview.py`, or publish its contents to a static HTTPS host. It contains
only the UI, engine and required English data, not test data or the full repo.
The destination directory must be empty.

Prefer serving all routes with these response headers:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Resource-Policy: same-origin
```

The bundle includes `_headers` for hosts that support that format (for example,
Cloudflare Pages and Netlify). Other hosts need equivalent configuration.
Serve `.mjs` as JavaScript and `.wasm` as `application/wasm`. Hosts that ignore `_headers` (including the initial Sites static deployment)
use the service-worker startup path instead. The heap starts at 256 MB and can grow to 1 GB for parallel PEG searches.
The initial pthread pool is capped at seven workers and grows on demand when
more analysis threads are selected; higher thread limits do not increase the
startup pool. Mobile defaults use at most four analysis threads (two when the
browser reports at most 4 GiB of device RAM or supplies no RAM hint), with a matching smaller initial
pool. Device hints are defaults, not measurements of free RAM.

Search memory can be set to 16 or 32 MiB; the default is 32 MiB, or 16 MiB on
those constrained mobile devices. Endgame uses one shared table. Browser PEG
splits an aggregate budget across workers and its helper, including each table's
ABDADA array, Zobrist allocations and metadata. The previous endgame table is released before PEG starts. Native engine sizing is unchanged. WMP remains
explicitly opt-in; cached files do not automatically enable it. Physical-phone
peak-memory testing is still needed before enabling WMP by default on mobile.

No deployment tokens are required in
PR CI; publishing the artifact uses the chosen host's normal deployment flow.

### Versioned app releases for Birdtest

The **Release WASM preview** workflow owns the deployable app versions in this
repository. Dispatch it on the revision to publish with a new tag such as
`wasm-preview-v0.1.0`. It builds with the Emscripten version and MAGPIE-DATA
revision pinned in `release-build.json`, fetches only the seven required data
files, and tests the packaged app with service workers blocked. A successful
run publishes a GitHub prerelease with `magpie-wasm-preview.tar.gz` and its
SHA-256 checksum. Never overwrite an existing version.

The archive's `release.json` records the source/data revisions, toolchain and
each web asset's checksum. Birdtest downloads a chosen release, verifies it,
and deploys it under an immutable version path on **magpie.birdtest.org**.
Birdtest owns the subdomain, HTTPS/isolation headers and activation/rollback;
the UI, engine, data packaging and releases stay here. Publishing a release
does not deploy it, and this workflow needs no AWS credentials.

After installing the browser test dependencies, check an unpacked or deployed
release with:

```sh
node wasmentry/qa/release-smoke.mjs https://magpie.birdtest.org/
```

This checks direct isolation headers, first-load startup without a reload,
all four search modes, reload and a second tab with service workers blocked.

## UI references

This is a browser adaptation of existing Magpie UI work, using the engine on
`main`, without pulling either native UI's dependencies into the WASM build:

- The TUI stack through [#735](https://github.com/jvc56/MAGPIE/pull/735),
  `tui-rack-multiletter` at `4867207bfa798d371488440195925fee1318c3c4`:
  position analysis on either turn (#711), PEG/endgame routing (#722), bounded
  searches (#723), and a 500-rollout simulation floor (#733). Candidate preview,
  cancellation and position-bound results follow the TUI interaction model.
- The latest Qt branch, `feature/qt-recent-games` at
  `b1dc1f4004816860153f087c4b7e85880402c550`: board/rack/analysis arrangement,
  board and tile colors from `src/qt/views/Main.qml`, and recent-position
  access adapted from its recent-game work.
- Merged [#705](https://github.com/jvc56/MAGPIE/pull/705) supplies `Square` owner
  storage. The UI keeps the same 0/1/unknown convention in local
  snapshots and derives owners from recorded GCG events, including challenge
  removals. It does not infer owners from a CGP or modify analysis board copies.

## Tests

```sh
cd wasmentry
npm install
npx playwright install chromium firefox webkit
npm test
```

Browser tests run the actual WASM engine for all four modes, verify cancellation
and reuse, CGP validation, board editing/undo, stale-result clearing, candidate
previews and mobile layout. The UI suite also runs against an ordinary static
server without isolation headers, checks the single startup reload and real
WASM calculations, and uses no browser flags that relax SharedArrayBuffer
requirements. Diagnostic-page simulation tests remain in the suite.

The full UI suite runs in Chromium. A focused engine suite also runs in Firefox
and Playwright WebKit: startup, move generation, constrained heap reservations,
WMP cancellation, and cached WMP/WIT allocation failure followed by baseline
move generation and simulation. These tests use real WASM with injected failures;
a mobile viewport and user-agent override do not reproduce a phone's RAM pressure.
Playwright WebKit is not the shipping Safari application.

CI also installs Microsoft Edge and runs that engine suite with its `msedge`
channel. To include branded Edge locally, install it with
`npx playwright install msedge`, then run
`MAGPIE_TEST_EDGE=1 npx playwright test --project=edge`.
Before release, separately check real iPhone/iPad Safari and a low-memory Android
Chrome device, including background/resume and memory pressure. Browser-process
termination by the OS cannot be caught by the page.

## Playing UI audit games with the native playfinder

Build the CLI with `make magpie BUILD=no_pgo_release`, install the browser
requirements above, and start the localhost preview. In another terminal:

```sh
node wasmentry/qa/playfinder.mjs '15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0 -lex CSW24'
node wasmentry/qa/play-games.mjs --games 10 --output /tmp/magpie-blitz-qa
```

The playfinder returns JSON using the native CLI's best static-equity move.
The game driver reads the visible board/rack and enters those moves through
normal browser controls, against the browser's Sim / solve computer. It uses
real three-minute clocks, no overtime, five seconds per computer move, and
alternates first player and desktop/phone layouts. It exercises notation,
tap-to-place, board typing, blanks, exchanges, pause/settings/resume, history,
and GCG save/reload. It never supplies the hidden opposing rack to the CLI.
This audits interaction reliability, not playing strength or engine speed.

Each game produces GCG, JSON diagnostics and a final screenshot. On failure it
saves the partial record and screenshot, then stops for investigation. Use
`--start N` to resume the numbered series after preserving the failed attempt;
`--url` selects another locally served build. No clocks are fast-forwarded.

To audit postmortem review of those saved games:

```sh
node wasmentry/qa/review-games.mjs --input /tmp/magpie-blitz-qa --output /tmp/magpie-review-qa --games 10
```

This selects every decision in each GCG and runs its bag-appropriate search,
using one second, ten sim candidates, and four plies. It checks errors, board
preservation, played-move inclusion in every mode, cached results and mode selection,
continuing simulations, candidate previews, endgame PV navigation, and one
whole-game review. Desktop and phone layouts alternate. JSON reports include
engine commands and displayed results; screenshots cover the first occurrence
of each phase in each game. `--url` selects another locally served build.
The short searches exercise the UI and are not a playing-strength evaluation.

Review always requests the played move alongside the search candidates. Sims
add it beyond the static cutoff and protect it from pruning, including at the
100-candidate setting. PEG protects it through the halving stages. Endgame
uses the solver's full-window actual-move evaluation and appends its principal
variation when it falls outside the displayed top five. Completed live games
hide the opponent rack during review until the bag is empty, matching GCG
imports; users can still enter an explicit known rack in position settings.

PEG streams a structured snapshot alongside the machine-readable table. The
web view shows each stage's completed/total count, depth, retained/pruned
counts, and elapsed time. Candidates retain their last measured values while
queued for a deeper stage; the current candidate is marked Evaluating, and
pruned rows stay visible. Different search depths are labeled explicitly and
are not compared as a played-move loss. Stage progress and rows restore with
the position's cached analysis.

On phones, unseen-tile totals remain visible while the full pool starts collapsed.
Expand it to see the same vowel-first, one-row-per-letter tracking as the desktop sidebar.

### Experimental word maps

Settings → Analysis offers **Off** (the default) and **Build on this device**.
WMP and matching WIT preparation run on a coordinator pthread. The WMP builder
uses up to 14 threads, bounded by the Threads setting. Progress reports the
build stages; Stop discards a native build after its preparation finishes.
Ordinary startup and GCG import never require WMP, WIT or RIT.

**Cache WMP and WIT on this device** is opt-in. The cache section lists
**CSW24** and **NWL23** independently, showing each table's size in MiB and
combined exact bytes in the tooltip. Cache builds and saves either lexicon
without running analysis. Both tables are saved atomically in IndexedDB as
Blobs and reused after reload. Remove deletes both tables for that lexicon
and releases its active engine tables, leaving the other lexicon cached.
Clearing browser site data also clears these files. Browser storage is
origin-specific and may be evicted. Existing WMP-only caches remain readable;
when caching is enabled, a locally built WIT is added on the next use.
The checkbox controls new writes; existing cached tables remain reusable.
Cache failures (including quota limits) are shown without failing analysis.

WIT pruning is enabled with WMP; RIT remains off. Only one lexicon's tables
are resident in the engine at a time. Cached files are checked against their
SHA-256 and the loaded KWG's SHA-256 before native parsing. A missing or invalid
cache is rebuilt locally. No word-map manifest or data download is needed.

The network WMP implementation is retained for developer use through
`EngineClient.prepareWMP(lexicon, "download", threads, cache)`. It is not exposed
in the UI, and release packages do not include hosted WMP data. The download
path still validates same-origin, content-addressed gzip parts individually
and after assembly. To generate an optional package for testing that API:

```sh
make -j2 magpie BUILD=no_pgo_release
python3 wasmentry/build_wmp_assets.py --threads 4
python3 wasmentry/package_preview.py preview-dist --with-wmp
```

Browser CI builds download fixtures to test the retained API, but packages the
normal release without them. WMPs use format 3 and board dimension 15. Generated
files remain outside Git.

### Recovery and optional word-map caching

A saved word-map preference falls back to ordinary move generation if the browser
can reserve less than a 1 GiB WASM heap. A null allocation while copying a cached
WMP or WIT into WASM also falls back after freeing partial input buffers, keeps the
saved files, and avoids retrying the same unavailable map on every analysis.
A native abort during construction still stops the engine and requires a reload;
this fallback does not cover OS termination of the tab. Cache records include the builder version,
lexicon hash and binary format. An incompatible cached installation is removed and
rebuilt once; transient storage failures do not delete saved maps. If WebKit
rejects storing Blob/File data, saving retries atomically with typed arrays. Stop requests
interrupt WMP construction between word-length groups; a WIT build already in
progress must finish before cancellation completes.

Game exports remain available after a fatal engine error. Native game imports still
use a watchdog that terminates an unresponsive worker; they do not yet support
cooperative cancellation. Starting another live game requires confirmation.
Computer simulations pool partial results from the same position after pause and
resume, just as manual Continue sim does.
