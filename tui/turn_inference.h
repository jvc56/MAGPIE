#ifndef TUI_TURN_INFERENCE_H
#define TUI_TURN_INFERENCE_H

#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "game_state.h"
#include <stdbool.h>

// Whether History turn `turn_idx` can have an inference of the
// opponent's leave: the turn before it is the opponent's played move
// (not a pass, nor a play that was challenged off) with a saved
// position, and the bag still held tiles at this turn, so the
// opponent's rack wasn't known. Whether that move was a bingo or the
// opening play is only known once it is validated against its
// position; the inference runner checks that. Caller holds
// state->mutex.
bool tui_turn_inference_applies(const TuiGameState *state, int turn_idx);

// Fills `out` from a finished inference of the leave of player
// `target_player_idx` after `after_move`, against a random leave of the
// same size from the inference's unseen tiles, valued with `klv`.
void tui_inference_summary_fill(InferenceResults *results,
                                const LetterDistribution *ld, const KLV *klv,
                                int target_player_idx, const char *after_move,
                                TuiInferenceSummary *out);

#endif
