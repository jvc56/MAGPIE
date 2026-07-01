#ifndef TUI_GAME_STATE_H
#define TUI_GAME_STATE_H

#include "../src/def/peg_defs.h"
#include "config.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Forward declarations keep this header light. Implementation pulls in
// the engine types directly.
struct Game;
struct LetterDistribution;
struct PlayersData;
struct BoardLayout;
struct MoveList;
struct WinPct;
struct SimResults;
struct EndgameResults;
struct EndgameCtx;
struct PegPoll;
struct PegPollSnapshot;
struct Board;
struct Move;
struct Rack;
typedef struct TuiGlyphCache TuiGlyphCache;

// Saved copy of an endgame solve. Owns its own board + rack snapshot
// + Move array so the renderer can keep displaying the leaderboard
// after the bot has played and the live board has moved on. Move
// strings render against `board`; leaves render against `solve_rack`.
// Updated incrementally via the engine's per-ply callback during a
// solve, then finalized when endgame_solve returns.
typedef struct {
  struct Board *board;     // owned; board_duplicate at solve time
  struct Rack *solve_rack; // owned; rack_duplicate at solve time
  struct Move **moves;     // owned: each Move owned; array also owned
  int *values;             // PV value (solver-perspective spread delta)
  int num_entries;
  int initial_spread;
  int depth;
  int solving_player;
  bool valid;
  // True when the snapshot was produced after a fully-completed
  // search (status FINISHED, no time interrupt). The analysis title
  // shows "(exhaustive)" instead of "(N-ply negamax)" in that case.
  bool exhaustive;
} TuiEndgameSnapshot;

enum {
  // Row capacity of the TUI-side PEG leaderboard: the current stage's
  // candidates plus the previous stage's baseline ranking (each capped
  // at PEG_POLL_MAX_ENTRIES by the engine's poll).
  TUI_PEG_SNAPSHOT_CAP = 2 * PEG_POLL_MAX_ENTRIES,
};

// Live view of an in-progress (or just-finished) PEG (pre-endgame)
// solve, rebuilt from the engine's PegPoll at the analysis panel's
// ~10 Hz refresh cadence. Owns a contiguous Move array (allocated
// once at game-state init) so the board-preview path can hand out
// stable Move pointers between refreshes. The rows merge the current
// stage's completed candidates (deepest data first) with the previous
// stage's baseline ranking, so a fresh halving stage never blanks the
// leaderboard while its first candidate is still solving. Written by
// the analysis row builder under state->mutex.
typedef struct {
  struct Move *moves;                    // owned; capacity TUI_PEG_SNAPSHOT_CAP
  double win_pcts[TUI_PEG_SNAPSHOT_CAP]; // mover win fraction, 0..1
  double spreads[TUI_PEG_SNAPSHOT_CAP];  // signed mean spread (points)
  int num_entries;
  int stage;          // current stage (0 = greedy seed)
  int fidelity_plies; // current stage's endgame fidelity (0 = greedy)
  int cands_done;     // candidates finished in the current stage
  int field_size;     // candidates being evaluated in the current stage
  int solving_player; // player on turn in the analyzed position
  bool done;          // solve finished
  bool valid;
} TuiPegSnapshot;

// Which engine produced an analysis leaderboard — the saved-snapshot
// discriminator and the render dispatch key. SIM is the zero value so
// a zero-initialized snapshot defaults to the sim shape.
enum {
  TUI_ANALYSIS_MODE_SIM = 0,
  TUI_ANALYSIS_MODE_PEG = 1,
  TUI_ANALYSIS_MODE_ENDGAME = 2,
};

enum {
  TUI_HISTORY_MAX = 200,
  // Max ranked candidates shown in the Analysis panel. Same cap is
  // used for the saved per-turn snapshot so navigating history can
  // replay the full leaderboard.
  // Max candidate rows the panel can track per frame and per
  // saved snapshot. Matches the upper bound of the sim_candidates
  // setting (Watch setup row, range 2-1024) so users who crank
  // candidates high can still scroll through every one.
  ANALYSIS_ROW_CAP = 1024,
  // Max plies the per-ply average columns track. Sim engine runs
  // up to ~4 plies in practice; the cap is generous.
  MAX_ANALYSIS_PLIES = 8,
};

typedef enum {
  ANALYSIS_TINT_NONE = 0, // default dim_fg
  ANALYSIS_TINT_WIN,      // positive final spread — accent (green)
  ANALYSIS_TINT_LOSS,     // negative final spread — error (red)
  ANALYSIS_TINT_TIE,      // zero final spread — dim (grey)
} AnalysisTint;

