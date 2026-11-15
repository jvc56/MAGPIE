#ifndef TUI_GAME_RENDER_H
#define TUI_GAME_RENDER_H

#include "game_state.h"
#include "theme.h"
#include "tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

struct Game;
struct LetterDistribution;

// Draws the full game frame, with `modal` (if any) on top. `modal_help`,
// when non-NULL, describes the open dialog's focused row on the
// command-bar row. Also records
// this frame's analysis rows, scroll offset, and scrollbar geometry in
// `state` for the input handlers, so the caller must hold state->mutex.
void tui_game_render(struct ncplane *plane, const Theme *theme,
                     TuiGameState *state, int time_per_side_seconds,
                     TuiModalState modal, const char *modal_help);

// Destroy any cached pixel-grid child planes (board, rack, both pills,
// preview). Call after the theme picker exits so the preview's grid
// doesn't linger under the in-game UI, and on resize so a font-size
// change rebuilds them at the new cell-to-pixel ratio.
void tui_game_render_reset_grids(void);

#endif
