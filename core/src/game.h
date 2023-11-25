#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#include "bag.h"
#include "config.h"
#include "movegen.h"
#include "player.h"
#include "rack.h"
#include "string_util.h"

#define MAX_SEARCH_DEPTH 25
#define MAX_SCORELESS_TURNS 6

typedef enum {
  CGP_PARSE_STATUS_SUCCESS,
  CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS,
  CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_ROWS,
  CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_COLUMNS,
  CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_RACKS,
  CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_SCORES,
  CGP_PARSE_STATUS_MALFORMED_BOARD_LETTERS,
  CGP_PARSE_STATUS_MALFORMED_SCORES,
  CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS,
  CGP_PARSE_STATUS_MALFORMED_CONSECUTIVE_ZEROS,
  CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BINGO_BONUS,
  CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BOARD_NAME,
  CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_GAME_VARIANT,
} cgp_parse_status_t;

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
  int starting_player_index;
  int consecutive_scoreless_turns;
  game_end_reason_t game_end_reason;
} MinimalGameBackup;

typedef struct Game {
  Generator *gen;
  Player *players[2];
  bool data_is_shared[NUMBER_OF_DATA];
  int player_on_turn_index;
  int starting_player_index;
  int consecutive_scoreless_turns;
  game_end_reason_t game_end_reason;
  MinimalGameBackup *game_backups[MAX_SEARCH_DEPTH];
  int backup_cursor;
  int backup_mode;
  bool backups_preallocated;
} Game;

void reset_game(Game *game);
void update_game(const Config *config, Game *game);
Game *create_game(const Config *config);
Game *copy_game(const Game *game, int move_list_capacity);
void destroy_game(Game *game);
cgp_parse_status_t load_cgp(Game *game, const char *cgp);
void draw_letter_to_rack(Bag *bag, Rack *rack, uint8_t letter,
                         int player_draw_index);
int get_player_draw_index(Game *game, int player_index);
int get_player_on_turn_draw_index(Game *game);
void set_backup_mode(Game *game, int backup_mode);
void backup_game(Game *game);
void unplay_last_move(Game *game);
int tiles_unseen(const Game *game);
game_variant_t get_game_variant_type_from_name(const char *variant_name);
void set_starting_player_index(Game *game, int starting_player_index);
void string_builder_add_game(const Game *game, StringBuilder *game_string);

#endif