// One ranked candidate row in the Analysis panel. Same shape is
// used both for the live render and for the saved-per-turn
// snapshot stored on each history entry, so the renderer can
// route through identical layout code regardless of which source
// is supplying the data.
typedef struct {
  char move[80];
  char leave[16];
  char score[8];     // "30" / "120"; empty if not applicable
  char primary[8];   // "67.3%" in sim; empty in endgame
  char secondary[8]; // "+32.5" or "+27"
  AnalysisTint secondary_tint;
  // Raw values, kept alongside the display strings so the renderer
  // can bold the highest-scoring row(s) using full precision
  // instead of the rounded display values.
  int score_value;
  double primary_value;
  double secondary_value;
  // Per-ply mean move-score (only populated for sim mode).
  double ply_avg[MAX_ANALYSIS_PLIES];
  int ply_count;
  // Player index whose turn it is for ply 0 (the candidate's
  // player). Used by the renderer to color the per-ply columns.
  int candidate_player_idx;
  bool valid;
} AnalysisRow;

// Saved-per-turn snapshot of the Analysis panel contents. Captured
// at bot-worker finalize time (move chosen, before play_move) so
// the user can navigate the history cursor back to any committed
// turn and re-display the analysis that was visible at the moment
// the bot decided. Sim, PEG, and endgame share this shape; `mode`
// selects which set of meta fields the title rendering reads.
typedef struct {
  AnalysisRow rows[ANALYSIS_ROW_CAP];
  int num_rows;
  // Sim-mode title meta. Reproduces what render_analysis_panel
  // would compute from a live SimResults.
  int sim_plies;
  uint64_t sim_iterations;
  uint64_t sim_nodes;
  // Endgame-mode title meta.
  int endgame_depth;
  uint64_t endgame_nodes;
  bool endgame_exhaustive;
  // PEG-mode title meta.
  int peg_fidelity;   // deepest fidelity (plies) behind the rows
  int peg_field_size; // candidates evaluated at capture time
  // Which engine produced the rows (TUI_ANALYSIS_MODE_*).
  int mode;
  bool valid;
} TuiAnalysisSnapshot;

// What a history entry represents. MOVE is a normal played turn. The
// clock-event kinds mirror the engine's GAME_EVENT_TIME_PENALTY
// game-history representation: for TIME_PENALTY, `score` holds the
// (negative) score adjustment and `total_after` the player's
// cumulative score after the adjustment. TIME_FORFEIT records a loss
// on time (flagging) — no score adjustment, text-only. (Challenged-
// off phonies are NOT a separate entry kind: they render as extra
// rows of the play's own MOVE entry via `challenged_off`, keeping the
// two-column history's index-parity column assignment intact.)
enum {
  TUI_HISTORY_ENTRY_MOVE = 0,
  TUI_HISTORY_ENTRY_TIME_PENALTY = 1,
  TUI_HISTORY_ENTRY_TIME_FORFEIT = 2,
};

