#ifndef TUI_EXCHANGE_DIALOG_H
#define TUI_EXCHANGE_DIALOG_H

#include "game_state.h"
#include "tui_ui_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The user's Exchange and Pass actions against the computer, from the
// Rack panel's buttons and menu and the /exchange and /pass commands.

// Opens the exchange picker on the user's rack (TUI_MODAL_EXCHANGE), or
// posts a notice saying why the user can't exchange now. While it's open
// the turn shows the picked tiles as its tentative move ("-LLQ"), cleared
// if the picker is cancelled.
void tui_exchange_dialog_open(TuiGameState *state, TuiUiState *ui);

// Opens "Pass your turn?" (TUI_MODAL_PASS_CONFIRM), or posts a notice
// saying why the user can't pass now.
void tui_pass_confirm_open(TuiGameState *state, TuiUiState *ui);

// Keys and clicks for either dialog. A tile's letter (or ?) picks one more
// tile with that face, until all are picked, then none; Space or a click
// toggles the tile on its row. Enter (or a click on Exchange) plays the
// exchange; Esc cancels. Returns true when a dialog consumed the key.
bool tui_input_exchange_dialog(TuiGameState *state, TuiUiState *ui,
                               uint32_t key, ncinput input);

// Writes the picker's picked (`picked`) or kept tiles, in rack order, as
// move notation ("LLQ", "?IOS") into `out`.
void tui_exchange_dialog_tiles(const TuiUiState *ui, bool picked, char *out,
                               size_t out_size);

#endif
