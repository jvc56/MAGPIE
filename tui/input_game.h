#ifndef TUI_INPUT_GAME_H
#define TUI_INPUT_GAME_H

#include "game_state.h"
#include "tui_ui_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Game-screen keys when no modal or cell editor is open: panel focus
// (0-5, Tab), Esc menu, CGP copy, board / Analysis / History navigation,
// and the command bar with its slash commands. Returns true when consumed.
bool tui_input_game(TuiGameState *state, TuiUiState *ui, TuiSession *session,
                    uint32_t key, ncinput input);

#endif