typedef struct {
  int player_idx;
  // Entry kind (TUI_HISTORY_ENTRY_*). Zero-init = MOVE.
  int kind;
  int score;              // points earned on this play
  int total_after;        // running total after this play (excluding bonus)
  int clock_at_start;     // seconds remaining when this player's turn began
  int opp_clock_at_start; // opponent's seconds remaining at the same moment
  // Seconds remaining when this player's move ended. Set by the bot
  // worker right after seconds_used is updated for the turn. For
  // loaded GCGs we don't have per-move clock data, so this stays at
  // clock_at_start (effectively unused outside the live-watch flow).
  int clock_at_end;
  char move_str[48]; // "8H POND" or "exch DEFG" or "pass" (no score)
  char rack_str[16]; // full rack the player had at the start of the turn
  char
      leave_str[16]; // tiles kept after the play (empty = outplay/exchange-all)
  // Going-out bonus, attached to the going-out player's last move so it
  // renders as a third line of that entry instead of its own row. Zero
  // when this entry has no end-of-game adjustment.
  int end_bonus;
  char end_rack_str[16]; // opponent's leftover tiles (e.g. "EE")
  // True when this play was challenged off (a phony under the
  // SINGLE / DOUBLE / PENALTY challenge rules): the tiles returned
  // and the turn was lost. `score` / `total_after` keep the play's
  // as-if values; the renderer appends a "challenged off −N" row
  // pair that cancels them (mirroring the engine's
  // GAME_EVENT_PHONY_TILES_RETURNED adjustment), and running-total
  // aggregation treats the entry as zero points.
  bool challenged_off;
  // True while the bot is still computing this turn's move. The
  // renderer shows a braille spinner in place of move_str / +score
  // and the bot worker flips it back to false once the move is
  // finalized.
  bool pending;

  // Board snapshot captured BEFORE this turn was played. Owned by
  // this entry; board_duplicate'd in append_pending_history and
  // destroyed in pop_history / state destroy / game reset. Used
  // by the History-panel cursor preview: navigating to entry idx
  // displays this board so the user can replay the position the
  // player faced at the moment of decision.
  struct Board *board_before;

  // Sim / endgame snapshot captured at finalize time (just after
  // the bot picks the move, before play_move). Lets the History
  // cursor replay the Analysis-panel contents that were visible
  // when this turn was decided. Cleared when the entry is dropped
  // or the game is reset.
  TuiAnalysisSnapshot analysis_snapshot;

  // Full deep-copy of the SimResults that drove this turn's sim
  // decision, captured at the same finalize-time moment as
  // analysis_snapshot. Owned by this entry. NULL when no sim ran
  // for this turn (e.g., endgame turns). Preserves every Stat /
  // PRNG / BAIResult / SimmedPlay so a follow-up "/resume" can
  // continue iterating onto the saved samples instead of
  // restarting from zero.
  struct SimResults *sim_results_saved;

  // Deep-copy of the endgame leaderboard moves at finalize time,
  // captured for turns the bot decided via endgame solve (rather
  // than sim). Lets the History cursor preview each endgame
  // candidate on the board the same way it does for sim turns.
  // Owned by this entry; contiguous Move array, length stored in
  // endgame_moves_saved_count. NULL when not an endgame turn.
  struct Move *endgame_moves_saved;
  int endgame_moves_saved_count;

  // Deep-copy of the PEG leaderboard moves at finalize time, captured
  // for turns the bot decided via the pre-endgame solver. Same role
  // as endgame_moves_saved: lets the History cursor preview each PEG
  // candidate on the board after the live peg_snapshot has moved on.
  // Owned by this entry; contiguous Move array. NULL when not a PEG
  // turn.
  struct Move *peg_moves_saved;
  int peg_moves_saved_count;

  // Pre-move rack snapshot, owned. Pairs with board_before for
  // resuming analysis on this turn's position. The text rack_str
  // above is for display; this is the engine-friendly object the
  // simmer / endgame would consume.
  struct Rack *rack_before;
  // CGP of the position this turn was decided from (on-turn player's
  // rack first). Captured when the pending entry is created; the
  // "/resume" analysis worker reloads it into a scratch Game so a
  // stopped sim / endgame solve can continue on the exact position —
  // board and racks alone wouldn't pin down the bag or the scores.
  char cgp_before[512];
  // Owned, deep-copied Move that this turn played, for loaded
  // GCG entries that don't carry a sim/endgame leaderboard. Lets
  // the board renderer ghost the played tiles when the user
  // cursors onto the "Plays" row. NULL for live-game entries
  // (those preview from sim_results_saved / endgame_moves_saved).
  struct Move *loaded_move;
  // Opponent's rack at the moment this turn began. Snapshotted so
  // that the History cursor can rewind the off-turn player's pill
  // alongside the on-turn rack panel and board. NULL until
  // populated by append_pending_history.
  struct Rack *opp_rack_before;

  // Annotation revalidation error message — populated by
  // tui_game_state_revalidate_history when the engine refuses to
  // apply this turn's move against the (so-far-correct) running
  // board. Examples: "tile collision", "disconnected play",
  // "rack missing T". Rendered as an extra row below the entry
  // in the History panel. Empty string ('\0') = no error.
  // Caller-managed; cleared on every revalidation pass and
  // re-populated for whatever turn (if any) trips validation
  // first. Sized to hold several accumulated problems for one turn
  // (e.g. "not enough P's …; Q's …") since a single play can
  // overrun more than one letter at once.
  char error_str[192];
} TuiHistoryEntry;

