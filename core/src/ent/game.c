#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/game_defs.h"

#include "board.h"

struct MinimalGameBackup {
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
};

struct Game {
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
};

int get_player_draw_index(Game *game, int player_index) {
  return player_index ^ game->starting_player_index;
}

int get_player_on_turn_draw_index(Game *game) {
  return get_player_draw_index(game, game->player_on_turn_index);
}

void draw_letter_to_rack(Bag *bag, Rack *rack, uint8_t letter,
                         int player_draw_index) {
  draw_letter(bag, letter, player_draw_index);
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
  size_t letters_length = string_length(letters);
  uint8_t *machine_letters = malloc_or_die(sizeof(uint8_t) * letters_length);
  int number_of_machine_letters =
      str_to_machine_letters(game->gen->letter_distribution, letters, false,
                             machine_letters, letters_length);
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  int col_start = *current_column_index;

  if (number_of_machine_letters < 0) {
    cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_BOARD_LETTERS;
  } else {
    for (int i = 0; i < number_of_machine_letters; i++) {
      set_letter(game->gen->board, row_start, col_start + i,
                 machine_letters[i]);
      // When placing letters on the board, we
      // assume that player 0 placed all of the tiles
      // for convenience.
      draw_letter(game->gen->bag, machine_letters[i], 0);
      incrememt_tiles_played(game->gen->board, 1);
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

int draw_rack_from_bag(const LetterDistribution *letter_distribution, Bag *bag,
                       Rack *rack, const char *rack_string,
                       int player_draw_index) {
  int number_of_letters_set =
      set_rack_to_string(letter_distribution, rack, rack_string);
  for (int i = 0; i < rack->array_size; i++) {
    for (int j = 0; j < rack->array[i]; j++) {
      draw_letter(bag, i, player_draw_index);
    }
  }
  return number_of_letters_set;
}

cgp_parse_status_t
parse_cgp_racks_with_string_splitter(const StringSplitter *player_racks,
                                     Game *game) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  int number_of_letters_added = draw_rack_from_bag(
      game->gen->letter_distribution, game->gen->bag, game->players[0]->rack,
      string_splitter_get_item(player_racks, 0),
      get_player_draw_index(game, 0));
  if (number_of_letters_added < 0) {
    return CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS;
  }
  number_of_letters_added = draw_rack_from_bag(
      game->gen->letter_distribution, game->gen->bag, game->players[1]->rack,
      string_splitter_get_item(player_racks, 1),
      get_player_draw_index(game, 1));
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
    cgp_parse_status = parse_cgp_racks_with_string_splitter(player_racks, game);
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

cgp_parse_status_t parse_cgp_with_cgp_fields(const StringSplitter *cgp_fields,
                                             Game *game) {
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
    cgp_parse_status = parse_cgp_with_cgp_fields(cgp_fields, game);
  }
  destroy_string_splitter(cgp_fields);
  return cgp_parse_status;
}

cgp_parse_status_t load_cgp(Game *game, const char *cgp) {
  reset_game(game);
  cgp_parse_status_t cgp_parse_status = parse_cgp(game, cgp);
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  game->player_on_turn_index = 0;

  generate_all_cross_sets(game->players[0]->kwg, game->players[1]->kwg,
                          game->gen->letter_distribution, game->gen->board,
                          game->data_is_shared[PLAYERS_DATA_TYPE_KWG]);
  update_all_anchors(game->gen->board);

  if (game->consecutive_scoreless_turns >= MAX_SCORELESS_TURNS) {
    game->game_end_reason = GAME_END_REASON_CONSECUTIVE_ZEROS;
  } else if (bag_is_empty(game->gen->bag) &&
             (game->players[0]->rack->empty || game->players[1]->rack->empty)) {
    game->game_end_reason = GAME_END_REASON_STANDARD;
  } else {
    game->game_end_reason = GAME_END_REASON_NONE;
  }
  return cgp_parse_status;
}

int tiles_unseen(const Game *game) {
  int their_rack_tiles =
      game->players[1 - game->player_on_turn_index]->rack->number_of_letters;

  return (their_rack_tiles + get_tiles_remaining(game->gen->bag));
}

void reset_game(Game *game) {
  reset_generator(game->gen);
  reset_player(game->players[0]);
  reset_player(game->players[1]);
  game->player_on_turn_index = 0;
  game->starting_player_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->backup_cursor = 0;
}

// This assumes the game has not started yet.
void set_starting_player_index(Game *game, int starting_player_index) {
  game->starting_player_index = starting_player_index;
  game->player_on_turn_index = starting_player_index;
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
    game->backups_preallocated = true;
  }
}

void update_game(const Config *config, Game *game) {
  // Player names are owned by config, so
  // we only need to update the movelist capacity.
  // In the future, we will need to update the board dimensions.
  for (int player_index = 0; player_index < 2; player_index++) {
    update_player(config, game->players[player_index]);
  }
  update_generator(config, game->gen);
}

Game *create_game(const Config *config) {
  Game *game = malloc_or_die(sizeof(Game));
  game->gen = create_generator(config, config->num_plays);
  for (int player_index = 0; player_index < 2; player_index++) {
    game->players[player_index] = create_player(config, player_index);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    game->data_is_shared[i] =
        players_data_get_is_shared(config->players_data, (players_data_t)i);
  }

  game->starting_player_index = 0;
  game->player_on_turn_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->backup_cursor = 0;
  game->backup_mode = BACKUP_MODE_OFF;
  game->backups_preallocated = false;
  return game;
}

Game *game_duplicate(const Game *game, int move_list_capacity) {
  Game *new_game = malloc_or_die(sizeof(Game));
  new_game->gen = generate_duplicate(game->gen, move_list_capacity);
  for (int j = 0; j < 2; j++) {
    new_game->players[j] = player_duplicate(game->players[j]);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    new_game->data_is_shared[i] = game->data_is_shared[i];
  }
  new_game->player_on_turn_index = game->player_on_turn_index;
  new_game->starting_player_index = game->starting_player_index;
  new_game->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
  new_game->game_end_reason = game->game_end_reason;
  // note: game backups must be explicitly handled by the caller if they want
  // game copies to have backups.
  new_game->backup_cursor = 0;
  new_game->backup_mode = BACKUP_MODE_OFF;
  new_game->backups_preallocated = false;
  return new_game;
}

void backup_game(Game *game) {
  if (game->backup_mode == BACKUP_MODE_OFF) {
    return;
  }
  if (game->backup_mode == BACKUP_MODE_SIMULATION) {
    MinimalGameBackup *state = game->game_backups[game->backup_cursor];
    board_copy(state->board, game->gen->board);
    bag_copy(state->bag, game->gen->bag);
    state->game_end_reason = game->game_end_reason;
    state->player_on_turn_index = game->player_on_turn_index;
    state->starting_player_index = game->starting_player_index;
    state->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
    rack_copy(state->p0rack, game->players[0]->rack);
    state->p0score = game->players[0]->score;
    rack_copy(state->p1rack, game->players[1]->rack);
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
  game->starting_player_index = state->starting_player_index;
  game->players[0]->score = state->p0score;
  game->players[1]->score = state->p1score;
  rack_copy(game->players[0]->rack, state->p0rack);
  rack_copy(game->players[1]->rack, state->p1rack);
  bag_copy(game->gen->bag, state->bag);
  board_copy(game->gen->board, state->board);
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

void string_builder_add_player_row(
    const LetterDistribution *letter_distribution, const Player *player,
    StringBuilder *game_string, bool player_on_turn) {

  const char *player_on_turn_marker = "-> ";
  const char *player_off_turn_marker = "   ";
  const char *player_marker = player_on_turn_marker;
  if (!player_on_turn) {
    player_marker = player_off_turn_marker;
  }

  char *player_name;
  if (player->name) {
    player_name = string_duplicate(player->name);
  } else {
    player_name = get_formatted_string("player%d", player->index + 1);
  }

  string_builder_add_formatted_string(game_string, "%s%s%*s", player_marker,
                                      player_name,
                                      25 - string_length(player_name), "");
  string_builder_add_rack(player->rack, letter_distribution, game_string);
  string_builder_add_formatted_string(game_string, "%*s%d",
                                      10 - player->rack->number_of_letters, "",
                                      player->score);
  free(player_name);
}

void string_builder_add_board_row(const LetterDistribution *letter_distribution,
                                  const Board *board,
                                  StringBuilder *game_string, int row) {
  string_builder_add_formatted_string(game_string, "%2d|", row + 1);
  for (int i = 0; i < BOARD_DIM; i++) {
    uint8_t current_letter = get_letter(board, row, i);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      string_builder_add_char(game_string,
                              CROSSWORD_GAME_BOARD[(row * BOARD_DIM) + i]);
    } else {
      string_builder_add_user_visible_letter(letter_distribution, game_string,
                                             current_letter);
    }
    string_builder_add_string(game_string, " ");
  }
  string_builder_add_string(game_string, "|");
}

void string_builder_add_move_with_rank_and_equity(const Game *game,
                                                  StringBuilder *game_string,
                                                  int move_index) {
  const Move *move = game->gen->move_list->moves[move_index];
  string_builder_add_formatted_string(game_string, " %d ", move_index + 1);
  string_builder_add_move(game->gen->board, move,
                          game->gen->letter_distribution, game_string);
  string_builder_add_formatted_string(game_string, " %0.2f", move->equity);
}

void string_builder_add_game(const Game *game, StringBuilder *game_string) {
  // TODO: update for super crossword game
  string_builder_add_string(game_string, "   A B C D E F G H I J K L M N O   ");
  string_builder_add_player_row(game->gen->letter_distribution,
                                game->players[0], game_string,
                                game->player_on_turn_index == 0);
  string_builder_add_string(game_string,
                            "\n   ------------------------------  ");
  string_builder_add_player_row(game->gen->letter_distribution,
                                game->players[1], game_string,
                                game->player_on_turn_index == 1);
  string_builder_add_string(game_string, "\n");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_board_row(game->gen->letter_distribution,
                                 game->gen->board, game_string, i);
    if (i == 0) {
      string_builder_add_string(
          game_string, " --Tracking-----------------------------------");
    } else if (i == 1) {
      string_builder_add_string(game_string, " ");
      string_builder_add_bag(game->gen->bag, game->gen->letter_distribution,
                             game_string);

      string_builder_add_formatted_string(game_string, "  %d",
                                          get_tiles_remaining(game->gen->bag));

    } else if (i - 2 < game->gen->move_list->count) {
      string_builder_add_move_with_rank_and_equity(game, game_string, i - 2);
    }
    string_builder_add_string(game_string, "\n");
  }

  string_builder_add_string(game_string, "   ------------------------------\n");
}