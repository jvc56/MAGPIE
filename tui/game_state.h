#ifndef TUI_GAME_STATE_H
#define TUI_GAME_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations keep this header light. Implementation pulls in
// the engine types directly.
struct Game;
struct LetterDistribution;
struct PlayersData;
struct BoardLayout;

typedef struct {
  struct Game *game;
  struct LetterDistribution *ld;
  struct PlayersData *players_data;
  struct BoardLayout *board_layout;
  const char *data_paths;  // non-owning; points at a static candidate path
  char lexicon[64];
} TuiGameState;

// Initializes a fresh game using `lexicon` (e.g., "CSW21"). Resolves the
// data root by probing for the lexicon's .kwg under "data/", "../data/",
// and "./data/". Loads the matching letter distribution, the default
// 15x15 board layout, and the lexicon's KWG. Calls draw_starting_racks.
//
// Returns true on success. On failure, fills `error_message` (caller
// buffer) with a user-visible diagnostic and leaves `out_state` zeroed.
bool tui_game_state_init(const char *lexicon, uint64_t seed,
                         TuiGameState *out_state, char *error_message,
                         size_t error_message_size);

void tui_game_state_destroy(TuiGameState *state);

#endif