typedef struct {
  struct Game *game;
  struct LetterDistribution *ld;
  struct PlayersData *players_data;
  struct BoardLayout *board_layout;
  const char *data_paths; // non-owning; points at a static candidate path
  char lexicon[64];

  // Mutex protects every read of *game, racks, scores, bag, and the
  // history array. Both the render path and the bot worker take it.
  pthread_mutex_t mutex;

  TuiHistoryEntry history[TUI_HISTORY_MAX];
  int history_count;

  // Per-player display name. Populated when a GCG is loaded (from
  // the #player1/#player2 directives) so the pill headers can
  // surface real names instead of the generic "P1"/"P2". Empty
  // string = no name set; renderer falls back to "P1"/"P2".
  char player_names[2][32];

  // Visual settings.
  int border_thickness; // pixel-grid line thickness; 0 = off
  bool blank_uppercase; // played blanks render uppercase + blank_tile_fg
  TuiPremiumLabels premium_labels; // TW/tw/none label style
  int board_scale; // 1 = classic cell tiles, 2 = 4×2 pixel tiles
  bool antialias;  // FT_RENDER_MODE_NORMAL vs MONO at 2x
  TuiScoreSubscripts score_subscripts; // off / nonzero / all (2x only)
  TuiRackSort rack_sort;               // display ordering of rack tiles
  // Glyph cache used by the 2x render path. NULL when the bundled font
  // could not be loaded or freetype init failed — that disables 2x
  // regardless of the user's saved board_scale. `glyph_cache_sub` is a
  // second instance of the same font sized for the score subscript so
  // we can hold both pixel sizes alive at once without thrashing
  // FT_Set_Pixel_Sizes per cell.
  TuiGlyphCache *glyph_cache;
  TuiGlyphCache *glyph_cache_sub;

  // Win-percentage table used by the simulator. Loaded once at init,
  // owned by the state, destroyed in tui_game_state_destroy. NULL when
  // the file couldn't be loaded — in that case the bot worker falls
  // back to plain equity-best moves.
  struct WinPct *win_pcts;

  // Sim results, allocated once and reused across turns. The bot
  // worker populates this on each sim turn and the analysis panel
  // reads it. `sim_results_active` tracks whether the SimResults
  // currently holds data — flipped to true when a sim starts and back
  // off when the turn is finalized. The `sim_results_turn_idx` value
  // is the history index this sim was computed for, so the renderer
  // can title the "Move N." block correctly even after play_move has
  // moved on.
  struct SimResults *sim_results;
  _Atomic bool sim_results_active;
  _Atomic int sim_results_turn_idx;

  // Endgame results, same idea as sim_results above — allocated once
  // at init, reused per turn. `endgame_initial_spread` captures the
  // solver's score margin (solver - opponent) at the moment the solve
  // begins so the renderer can show W/T/L vs the current score
  // without having to know which player was on turn at solve time.
  struct EndgameResults *endgame_results;
  // Reused across turns so the TT and small-move arena don't get
  // reallocated each solve — the old per-turn pattern churned ~1.6GB
  // of virtual memory and could SIGABRT under fragmentation. Owned
  // by the bot worker thread; destroyed in tui_game_state_destroy.
  struct EndgameCtx *endgame_ctx;
  _Atomic bool endgame_results_active;
  _Atomic int endgame_results_turn_idx;
  _Atomic int endgame_initial_spread;

  // Latest endgame solve snapshot, populated by the bot worker after
  // each successful solve (and while it still holds the pre-play
  // board). Renderer reads under state->mutex.
  TuiEndgameSnapshot endgame_snapshot;

  // PEG (pre-endgame) live analysis. peg_poll is the engine's
  // mutex-guarded live view — created once at init, reset before each
  // solve, safe to read from the render thread while peg_solve runs
  // on the bot worker. peg_poll_scratch is a reusable heap buffer for
  // peg_poll_read (PegPollSnapshot is tens of KB; don't stack one per
  // frame). peg_snapshot is the TUI-side merged leaderboard rebuilt
  // from the scratch snapshot under state->mutex. The active flag and
  // turn index mirror the sim/endgame pairs above; the reset paths
  // flip the turn index to -1 to mark the poll's contents stale.
  struct PegPoll *peg_poll;
  struct PegPollSnapshot *peg_poll_scratch;
  TuiPegSnapshot peg_snapshot;
  _Atomic bool peg_results_active;
  _Atomic int peg_results_turn_idx;
  // Monotonically bumped whenever something that affects pixel-plane
  // content changes (move played by the bot, theme switch, setting
  // toggle). The renderer caches its last successful blit signature
  // and short-circuits ncblit_rgba when this counter is unchanged —
  // that's what keeps 2x mode at 60fps instead of constantly
  // re-serializing 4.5MB of Kitty graphics per frame.
  _Atomic uint64_t render_version;

  // Clock state.
  int time_per_side_seconds;
  double seconds_used[2];       // accumulated time used by each side
  struct timespec turn_started; // CLOCK_MONOTONIC; when the on-turn
                                // player's current turn started ticking
  // Overtime rules (play-vs-computer only; see UiOvertimeRule).
  // The cap applies only under UI_OVERTIME_MAX.
  UiOvertimeRule overtime_rule;
  int overtime_cap_minutes;
  UiTimePenaltyRate time_penalty_rate;
  // Challenge rule (play-vs-computer only; see UiChallengeRule).
  // VOID rejects invalid plays at commit (do-over); the other rules
  // auto-challenge them off for loss of turn. The penalty variant
  // only matters once challenging the computer's plays exists.
  UiChallengeRule challenge_rule;
  UiChallengePenalty challenge_penalty;
  // Player who lost on time (-1 = none). Set by the bot worker when
  // the on-turn player's clock violates the overtime rule; the
  // commit / render paths treat a set value as game over.
  int time_forfeit_player_idx;
  // Guard so end-of-game overtime penalties are appended exactly once
  // (both the human-commit and bot-finalize paths can end the game).
  bool time_penalties_applied;

  // Bot worker.
  pthread_t bot_thread;
  bool bot_started;
  _Atomic bool bot_stop;

  // Post-game analysis-resume worker ("/resume"). Continues the
  // history-cursored turn's saved sim — accumulating onto the samples
  // gathered during the game — or re-solves its endgame with the
  // session's warm transposition table, streaming progress into the
  // live analysis panel. One resume at a time; "/stop" (or any reset
  // / new-game path) interrupts and joins it.
  pthread_t analysis_thread;
  bool analysis_started; // thread handle valid; join before reuse
  _Atomic bool analysis_stop;
  _Atomic bool analysis_running;
  int analysis_resume_turn_idx; // history idx being resumed; -1 = none
  // Scratch game positioned at the resumed turn (reloaded from the
  // entry's cgp_before). Owned by the worker; non-NULL only while a
  // resume is running. The analysis row builder and snapshot capture
  // format moves / read the bag against it instead of the live
  // (post-game) game.
  struct Game *analysis_game;

  // Pixel worker — rasterizes the 2x board RGBA composite off the
  // UI thread so cursor scrubbing through History doesn't stall
  // notcurses_render. The worker owns its own glyph caches
  // (pixel_glyph_cache / _sub) and communicates with the UI thread
  // via two slots guarded by pixel_mutex + pixel_cond:
  //   - pixel_request: UI snapshots the inputs (board copy, scale,
  //     antialias, etc.) and signals; worker picks it up.
  //   - pixel_result: worker publishes an RGBA buffer + the
  //     identity of the snapshot it was rendered from; UI ncblits
  //     it on the next frame whose params still match.
  pthread_t pixel_thread;
  bool pixel_started;
  _Atomic bool pixel_stop;
  pthread_mutex_t pixel_mutex;
  pthread_cond_t pixel_cond;
  TuiGlyphCache *pixel_glyph_cache;
  TuiGlyphCache *pixel_glyph_cache_sub;
  struct TuiPixelRequest {
    bool pending;
    struct Board *board; // owned: board_duplicate'd by UI
    const void *theme;   // borrowed pointer to a Theme (theme.h);
                         // declared as void* here to keep game_state.h free
                         // of the notcurses dependency theme.h drags in.
    int scale;
    int cell_w, cell_h;
    unsigned cdy, cdx;
    bool blank_uppercase;
    bool antialias;
    TuiPremiumLabels premium_labels;
    TuiScoreSubscripts score_subscripts;
    int border_thickness;
    uint64_t version;   // render_version at request time
    int history_cursor; // -1 = live, else committed entry idx
  } pixel_request;
  struct TuiPixelResult {
    bool ready;
    uint8_t *buf; // owned: worker malloc'd, UI free's after blit
    int buf_w;
    int buf_h;
    // Identity matches pixel_request shape so UI can compare and
    // discard the result if it's stale relative to current params.
    int scale;
    int cell_w, cell_h;
    unsigned cdy, cdx;
    bool blank_uppercase;
    bool antialias;
    TuiPremiumLabels premium_labels;
    TuiScoreSubscripts score_subscripts;
    int border_thickness;
    uint64_t version;
    int history_cursor;
  } pixel_result;

  // Active session snapshot of the settings that take effect at game-
  // state init time only (lexicon name, whether RIT was loaded). The
  // Settings UI writes to the config; comparing config-vs-active is
  // how the "(restart to apply)" pending-change line decides what to
  // surface. Replaced on each tui_game_state_init.
  char active_lexicon[32];
  bool active_load_rit;
  // Currently focused panel (0 = none, 1..5 = Board/Rack/Bag/History
  // /Analysis). Driven by 1..5 / 0 / Tab in main.c's input loop;
  // consumed by game_render.c to draw a focused-panel border style
  // (double-line glyphs on a slightly-lighter bg strip).
  int focused_panel;

  // History-panel cursor. -1 = cursor on the "[4>" label (default
  // upon focusing the panel); 0..history_count-1 = cursor on that
  // entry. Driven by Up/Down (or k/j) while the History panel is
  // focused. Resets to -1 each time focus enters History.
  int history_cursor;

  // Analysis-panel cursor. -1 = cursor on the "[5>" label; 0..N-1
  // = on the N-th visible candidate row. Same shape as
  // history_cursor — navigates with Up/Down/Left/Right (and
  // h/j/k/l) when the Analysis panel is focused, and click-to-
  // select via the same rect-map hit test history uses.
  int analysis_cursor;
  // Cursor column for the Analysis panel — RANK (cursor pinned to
  // a row index) vs MOVE (cursor pinned to a specific move so the
  // selection follows it as the sim reorders). Stored as an int so
  // game_state.h doesn't have to include game_render.h; values are
  // the TuiAnalysisColumn enum (0=RANK, 1=MOVE).
  int analysis_cursor_column;
  // When analysis_cursor_column == MOVE, holds the move-text the
  // cursor is anchored to (e.g. "5E IrISATED"). Each render in
  // MOVE mode scans the visible rows for a matching `move` field
  // to recover the cursor's current index. Empty string when not
  // anchored.
  char analysis_anchored_move[80];
  // Snapshot of the analysis rows actually rendered last frame.
  // Input handlers read this when they need to know the move text
  // at a given row index (e.g. when transitioning to MOVE column).
  // Written each frame under state->mutex by the render path.
  AnalysisRow last_rendered_analysis_rows[ANALYSIS_ROW_CAP];
  int last_rendered_analysis_row_count;
  // Scroll position for the Analysis panel — the rank index of the
  // first row visible in the scroll window. Auto-adjusted to keep
  // the cursor visible; can also be driven directly by the
  // scrollbar (click + drag) or scroll wheel. Reset to 0 whenever
  // history_cursor lands on a new turn, since the rank set
  // changes.
  int analysis_scroll_offset;
  // True while a scrollbar drag is in progress (button held down
  // after a click that started inside the thumb). Reset on
  // button-up or focus change. The render code uses this to keep
  // tracking even when the cursor leaves the thumb's screen rect
  // mid-drag.
  bool analysis_scrollbar_dragging;
  // Geometry the renderer publishes for the Analysis scrollbar so
  // main.c can hit-test mouse clicks and convert them to a
  // scroll_offset. The rectangle spans
  //   [scrollbar_top..scrollbar_bottom] × scrollbar_col
  // in plane coordinates. analysis_scrollbar_total is the total
  // rank count the bar is scrolling over (state at last render);
  // analysis_scrollbar_view is the visible window height. Zero
  // values mean the scrollbar is hidden (no scroll needed).
  _Atomic int analysis_scrollbar_top;
  _Atomic int analysis_scrollbar_bottom;
  _Atomic int analysis_scrollbar_col;
  _Atomic int analysis_scrollbar_total;
  _Atomic int analysis_scrollbar_view;
  // Sim configuration the bot worker passes through to simmer
  // every turn. Defaults seeded at game_state init; the Watch
  // setup modal mutates these before a new game starts so each
  // run can tune depth vs. breadth without rebuilding. Both
  // persist to tui.toml so subsequent launches reuse the last
  // values.
  int sim_plies;
  int sim_candidates;
  // Renderer publishes the count of visible analysis rows here so
  // main.c can bound cursor++ without re-computing the layout.
  _Atomic int analysis_visible_rows;

  // Command-bar slash input. When slash_active is true, the bar
  // renders a "/" prompt + slash_buf with the terminal cursor live
  // at slash_buf's end, and main.c's input loop captures letters /
  // Tab / Enter / Backspace into the buffer instead of the global
  // hotkey dispatch.
  bool slash_active;
  char slash_buf[64];
  int slash_len;
  // Insertion-point cursor into slash_buf, in [0, slash_len]. Driven
  // by arrow keys / Home / End. Typed characters insert at this
  // position, Backspace removes the char before it, Delete removes
  // the char at it.
  int slash_cursor;

  // Pending values written by the Settings UI when the user picks a
  // different lexicon or toggles RIT. The renderer compares
  // pending_* to active_* to decide whether to show the pending-
  // change banner above the status bar. Initialized to match the
  // active values at game-state init.
  char pending_lexicon[32];
  bool pending_load_rit;

  // Transient status-bar notice (e.g. "Copied CGP"). Rendered until
  // notice_expires_at (CLOCK_MONOTONIC); an all-zero timespec means
  // no notice. The once-a-second clock render tick repaints the bar
  // within a second of expiry, so no extra invalidation is needed.
  char notice_buf[64];
  struct timespec notice_expires_at;

  // Annotation edit state. edit_history_idx == -1 means we're
  // not editing anything; otherwise it's the index of the
  // pending entry being edited and edit_field selects which
  // sub-cell is taking keystrokes.
  //   TUI_EDIT_FIELD_MOVE — the row-1 text ("8H POND"). On
  //     commit, the coord + word land in entry->move_str. If
  //     the word has any uppercase letters (newly-placed
  //     tiles), those letters are copied into the rack too
  //     so a 7-tile bingo implicitly specifies the rack.
  //   TUI_EDIT_FIELD_RACK — the row-2 rack ("ABCDEFG"). On
  //     commit, lands in entry->rack_str.
  // Each field keeps its own buffer + cursor so the user can
  // hop between them without losing in-progress text.
  int edit_history_idx;
  int edit_field;
  char edit_move_buf[80];
  int edit_move_len;
  int edit_move_cursor;
  bool edit_move_valid;
  char edit_rack_buf[20];
  int edit_rack_len;
  int edit_rack_cursor;
  bool edit_rack_valid;
  // Editable leave buffer — only reachable via mouse click on the
  // leave column of a pending entry. Arrow-key nav skips it. When
  // it has focus, typing appends to this buffer the same way the
  // rack field does.
  char edit_leave_buf[20];
  int edit_leave_len;
  int edit_leave_cursor;
  // Carryover leave seeded onto a freshly-created turn (the same
  // player's previous-turn leave). While the rack stays un-edited
  // by the user, the effective rack auto-becomes carryover +
  // the move's played tiles — so typing "KAM" on a turn carrying
  // "ERST" forward yields the rack "AEKMRST" (no overlap = pure
  // union). Cleared the moment the user types into the RACK field
  // directly (edit_rack_user_modified), and reset on each new turn.
  char edit_rack_carryover[20];
  // True when the user has typed into the RACK field directly
  // (i.e., the rack buffer is "their" content, not an auto-seed
  // from the move's inferred tiles). While false, the rack panel
  // and row-2 cell track the move's inferred rack live as the
  // user edits the move. Cleared on edit-mode entry, on
  // auto-seed from inferred, and on annotation game reset.
  bool edit_rack_user_modified;
  // Result of parsing the move buffer. edit_move_kind classifies
  // what the buffer currently represents — drives both validity
  // coloring and the commit path:
  //   EMPTY        — nothing typed; row renders neutral.
  //   PARTIAL      — well-formed prefix (e.g. "8H", "ex "); not
  //                  yet a complete move. Renders as valid.
  //   PLACEMENT    — "<coord> <word>"; canonical lives in
  //                  edit_move_canonical. Score in edit_move_score.
  //   WORD_ONLY    — bare word, no coord. Triggers the placement-
  //                  enumeration flow on focus-away.
  //   EXCHANGE     — "-<tiles>" or any prefix of "exchange" +
  //                  tiles. Canonical is "ex <tiles>".
  //   PASS         — first token is "pass".
  //   INVALID      — anything else; renders red.
  int edit_move_kind;
  // Engine score (raw points) for a PLACEMENT/EXCHANGE that
  // validated cleanly. -1 if score is not applicable.
  int edit_move_score;
  // Normalized notation passed to the engine on commit /
  // validation. Empty when kind is not PLACEMENT/EXCHANGE/PASS.
  char edit_move_canonical[80];
  // Tiles inferred from the played-letters portion of the move
  // buffer (uppercase → letter, lowercase → '?', dots skipped).
  // The rack panel uses this to preview the player's rack
  // while the user is still typing the move.
  char edit_move_inferred_rack[16];
  // Leave preview: the rack buffer's tiles minus the move's
  // played tiles (multiset subtraction). Empty when one side
  // is missing or the subtraction can't be performed because
  // the rack doesn't contain all played letters.
  char edit_move_leave[16];
  // Live preview Move object — owned, allocated at game init.
  // Populated by tui_game_state_parse_edit_buf when the user's
  // typed move text validates cleanly against the live board.
  // `edit_preview_move_valid` flips true when the buffer's
  // contents are this Move; the board renderer uses this to
  // ghost the typed play onto the board as the user types.
  struct Move *edit_preview_move;
  bool edit_preview_move_valid;
  // Which turn index the engine board is currently positioned for
  // (pre-move) during annotation editing. The editor seeks the
  // engine to the edited turn's pre-move board so move validation
  // doesn't collide with that turn's own already-placed tiles.
  // -2 = stale / unknown (force a re-seek on the next parse).
  int engine_positioned_for_turn;

  // App mode — makes the "who controls each seat" distinction explicit
  // instead of inferring it from bot_started + whether racks were drawn.
  // The bot worker, commit paths, and rack concealment all branch on it.
  //   WATCH           — both seats are the bot (the original default).
  //   ANNOTATE        — human fills both racks by hand; no bot runs.
  //   PLAY_VS_COMPUTER— one human seat (human_player_idx), bot plays the
  //                     other. Board move-entry submits the human's turn;
  //                     the bot idles while the human is on turn.
  int app_mode; // TuiAppMode
  // Seat the human controls in PLAY_VS_COMPUTER (0 or 1). Unused in
  // WATCH / ANNOTATE.
  int human_player_idx;

  // Board move-builder. Active only while board_entry_active is true.
  // The play's letters/blanks live in edit_move_buf's word token (exactly
  // as typed annotation stores them); the builder only owns the anchor +
  // direction. The on-board cursor (next empty square) and the ghosted
  // tiles are DERIVED from edit_preview_move at render time, so no
  // separate placement list or cursor field is stored.
  //
  // origin = the empty cell the user clicked (where typing starts);
  // anchor = the true word start after walking back through any leading
  // played-through tiles in the current direction. The anchor is
  // DERIVED from origin + direction on every re-anchor; direction
  // toggles and arrow-key moves operate on the origin (the user's
  // cell), never the walked-back anchor — comparing clicks against the
  // anchor made the same-cell direction toggle unreachable whenever a
  // leading-playthrough walk-back had moved it.
  bool board_entry_active;
  int board_origin_row;
  int board_origin_col;
  int board_anchor_row;
  int board_anchor_col;
  int board_dir; // BOARD_HORIZONTAL_DIRECTION / BOARD_VERTICAL_DIRECTION
} TuiGameState;

