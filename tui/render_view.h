#ifndef TUI_RENDER_VIEW_H
#define TUI_RENDER_VIEW_H

#include "../src/ent/board.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "config.h"
#include "game_state.h"
#include <stdbool.h>
#include <stdint.h>

void compute_rack_ghost_mask(const TuiGameState *state,
                             const MachineLetter *slot_letters, int slot_count,
                             bool *out_ghost);
int effective_analysis_cursor(const TuiGameState *state);
const Move *pick_analysis_preview_move(const TuiGameState *state,
                                       int *out_player_idx);
const TuiHistoryEntry *pick_history_view(const TuiGameState *state);
const Board *pick_render_board(const TuiGameState *state);
double pick_render_clock_seconds(const TuiGameState *state, int player_idx);
int pick_render_on_turn(const TuiGameState *state);
const Rack *pick_render_rack(const TuiGameState *state, int player_idx);
int pick_render_score(const TuiGameState *state, int player_idx);
double seconds_remaining(const TuiGameState *state, int player_idx);
int sort_rack_for_display(const Rack *rack, const LetterDistribution *ld,
                          TuiRackSort sort, MachineLetter *out_slots,
                          int max_slots);

#endif
