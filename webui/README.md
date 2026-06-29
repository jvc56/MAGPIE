# MAGPIE Web UI — Analysis Board

A mouse-oriented, browser-based analysis board for MAGPIE, running the engine
entirely client-side via WebAssembly. Set up a position by clicking, then
generate plays, run a Monte-Carlo simulation, solve the endgame, or run an
inference — all locally in the browser.

It is a sibling of the keyboard-driven notcurses TUI, translated to the mouse:
click a square to place tiles, click moves to preview them on the board, sort
the play list by clicking column headers.

## Quick start

```bash
# 1. Build the WASM artifacts (writes wasmentry/magpie_wasm.{mjs,wasm}).
#    Use a separate object dir so a native build's objects aren't clobbered.
make -f Makefile-wasm OBJ_DIR=obj-wasm magpie_wasm

# 2. Make sure the data files exist (lexica, leaves, winpct, layout).
./download_data.sh        # only needed on a fresh clone

# 3. Serve the repo root with the required COOP/COEP headers.
python3 webui/serve.py    # then open http://localhost:8080/webui/
```

`SharedArrayBuffer` (and therefore the engine's threads) requires the
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` headers; `serve.py` sends them.
Any other static host works too as long as it sends those two headers.

## Using it

- **Place tiles:** click a board square to set the entry cursor, click it again
  to flip between across/down, then click letters in the palette. Toggle
  **Blank** for a designated blank (rendered lowercase). **⌫** removes the last
  tile; right-clicking a square erases it. Typing on the keyboard works too.
- **Racks:** click a rack to focus it, then use the palette (letters / **Blank**
  for `?`), type directly, click a tile to remove it, or hit **Random**. Use
  **swap to move** to analyze the other player.
- **Analyze:** **Generate** lists plays; **Simulate** adds win% and mean equity;
  **Endgame** solves and highlights the best move; **Infer** runs an inference.
- **Plays:** hover a row to preview it on the board, click to pin the
  highlight, click column headers to sort, **Play** to apply it and advance.
- **Positions:** **Load** a built-in sample, **Copy CGP**, **Load CGP…** (an
  inline field — paste and press Enter), **New** for an empty board.

The look matches the MAGPIE TUI: amber wood tiles with point-value subscripts,
the on-turn rack and selection highlights in green, blanks in red italic. The
board auto-sizes so the whole UI fits the window without scrolling.

## How it works

The page drives the existing Web Worker (`wasmentry/wasm-worker.js`), which was
extended with three JSON accessors exported from the engine:

| Export                  | C entry point          | Returns                              |
| ----------------------- | ---------------------- | ------------------------------------ |
| `wasm_get_state_json`   | `json_api_get_state`   | board, premiums, racks, scores, CGP  |
| `wasm_get_moves_json`   | `json_api_get_moves`   | generated plays (+ sim win%/equity)  |
| `wasm_get_endgame_json` | `json_api_get_endgame` | endgame value/depth + best move      |

The board, racks, and scores are edited in the browser; before each analysis the
UI serializes them to a CGP string and syncs it into the engine, then reads the
structured JSON back to render. See `src/impl/json_api.{c,h}` for the schema.

## Embedding

The directory is self-contained and iframe-friendly. To embed it in a docs site,
serve `webui/` (with the engine artifacts and `data/`) under the COOP/COEP
headers above and point an `<iframe>` at `index.html`. The hosting page must
itself be cross-origin-isolated to embed a `require-corp` frame.

## Tests

Browser end-to-end tests live in `wasmentry/tests/webui.test.js` (Playwright)
and cover loading + generating, loading a CGP via the inline field, and playing
through a game. They reuse the existing Playwright config (which serves the repo
root with COOP/COEP):

```bash
cd wasmentry
npm install
npx playwright install chromium
npx playwright test tests/webui.test.js --project=chromium
```

## Notes / limitations

- Ships the English lexica MAGPIE has data files for (CSW21/24, NWL20/23,
  TWL06/14/98) on the standard 15x15 board. Other letter distributions/layouts
  are future work.
- **Play** advances the position locally (places the tiles, sets the mover's
  rack to its leave, flips the turn) rather than committing through the engine:
  the engine's `commit` command currently mis-validates plays that run through
  existing tiles. Advancing locally is also a better fit for analysis — you
  choose the next rack instead of a random draw.
- Inference is exposed but minimal; it shows the engine's text output.
