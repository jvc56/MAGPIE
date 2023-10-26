#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "cross_set.h"
#include "game.h"
#include "log.h"
#include "movegen.h"
#include "player.h"
#include "string_util.h"
#include "util.h"

#define GAME_VARIANT_CLASSIC_NAME "classic"
#define GAME_VARIANT_WORDSMOG_NAME "wordsmog"

void draw_letter_to_rack(Bag *bag, Rack *rack, uint8_t letter) {
  draw_letter(bag, letter);
  add_letter_to_rack(rack, letter);
}

game_variant_t get_game_variant_type_from_name(const char *variant_name) {
  game_variant_t game_variant = GAME_VARIANT_UNKNOWN;
  if (strings_equal(variant_name, GAME_VARIANT_CLASSIC_NAME)) {
    game_variant = GAME_VARIANT_CLASSIC;
  } else if (strings_equal(variant_name, GAME_VARIANT_WORDSMOG_NAME)) {
    game_variant = GAME_VARIANT_WORDSMOG;
  }
  return game_variant;
}

cgp_parse_status_t place_letters_on_board(Game *game, const char *letters,
                                          int row_start,
                                          int *current_column_index) {
  uint8_t *machine_letters =
      malloc_or_die(sizeof(uint8_t) * string_length(letters));
  int number_of_machine_letters = str_to_machine_letters(
      game->gen->letter_distribution, letters, false, machine_letters);
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  int col_start = *current_column_index;

  if (number_of_machine_letters < 0) {
    cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_BOARD_LETTERS;
  } else {
    for (int i = 0; i < number_of_machine_letters; i++) {
      set_letter(game->gen->board, row_start, col_start + i,
                 machine_letters[i]);
      draw_letter(game->gen->bag, machine_letters[i]);
      game->gen->board->tiles_played++;
    }
    *current_column_index = *current_column_index + number_of_machine_letters;
  }
  free(machine_letters);
  return cgp_parse_status;
}

cgp_parse_status_t parse_cgp_board_row(Game *game, const char *cgp_board_row,
                                       int row_index) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringBuilder *tile_string_builder = create_string_builder();
  int row_length = string_length(cgp_board_row);

  int current_row_number_of_spaces = 0;
  int current_column_index = 0;
  for (int i = 0; i < row_length; i++) {
    char current_char = cgp_board_row[i];
    if (isdigit(current_char)) {
      current_row_number_of_spaces =
          (current_row_number_of_spaces * 10) + char_to_int(current_char);
      if (string_builder_length(tile_string_builder) > 0) {
        cgp_parse_status = place_letters_on_board(
            game, string_builder_peek(tile_string_builder), row_index,
            &current_column_index);
        string_builder_clear(tile_string_builder);
        if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
          break;
        }
      }
    } else {
      if (i == 0 || current_row_number_of_spaces > 0) {
        current_column_index += current_row_number_of_spaces;
        current_row_number_of_spaces = 0;
      }
      string_builder_add_char(tile_string_builder, current_char);
    }
  }

  if (string_builder_length(tile_string_builder) > 0) {
    cgp_parse_status =
        place_letters_on_board(game, string_builder_peek(tile_string_builder),
                               row_index, &current_column_index);
  } else {
    current_column_index += current_row_number_of_spaces;
  }
  destroy_string_builder(tile_string_builder);

  if (current_column_index != BOARD_DIM &&
      cgp_parse_status == CGP_PARSE_STATUS_SUCCESS) {
    cgp_parse_status = CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_COLUMNS;
  }

  return cgp_parse_status;
}

cgp_parse_status_t parse_cgp_board(Game *game, const char *cgp_board) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *board_rows = split_string(cgp_board, '/', true);

  if (string_splitter_get_number_of_items(board_rows) != BOARD_DIM) {
    cgp_parse_status = CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_ROWS;
  } else {
    for (int i = 0; i < BOARD_DIM; i++) {
      cgp_parse_status =
          parse_cgp_board_row(game, string_splitter_get_item(board_rows, i), i);
      if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
        break;
      }
    }
  }
  destroy_string_splitter(board_rows);
  return cgp_parse_status;
}

int draw_rack_from_bag(Bag *bag, Rack *rack, const char *rack_string,
                       LetterDistribution *letter_distribution) {
  int number_of_letters_set =
      set_rack_to_string(rack, rack_string, letter_distribution);
  for (int i = 0; i < rack->array_size; i++) {
    for (int j = 0; j < rack->array[i]; j++) {
      draw_letter(bag, i);
    }
  }
  return number_of_letters_set;
}

