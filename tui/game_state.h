#ifndef TUI_GAME_STATE_H
#define TUI_GAME_STATE_H

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
  TUI_HISTORY_MAX = 200,
};

typedef struct {
  int player_idx;
  int score;          // points earned on this play
  int total_after;    // running total after this play (excluding bonus)
  int clock_at_start; // seconds remaining when this player's turn began
  char move_str[48];  // "8H POND" or "exch DEFG" or "pass" (no score)
  char rack_str[16];  // full rack the player had at the start of the turn
  char leave_str[16]; // tiles kept after the play (empty = outplay/exchange-all)
  // Going-out bonus, attached to the going-out player's last move so it
  // renders as a third line of that entry instead of its own row. Zero
  // when this entry has no end-of-game adjustment.
  int end_bonus;
  char end_rack_str[16]; // opponent's leftover tiles (e.g. "EE")
  // True while the bot is still computing this turn's move. The
  // renderer shows a braille spinner in place of move_str / +score
  // and the bot worker flips it back to false once the move is
  // finalized.
  bool pending;
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

  // Visual settings.
  int border_thickness; // pixel-grid line thickness; 0 = off
  bool blank_uppercase; // played blanks render uppercase + blank_tile_fg
  TuiPremiumLabels premium_labels; // TW/tw/none label style
  int board_scale; // 1 = classic cell tiles, 2 = 4×2 pixel tiles
  bool antialias;  // FT_RENDER_MODE_NORMAL vs MONO at 2x
  TuiScoreSubscripts score_subscripts; // off / nonzero / all (2x only)
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

  // Bot worker.
  pthread_t bot_thread;
  bool bot_started;
  _Atomic bool bot_stop;

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
} TuiGameState;

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

// Frees everything inside an endgame snapshot (board, rack, moves,
// values), zeros the fields, marks invalid. Safe on a zero-init or
// already-cleared snapshot.
void tui_endgame_snapshot_clear(TuiEndgameSnapshot *snap);

// Set the per-side time budget. Call once after init, before the bot
// worker starts. Resets the on-turn player's turn_started to "now".
void tui_game_state_set_time_per_side(TuiGameState *state, int seconds);

// Resets the game to a fresh start: new seed, fresh racks, empty
// history, zeroed clocks, cleared sim/endgame state, cleared
// snapshot, and a cold endgame TT. Caller must stop the bot worker
// first (otherwise the bot's reads race against the reset).
void tui_game_state_reset_game(TuiGameState *state, uint64_t seed);

#endif
