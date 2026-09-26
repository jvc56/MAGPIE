#ifndef TUI_PREVIEW_GAME_H
#define TUI_PREVIEW_GAME_H

#include <stdbool.h>

// A standalone "demo" game used by the theme picker preview: probes for
// any installed lexicon, runs an autoplay loop picking the highest-scoring
// move each turn until the game ends (or no legal move is found), and
// holds the final position for rendering. No threads, no mutex, no
// history — strictly read-only after creation.

struct Game;
struct LetterDistribution;
struct PlayersData;
struct BoardLayout;

typedef struct {
  struct Game *game;
  struct LetterDistribution *ld;
  struct PlayersData *players_data;
  struct BoardLayout *board_layout;
} TuiPreviewGame;

// Builds and plays out the preview game. Returns true on success and
// fills out_pg. Returns false if no lexicon could be loaded (e.g., the
// data/ tree was never downloaded), in which case out_pg is zeroed.
bool tui_preview_game_init(TuiPreviewGame *out_pg);

void tui_preview_game_destroy(TuiPreviewGame *pg);

#endif
