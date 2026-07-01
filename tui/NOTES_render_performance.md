# TUI render-loop performance notes

Rules, diagnostics, and incident history for keeping the render loop at
60fps. Read this before adding ANY per-frame work to `tui_game_render`
or its callees.

## The frame budget

The render loop targets 60fps (16.6ms/frame). The 2x pixel board's
budget is dominated by per-tile compose+blit (~1.1ms/tile measured), so
the architecture is built around doing close to ZERO work on an
unchanged frame:

- **Conditional render** (main.c): a frame renders only when input
  arrived, `render_version` bumped, a modal is open, the wall-second
  ticked, or the bot spinner is animating. An idle screen renders ~1fps.
- **Content-keyed per-tile caches** (game_render.c): every board cell,
  rack slot, and the edit arrow has its own pixel plane, re-blitted only
  when its cache key (letter, owner, score, preview flag, geometry,
  theme inputs) changes. A steady frame re-blits 0 tiles.
- **Targeted erase**: at 2x the std plane is erased only OUTSIDE the
  board+rack region so notcurses elides the unchanged sprixels. A
  blanket `ncplane_erase` re-emits all ~225 board sprixels (~250ms).
- **Edge-triggered geometry recovery**: `tui_sync_plane_to_terminal`
  acts only when the ioctl winsize CHANGES; the cdy/cdx drift detector
  only when the probed cell-pixel size CHANGES. Level-triggered
  versions of either re-run full-screen recovery every frame (this was
  the original 4fps bug — see incidents).

## Rules (each one earned by an incident)

1. **No per-frame work without a consumer.** Gate preparation work on
   whether anything will render it. The Analysis panel is hidden for
   the whole live play-vs-computer game, but its row builder still ran
   every frame — see incident #2. If a panel can be hidden, its data
   preparation must check the same condition the layout uses.

2. **Treat the render thread's allocations as ~100x more expensive
   while the engine is searching.** The bot's search threads saturate
   every core AND hammer malloc. Render-thread `strdup`/`free` churn
   that costs microseconds in isolation costs milliseconds under that
   contention (measured: ~280ms for ~500 small allocations/frame).
   Prefer fixed buffers / snprintf into stack arrays for anything that
   runs per frame; if engine string helpers must be used
   (`string_builder_*`, `ld_ml_to_hl`), don't call them at frame rate.

3. **Throttle anything derived from live search results.** A sim /
   endgame leaderboard changes meaningfully a few times per second, not
   60. `populate_frame_analysis_rows` rebuilds live rows at most every
   100ms and serves the previous rows in between. Apply the same
   pattern to any future live-updating view.

4. **Never sleep or do slow work holding `game_state.mutex`** — the
   render thread, input handler, and bot worker all take it. The bot's
   human-turn wait gate unlocks before its 30ms nanosleep.

5. **Measure the specific frame, not the average.** The fps readout
   shows the WORST frame in the last second (`1e6 / g_max_frame_us`)
   precisely because lag is felt at the tail. A healthy typing frame is
   1-3ms at 2x; anything >50ms has a bug.

## Diagnostics

- **Status bar**: `· N ms lag` = keypress-to-pixels latency of the last
  input frame; the fps badge appears (red) only when off-target.
- **`MAGPIE_FPS_DEBUG=1 bin/magpie_tui`** writes one line per rendered
  frame to `/tmp/magpie_stderr.log`:

  ```
  [fps] full_us=… lock_us=… emit_us=… emit+=… elide+=… blits=… rack+=… rast+=… inv=… input_lag_us=…
  ```

  | field | meaning | a slow frame with this big means |
  |---|---|---|
  | `full_us` | whole render path (compose + blit + emit) | (the symptom) |
  | `lock_us` | wait acquiring `game_state.mutex` | contention with the bot worker |
  | `emit_us` | `notcurses_render` only | terminal emit / backpressure |
  | `blits` | board tiles re-blitted this frame | tile-cache misses (key flapping / invalidation) |
  | `rack+` | rack-panel tiles re-blitted | rack cache misses |
  | `rast+` | FreeType rasterizations | glyph-cache thrash (size ping-pong drops all slots) |
  | `inv` | cumulative full tile-plane invalidations | resize recovery / cdy-cdx drift firing |

  If a slow frame shows ~zero in ALL of these, the time is in
  un-instrumented CPU inside `tui_game_render` — go straight to the
  profiler.

- **Profiler**: `sample magpie_tui 20 -file /tmp/s.txt` while the lag
  is happening, then read the `com.apple.main-thread` tree. This is
  what actually identified incident #2 after the counters all came up
  empty; don't spend long theorizing when a 20-second sample names the
  function.

## Incident history

1. **4fps at 2x, idle or typing (June 2026).**
   `tui_sync_plane_to_terminal` was level-triggered: on terminals where
   the ioctl winsize persistently disagrees with notcurses' clamped
   plane size (Ghostty), it ran `notcurses_refresh` — a full-screen
   redraw re-emitting all ~225 sprixels (~250ms) — every frame.
   Fix: edge-trigger on the ioctl size actually changing.

2. **160-300ms input lag / 4-5fps during and after the computer's turn
   in play-vs-computer (June 2026).** `populate_frame_analysis_rows`
   ran every frame and rebuilt the sim leaderboard rows — for a panel
   that play-vs-computer HIDES. Each row's move/leave strings did
   per-letter `strdup`/`free`; with ~10 search threads hammering the
   allocator, ~500 render-thread allocations cost ~280ms/frame
   (rules 1, 2, 3). Diagnosed with `sample` after every counter in the
   perf trace came up clean. Fix: skip row building entirely during a
   live play-vs-computer game; throttle live-leaderboard rebuilds to
   ~10Hz everywhere else.

3. **~16ms latency floor on every keypress.** The input drain ran at
   the bottom of the main loop, after the pacing sleep — a keystroke
   always waited out the remainder of a frame interval before its
   render. Fix: collapse the pacing deadline to "now" when input
   dirties the frame. (Measured 10-23ms → <1ms at 1x.)