typedef enum {
  TUI_APP_MODE_WATCH = 0,
  TUI_APP_MODE_ANNOTATE = 1,
  TUI_APP_MODE_PLAY_VS_COMPUTER = 2,
} TuiAppMode;

enum {
  TUI_EDIT_FIELD_MOVE = 0,
  TUI_EDIT_FIELD_RACK = 1,
  // Editable leave field — sits on the move row, right-aligned
  // next to the score. Currently focus-only (clickable, walks
  // through tab cycle); full edit support (typing, override of
  // auto-derived leave) follows once basic placement is in.
  TUI_EDIT_FIELD_LEAVE = 2,
};

typedef enum {
  TUI_EDIT_MOVE_KIND_EMPTY = 0,
  TUI_EDIT_MOVE_KIND_PARTIAL,
  TUI_EDIT_MOVE_KIND_PLACEMENT,
  TUI_EDIT_MOVE_KIND_WORD_ONLY,
  TUI_EDIT_MOVE_KIND_EXCHANGE,
  TUI_EDIT_MOVE_KIND_PASS,
  TUI_EDIT_MOVE_KIND_INVALID,
} TuiEditMoveKind;

// Initializes a fresh game using `lexicon` (e.g., "CSW21"). Resolves the
// data root by probing for the lexicon's .kwg under "data/", "../data/",
// and "./data/". Loads the matching letter distribution, KLV, board
// layout, and KWG. Calls draw_starting_racks. Initializes the mutex and
// history; does not start the bot worker.
//
// Returns true on success. On failure, fills `error_message` (caller
// buffer) with a user-visible diagnostic and leaves `out_state` zeroed.
bool tui_game_state_init(const char *lexicon, uint64_t seed, bool load_rit,
                         TuiGameState *out_state, char *error_message,
                         size_t error_message_size);