cgp_parse_status_t
parse_cgp_racks_with_string_splitter(Game *game, StringSplitter *player_racks) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  int number_of_letters_added =
      draw_rack_from_bag(game->gen->bag, game->players[0]->rack,
                         string_splitter_get_item(player_racks, 0),
                         game->gen->letter_distribution);
  if (number_of_letters_added < 0) {
    return CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS;
  }
  number_of_letters_added =
      draw_rack_from_bag(game->gen->bag, game->players[1]->rack,
                         string_splitter_get_item(player_racks, 1),
                         game->gen->letter_distribution);
  if (number_of_letters_added < 0) {
    cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS;
  }
  return cgp_parse_status;
}

cgp_parse_status_t parse_cgp_racks(Game *game, const char *cgp_racks) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *player_racks = split_string(cgp_racks, '/', false);

  if (string_splitter_get_number_of_items(player_racks) != 2) {
    cgp_parse_status = CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_RACKS;
  } else {
    cgp_parse_status = parse_cgp_racks_with_string_splitter(game, player_racks);
  }
  destroy_string_splitter(player_racks);
  return cgp_parse_status;
}

cgp_parse_status_t parse_cgp_scores(Game *game, const char *cgp_scores) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *player_scores = split_string(cgp_scores, '/', false);
  if (string_splitter_get_number_of_items(player_scores) != 2) {
    cgp_parse_status = CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_SCORES;
  } else if (!is_all_digits_or_empty(
                 string_splitter_get_item(player_scores, 0)) ||
             !is_all_digits_or_empty(
                 string_splitter_get_item(player_scores, 1))) {
    cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_SCORES;
  } else {
    game->players[0]->score =
        string_to_int(string_splitter_get_item(player_scores, 0));
    game->players[1]->score =
        string_to_int(string_splitter_get_item(player_scores, 1));
  }
  destroy_string_splitter(player_scores);
  return cgp_parse_status;
}

cgp_parse_status_t
parse_cgp_consecutive_zeros(Game *game, const char *cgp_consecutive_zeros) {

  if (!is_all_digits_or_empty(cgp_consecutive_zeros)) {
    return CGP_PARSE_STATUS_MALFORMED_CONSECUTIVE_ZEROS;
  }
  game->consecutive_scoreless_turns = string_to_int(cgp_consecutive_zeros);
  return CGP_PARSE_STATUS_SUCCESS;
}

cgp_parse_status_t parse_cgp_with_cgp_fields(Game *game,
                                             StringSplitter *cgp_fields) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;

  cgp_parse_status =
      parse_cgp_board(game, string_splitter_get_item(cgp_fields, 0));
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  cgp_parse_status =
      parse_cgp_racks(game, string_splitter_get_item(cgp_fields, 1));
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  cgp_parse_status =
      parse_cgp_scores(game, string_splitter_get_item(cgp_fields, 2));
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  return parse_cgp_consecutive_zeros(game,
                                     string_splitter_get_item(cgp_fields, 3));
}

cgp_parse_status_t parse_cgp(Game *game, const char *cgp) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *cgp_fields = split_string_by_whitespace(cgp, true);

  if (string_splitter_get_number_of_items(cgp_fields) < 4) {
    cgp_parse_status = CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS;
  } else {
    cgp_parse_status = parse_cgp_with_cgp_fields(game, cgp_fields);
  }
  destroy_string_splitter(cgp_fields);
  return cgp_parse_status;
}

cgp_parse_status_t load_cgp(Game *game, const char *cgp) {
  cgp_parse_status_t cgp_parse_status = parse_cgp(game, cgp);
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  game->player_on_turn_index = 0;

  generate_all_cross_sets(game->gen->board, game->players[0]->kwg,
                          game->players[1]->kwg, game->gen->letter_distribution,
                          game->data_is_shared[PLAYERS_DATA_TYPE_KWG]);
  update_all_anchors(game->gen->board);

  if (game->consecutive_scoreless_turns >= MAX_SCORELESS_TURNS) {
    game->game_end_reason = GAME_END_REASON_CONSECUTIVE_ZEROS;
  } else if (game->gen->bag->last_tile_index == -1 &&
             (game->players[0]->rack->empty || game->players[1]->rack->empty)) {
    game->game_end_reason = GAME_END_REASON_STANDARD;
  } else {
    game->game_end_reason = GAME_END_REASON_NONE;
  }
  return cgp_parse_status;
}

