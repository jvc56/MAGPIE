#ifndef GAME_H
#define GAME_H

#include "../def/game_defs.h"

#include "config.h"

struct MinimalGameBackup;
typedef struct MinimalGameBackup MinimalGameBackup;

struct Game;
typedef struct Game Game;

Generator *game_get_gen(Game *game);
Player *game_get_player(Game *game, int player_index);
int game_get_player_on_turn_index(Game *game);
int game_get_game_end_reason(Game *game);

void reset_game(Game *game);
void update_game(const Config *config, Game *game);
Game *create_game(const Config *config);
Game *game_duplicate(const Game *game, int move_list_capacity);
void destroy_game(Game *game);
cgp_parse_status_t load_cgp(Game *game, const char *cgp);
int get_player_draw_index(Game *game, int player_index);
int get_player_on_turn_draw_index(Game *game);
void set_backup_mode(Game *game, int backup_mode);
void backup_game(Game *game);
void unplay_last_move(Game *game);
int tiles_unseen(const Game *game);
game_variant_t get_game_variant_type_from_name(const char *variant_name);
void set_starting_player_index(Game *game, int starting_player_index);

#endif