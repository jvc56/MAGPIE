#ifndef CGP_H
#define CGP_H

#include "../def/game_defs.h"
#include "../ent/game.h"
#include "../util/io_util.h"
#include <stddef.h>

void game_load_cgp(Game *game, const char *cgp, ErrorStack *error_stack);
char *game_get_cgp(const Game *game, bool write_player_on_turn_first);
// Writes the same representation as game_get_cgp into dest instead of
// malloc'ing a new string, truncating if it does not fit. dest_size must
// include room for the terminating null.
void game_get_cgp_string(const Game *game, bool write_player_on_turn_first,
                         char *dest, size_t dest_size);
char *game_get_cgp_with_options(const Game *game,
                                bool write_player_on_turn_first,
                                PlayersData *players_data, int bingo_bonus,
                                const char *board_layout_name,
                                const char *ld_name,
                                game_variant_t game_variant);

#endif