int tiles_unseen(Game *game) {
  int bag_idx = game->gen->bag->last_tile_index;
  int their_rack_tiles =
      game->players[1 - game->player_on_turn_index]->rack->number_of_letters;

  return (their_rack_tiles + bag_idx + 1);
}

void reset_game(Game *game) {
  reset_generator(game->gen);
  reset_player(game->players[0]);
  reset_player(game->players[1]);
  game->player_on_turn_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->backup_cursor = 0;
}

void set_player_on_turn(Game *game, int player_on_turn_index) {
  game->player_on_turn_index = player_on_turn_index;
}

void pre_allocate_backups(Game *game) {
  // pre-allocate heap backup structures to make backups as fast as possible.
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    game->game_backups[i] = malloc_or_die(sizeof(MinimalGameBackup));
    game->game_backups[i]->bag = create_bag(game->gen->letter_distribution);
    game->game_backups[i]->board = create_board();
    game->game_backups[i]->p0rack =
        create_rack(game->gen->letter_distribution->size);
    game->game_backups[i]->p1rack =
        create_rack(game->gen->letter_distribution->size);
  }
}

void set_backup_mode(Game *game, int backup_mode) {
  game->backup_mode = backup_mode;
  if (backup_mode == BACKUP_MODE_SIMULATION && !game->backups_preallocated) {
    game->backup_cursor = 0;
    pre_allocate_backups(game);
    game->backups_preallocated = 1;
  }
}

void update_game(const Config *config, Game *game) {
  // Player names are owned by config, so
  // we only need to update the movelist capacity.
  // In the future, we will need to update the board dimensions.
  if (config->num_plays != game->gen->move_list->capacity) {
    destroy_move_list(game->gen->move_list);
    printf("move list capacity changed to %d\n", config->num_plays);
    game->gen->move_list = create_move_list(config->num_plays);
  }
}

Game *create_game(const Config *config) {
  Game *game = malloc_or_die(sizeof(Game));
  game->gen = create_generator(config, config->num_plays);
  for (int player_index = 0; player_index < 2; player_index++) {
    game->players[player_index] = create_player(
        config, player_index,
        players_data_get_name(config->players_data, player_index));
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    game->data_is_shared[i] =
        players_data_get_is_shared(config->players_data, (players_data_t)i);
  }

  game->player_on_turn_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->backup_cursor = 0;
  game->backup_mode = BACKUP_MODE_OFF;
  game->backups_preallocated = 0;
  return game;
}

Game *copy_game(Game *game, int move_list_capacity) {
  Game *new_game = malloc_or_die(sizeof(Game));
  new_game->gen = copy_generator(game->gen, move_list_capacity);
  for (int j = 0; j < 2; j++) {
    new_game->players[j] = copy_player(game->players[j]);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    new_game->data_is_shared[i] = game->data_is_shared[i];
  }
  new_game->player_on_turn_index = game->player_on_turn_index;
  new_game->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
  new_game->game_end_reason = game->game_end_reason;
  // note: game backups must be explicitly handled by the caller if they want
  // game copies to have backups.
  new_game->backup_cursor = 0;
  new_game->backup_mode = BACKUP_MODE_OFF;
  new_game->backups_preallocated = 0;
  return new_game;
}

void backup_game(Game *game) {
  if (game->backup_mode == BACKUP_MODE_OFF) {
    return;
  }
  if (game->backup_mode == BACKUP_MODE_SIMULATION) {
    MinimalGameBackup *state = game->game_backups[game->backup_cursor];
    copy_board_into(state->board, game->gen->board);
    copy_bag_into(state->bag, game->gen->bag);
    state->game_end_reason = game->game_end_reason;
    state->player_on_turn_index = game->player_on_turn_index;
    state->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
    copy_rack_into(state->p0rack, game->players[0]->rack);
    state->p0score = game->players[0]->score;
    copy_rack_into(state->p1rack, game->players[1]->rack);
    state->p1score = game->players[1]->score;

    game->backup_cursor++;
  }
}

