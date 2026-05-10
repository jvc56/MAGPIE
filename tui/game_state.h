#ifndef TUI_GAME_STATE_H
#define TUI_GAME_STATE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations keep this header light. Implementation pulls in
// the engine types directly.
struct Game;
struct LetterDistribution;
struct PlayersData;
struct BoardLayout;
struct MoveList;

enum {
  TUI_HISTORY_MAX = 200,
};

typedef struct {
  int player_idx;
  int score;        // points earned on this play
  int total_after;  // running total after this play
  char move_str[48];   // "8H POND" or "exch DEFG" or "pass"
  char leave_str[16];  // tiles left in rack after the play
} TuiHistoryEntry;

typedef struct {
  struct Game *game;
  struct LetterDistribution *ld;
  struct PlayersData *players_data;
  struct BoardLayout *board_layout;
  const char *data_paths;  // non-owning; points at a static candidate path
  char lexicon[64];

  // Mutex protects every read of *game, racks, scores, bag, and the
  // history array. Both the render path and the bot worker take it.
  pthread_mutex_t mutex;

  TuiHistoryEntry history[TUI_HISTORY_MAX];
  int history_count;

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

#endif
