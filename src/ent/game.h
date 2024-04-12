#ifndef GAME_H
#define GAME_H

#include <stdbool.h>

#include "../def/game_defs.h"
#include "../def/players_data_defs.h"

#include "bag.h"
#include "board.h"
#include "config.h"
#include "letter_distribution.h"
#include "player.h"

typedef struct Game Game;

Game *game_create(const Config *config);
void game_destroy(Game *game);
void game_update(const Config *config, Game *game);
Game *game_duplicate(const Game *game);
void game_reset(Game *game);

game_variant_t game_get_variant(const Game *game);
Board *game_get_board(const Game *game);
Bag *game_get_bag(const Game *game);
const LetterDistribution *game_get_ld(const Game *game);
Player *game_get_player(const Game *game, int player_index);
int game_get_player_on_turn_index(const Game *game);
game_end_reason_t game_get_game_end_reason(const Game *game);
backup_mode_t game_get_backup_mode(const Game *game);
int game_get_consecutive_scoreless_turns(const Game *game);
int game_get_player_draw_index(const Game *game, int player_index);
int game_get_player_on_turn_draw_index(const Game *game);
bool game_get_data_is_shared(const Game *game,
                             players_data_t players_data_type);

void game_set_consecutive_scoreless_turns(Game *game, int value);
void game_increment_consecutive_scoreless_turns(Game *game);
void game_set_game_end_reason(Game *game, game_end_reason_t game_end_reason);
void game_start_next_player_turn(Game *game);

void game_set_backup_mode(Game *game, int backup_mode);
void game_backup(Game *game);
void game_unplay_last_move(Game *game);
void game_set_starting_player_index(Game *game, int starting_player_index);
void game_gen_all_cross_sets(Game *game);
void game_gen_cross_set(Game *game, int row, int col, int dir,
                        int cross_set_index);
cgp_parse_status_t game_load_cgp(Game *game, const char *cgp);

#endif