void tui_game_state_destroy(TuiGameState *state);

// Re-parse the annotation edit buffers (move + rack) and store
// the resulting validity flags on state. Called whenever a
// buffer changes (typing / backspace / etc.). Move parses as
// "<coord> <word>"; rack parses as 1-7 chars of A-Z/?.
void tui_game_state_parse_edit_buf(TuiGameState *state);

// Single source of truth for the editor's effective rack — the letters that
// should appear in BOTH the player pill (engine rack, set by
// sync_player_rack_to_editor) and the history cell's rack row (rendered in
// game_render.c). Centralizing the selection keeps those two paths from
// drifting. Pure read of editor state; does not mutate. Writes the rack
// (NUL-terminated, unsorted "source" order) to `out`. `out_from_buffer`, if
// non-NULL, is set true when the result came from the live edit_rack_buf
// (so a caller may preserve the user's typed order; every other source is
// alphagram-sorted for display). Returns the number of letters written.
size_t tui_game_state_effective_editor_rack(const TuiGameState *state,
                                            char *out, size_t out_size,
                                            bool *out_from_buffer);

// Annotation-mode reset: same per-entry cleanup and clock /
// cursor reset as tui_game_state_reset_game, but resets the
// game to an empty board with full bag and DOES NOT draw
// starting racks. The annotator fills the racks in by hand as
// the live game progresses.
void tui_game_state_reset_game_for_annotation(TuiGameState *state);

