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
typedef struct TuiGlyphCache TuiGlyphCache;

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
} TuiGameState;

// Initializes a fresh game using `lexicon` (e.g., "CSW21"). Resolves the
// data root by probing for the lexicon's .kwg under "data/", "../data/",
// and "./data/". Loads the matching letter distribution, KLV, board
// layout, and KWG. Calls draw_starting_racks. Initializes the mutex and
// history; does not start the bot worker.
//
// Returns true on success. On failure, fills `error_message` (caller
// buffer) with a user-visible diagnostic and leaves `out_state` zeroed.
bool tui_game_state_init(const char *lexicon, uint64_t seed,
                         TuiGameState *out_state, char *error_message,
                         size_t error_message_size);

void tui_game_state_destroy(TuiGameState *state);

// Set the per-side time budget. Call once after init, before the bot
// worker starts. Resets the on-turn player's turn_started to "now".
void tui_game_state_set_time_per_side(TuiGameState *state, int seconds);

#endif