void unplay_last_move(Game *game) {
  // restore from backup (pop the last element).
  if (game->backup_cursor == 0) {
    log_fatal("error: no backup\n");
  }
  MinimalGameBackup *state = game->game_backups[game->backup_cursor - 1];
  game->backup_cursor--;

  game->consecutive_scoreless_turns = state->consecutive_scoreless_turns;
  game->game_end_reason = state->game_end_reason;
  game->player_on_turn_index = state->player_on_turn_index;
  game->players[0]->score = state->p0score;
  game->players[1]->score = state->p1score;
  copy_rack_into(game->players[0]->rack, state->p0rack);
  copy_rack_into(game->players[1]->rack, state->p1rack);
  copy_bag_into(game->gen->bag, state->bag);
  copy_board_into(game->gen->board, state->board);
}

void destroy_backups(Game *game) {
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    destroy_rack(game->game_backups[i]->p0rack);
    destroy_rack(game->game_backups[i]->p1rack);
    destroy_bag(game->game_backups[i]->bag);
    destroy_board(game->game_backups[i]->board);
    free(game->game_backups[i]);
  }
}

void destroy_game(Game *game) {
  destroy_generator(game->gen);
  destroy_player(game->players[0]);
  destroy_player(game->players[1]);
  if (game->backups_preallocated) {
    destroy_backups(game);
  }
  free(game);
}

// Human readable print functions

void string_builder_add_player_row(LetterDistribution *letter_distribution,
                                   Player *player, bool player_on_turn,
                                   StringBuilder *game_string) {

  char *player_on_turn_marker = "-> ";
  char *player_off_turn_marker = "   ";
  char *player_marker = player_on_turn_marker;
  if (!player_on_turn) {
    player_marker = player_off_turn_marker;
  }

  string_builder_add_formatted_string(game_string, "%s%s%*s", player_marker,
                                      player->name,
                                      25 - string_length(player->name), "");
  string_builder_add_rack(player->rack, letter_distribution, game_string);
  string_builder_add_formatted_string(game_string, "%*s%d",
                                      10 - player->rack->number_of_letters, "",
                                      player->score);
}

void string_builder_add_board_row(LetterDistribution *letter_distribution,
                                  Board *board, int row,
                                  StringBuilder *game_string) {
  string_builder_add_formatted_string(game_string, "%2d|", row + 1);
  for (int i = 0; i < BOARD_DIM; i++) {
    uint8_t current_letter = get_letter(board, row, i);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      string_builder_add_char(game_string,
                              CROSSWORD_GAME_BOARD[(row * BOARD_DIM) + i]);
    } else {
      string_builder_add_user_visible_letter(letter_distribution,
                                             current_letter, 0, game_string);
    }
    string_builder_add_string(game_string, " ", 0);
  }
  string_builder_add_string(game_string, "|", 0);
}

void string_builder_add_move_with_rank_and_equity(Game *game, int move_index,
                                                  StringBuilder *game_string) {
  Move *move = game->gen->move_list->moves[move_index];
  string_builder_add_int(game_string, move_index + 1);
  string_builder_add_move(game->gen->board, move,
                          game->gen->letter_distribution, game_string);
  string_builder_add_double(game_string, move->equity);
}

void string_builder_add_game(Game *game, StringBuilder *game_string) {
  // TODO: update for super crossword game
  string_builder_add_string(game_string, "   A B C D E F G H I J K L M N O   ",
                            0);
  string_builder_add_player_row(game->gen->letter_distribution,
                                game->players[0],
                                game->player_on_turn_index == 0, game_string);
  string_builder_add_string(game_string,
                            "\n   ------------------------------  ", 0);
  string_builder_add_player_row(game->gen->letter_distribution,
                                game->players[1],
                                game->player_on_turn_index == 1, game_string);
  string_builder_add_string(game_string, "\n", 0);

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_board_row(game->gen->letter_distribution,
                                 game->gen->board, i, game_string);
    if (i == 0) {
      string_builder_add_string(
          game_string, " --Tracking-----------------------------------", 0);
    } else if (i == 1) {
      string_builder_add_string(game_string, " ", 0);
      string_builder_add_bag(game->gen->bag, game->gen->letter_distribution, 0,
                             game_string);

      string_builder_add_formatted_string(game_string, "  %d",
                                          game->gen->bag->last_tile_index + 1);

    } else if (i - 2 < game->gen->move_list->count) {
      string_builder_add_move_with_rank_and_equity(game, i - 2, game_string);
    }
    string_builder_add_string(game_string, "\n", 0);
  }

  string_builder_add_string(game_string, "   ------------------------------\n",
                            0);
}