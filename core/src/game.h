#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#include "bag.h"
#include "config.h"
#include "movegen.h"
#include "player.h"
#include "rack.h"

#define MAX_SEARCH_DEPTH 25
#define MAX_SCORELESS_TURNS 6

typedef enum {
  GAME_VARIANT_UNKNOWN,
  GAME_VARIANT_CLASSIC,
  GAME_VARIANT_WORDSMOG,
} game_variant_t;

typedef enum {
  GAME_END_REASON_NONE,
  GAME_END_REASON_STANDARD,
  GAME_END_REASON_CONSECUTIVE_ZEROS,
} game_end_reason_t;

typedef struct MinimalGameBackup {
  Board *board;
  Bag *bag;
  Rack *p0rack;
  Rack *p1rack;
  int p0score;
  int p1score;
  int player_on_turn_index;
  int consecutive_scoreless_turns;
  int game_end_reason;
} MinimalGameBackup;

typedef struct Game {
  Generator *gen;
  Player *players[2];
  int player_on_turn_index;
  int consecutive_scoreless_turns;
  int game_end_reason;
  MinimalGameBackup *game_backups[MAX_SEARCH_DEPTH];
  int backup_cursor;
  int backup_mode;
  int backups_preallocated;
} Game;

void reset_game(Game *game);
Game *create_game(Config *config);
Game *copy_game(Game *game, int move_list_size);
void destroy_game(Game *game);
void load_cgp(Game *game, const char *cgp);
void draw_letter_to_rack(Bag *bag, Rack *rack, uint8_t letter);
void set_backup_mode(Game *game, int backup_mode);
void backup_game(Game *game);
void unplay_last_move(Game *game);
void lexicon_ld_from_cgp(char *cgp, char *lexicon, char *ldname);
int tiles_unseen(Game *game);
game_variant_t get_game_variant_type_from_name(const char *variant_name);
void set_player_on_turn(Game *game, int player_on_turn_index);

#endif