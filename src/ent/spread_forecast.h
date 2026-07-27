#ifndef SPREAD_FORECAST_H
#define SPREAD_FORECAST_H

#include "../util/io_util.h"
#include "equity.h"
#include "game.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct SpreadForecast SpreadForecast;

// Loads a compact game-state forecast from data/strategy/<name>.sfc.
SpreadForecast *spread_forecast_create(const char *data_paths, const char *name,
                                       ErrorStack *error_stack);
void spread_forecast_destroy(SpreadForecast *forecast);

const char *spread_forecast_get_name(const SpreadForecast *forecast);

// Count-level form used by evaluators that already have a canonical
// player-on-turn state (and by tests). The game wrapper below handles POV.
Equity spread_forecast_get_on_turn(const SpreadForecast *forecast,
                                   Equity current_on_turn_spread,
                                   unsigned int bag_tiles,
                                   unsigned int on_turn_rack_tiles,
                                   unsigned int off_turn_rack_tiles);
double spread_forecast_get_expected_turns_for_counts(
    const SpreadForecast *forecast, unsigned int bag_tiles,
    unsigned int on_turn_rack_tiles, unsigned int off_turn_rack_tiles,
    bool for_player_on_turn);

// Returns the expected final spread from pov_player_index's perspective.
// The model is canonicalized around the player on turn and uses the exact bag
// count plus both rack sizes, so it preserves end-of-bag timing information
// that an aggregate "tiles unseen" key loses.
Equity spread_forecast_get(const SpreadForecast *forecast, const Game *game,
                           int pov_player_index);

// Diagnostic outputs learned alongside spread. They are not used by the
// simmer, but make the timing mechanism directly testable.
double spread_forecast_get_expected_turns(const SpreadForecast *forecast,
                                          const Game *game, int player_index);
uint32_t spread_forecast_get_sample_count(const SpreadForecast *forecast,
                                          const Game *game);

#endif
