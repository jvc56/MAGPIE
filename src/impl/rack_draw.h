#ifndef RACK_DRAW_H
#define RACK_DRAW_H

#include "../ent/game.h"
#include "../ent/rack.h"
#include <stdbool.h>

// Returns the number of letters drawn, or a DRAW_RACK_STRING_* code from
// def/gameplay_defs.h.
int draw_rack_string_from_bag(const Game *game, int player_index,
                              const char *rack_string);
bool draw_rack_from_bag(const Game *game, int player_index,
                        const Rack *rack_to_draw);
bool rack_is_drawable(const Game *game, int player_index,
                      const Rack *rack_to_draw);

#endif