// Replays committed history entries from turn 1 forward on a
// fresh engine state, validating each turn's move against the
// running board + the entry's rack_str. The first turn whose
// move can't be applied — tile collision, disconnected play,
// rack missing letters, bag underflow, invalid notation —
// gets a human-readable explanation written to its error_str
// field; replay stops there and the engine state is left at
// the position before the failing turn. Turns past the
// failing one keep whatever error_str they had (cleared on
// the next successful re-validation pass). On full success
// the live game state matches the committed sequence and
// every entry's error_str is empty.
void tui_game_state_revalidate_history(TuiGameState *state);

// Position the engine board at the START of turn `idx` (replays
// committed turns [0, idx)). The annotation editor calls this so
// re-validating a committed turn's move sees the board the player
// actually faced, not the post-game board with that turn's tiles
// already placed.
void tui_game_state_seek_engine_to_turn(TuiGameState *state, int idx);

// Frees everything inside an endgame snapshot (board, rack, moves,
// values), zeros the fields, marks invalid. Safe on a zero-init or
// already-cleared snapshot.
void tui_endgame_snapshot_clear(TuiEndgameSnapshot *snap);

// Zeros a PEG snapshot's rows and meta, marks it invalid. Keeps the
// owned Move array allocated for reuse (it is sized once at init and
// freed in tui_game_state_destroy). Safe on a zero-init snapshot.
void tui_peg_snapshot_clear(TuiPegSnapshot *snap);

// True when `game` is a PEG-phase position: the effective bag size —
// the raw bag minus the opponent's unknown tiles, the same formula
// peg_solve validates against — is within PEG_MIN_BAG..PEG_MAX_BAG.
bool tui_game_state_is_peg_position(const struct Game *game);

// Set the per-side time budget. Call once after init, before the bot
// worker starts. Resets the on-turn player's turn_started to "now".
void tui_game_state_set_time_per_side(TuiGameState *state, int seconds);

// True when the play has ended — either the engine says the game is
// over or a player lost on time (time_forfeit_player_idx is set).
// Play-vs-computer interactivity / concealment paths use this instead
// of bare game_over so a time forfeit also ends the game.
bool tui_game_state_play_over(const TuiGameState *state);

// Resets the game to a fresh start: new seed, fresh racks, empty
// history, zeroed clocks, cleared sim/endgame state, cleared
// snapshot, and a cold endgame TT. Caller must stop the bot worker
// first (otherwise the bot's reads race against the reset).
void tui_game_state_reset_game(TuiGameState *state, uint64_t seed);

#endif
