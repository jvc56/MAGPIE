#include <stdint.h>
#include <stdlib.h>

#include "../def/game_defs.h"
#include "../def/players_data_defs.h"

#include "../str/string_util.h"

#include "bag.h"
#include "board.h"
#include "game.h"
#include "move.h"
#include "movegen.h"
#include "player.h"
#include "rack.h"

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

Generator *game_get_gen(const Game *game) { return game->gen; }

Player *game_get_player(const Game *game, int player_index) {
  return game->players[player_index];
}

int game_get_player_on_turn_index(const Game *game) {
  return game->player_on_turn_index;
}

game_end_reason_t game_get_game_end_reason(const Game *game) {
  return game->game_end_reason;
}

void game_set_game_end_reason(Game *game, game_end_reason_t game_end_reason) {
  game->game_end_reason = game_end_reason;
}

int game_get_player_draw_index(const Game *game, int player_index) {
  return player_index ^ game->starting_player_index;
}

int game_get_player_on_turn_draw_index(const Game *game) {
  return game_get_player_draw_index(game, game->player_on_turn_index);
}

backup_mode_t game_get_backup_mode(const Game *game) {
  return game->backup_mode;
}

int game_get_consecutive_scoreless_turns(const Game *game) {
  return game->consecutive_scoreless_turns;
}

void game_set_consecutive_scoreless_turns(Game *game, int value) {
  game->consecutive_scoreless_turns = value;
}

void game_increment_consecutive_scoreless_turns(Game *game) {
  game->consecutive_scoreless_turns++;
}

void game_start_next_player_turn(Game *game) {
  game->player_on_turn_index = 1 - game->player_on_turn_index;
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
  int number_of_machine_letters = str_to_machine_letters(
      gen_get_ld(game->gen), letters, false, machine_letters, letters_length);
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  int col_start = *current_column_index;

  if (number_of_machine_letters < 0) {
    cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_BOARD_LETTERS;
  } else {
    for (int i = 0; i < number_of_machine_letters; i++) {
      set_letter(gen_get_board(game->gen), row_start, col_start + i,
                 machine_letters[i]);
      // When placing letters on the board, we
      // assume that player 0 placed all of the tiles
      // for convenience.
      draw_letter(gen_get_bag(game->gen), machine_letters[i], 0);
      incrememt_tiles_played(gen_get_board(game->gen), 1);
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
  for (int i = 0; i < get_array_size(rack); i++) {
    for (int j = 0; j < get_number_of_letter(rack, i); j++) {
      draw_letter(bag, i, player_draw_index);
    }
  }
  return number_of_letters_set;
}

cgp_parse_status_t
parse_cgp_racks_with_string_splitter(const StringSplitter *player_racks,
                                     Game *game) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  int number_of_letters_added =
      draw_rack_from_bag(gen_get_ld(game->gen), gen_get_bag(game->gen),
                         player_get_rack(game->players[0]),
                         string_splitter_get_item(player_racks, 0),
                         game_get_player_draw_index(game, 0));
  if (number_of_letters_added < 0) {
    return CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS;
  }
  number_of_letters_added =
      draw_rack_from_bag(gen_get_ld(game->gen), gen_get_bag(game->gen),
                         player_get_rack(game->players[1]),
                         string_splitter_get_item(player_racks, 1),
                         game_get_player_draw_index(game, 1));
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
    player_set_score(game->players[0],
                     string_to_int(string_splitter_get_item(player_scores, 0)));
    player_set_score(game->players[1],
                     string_to_int(string_splitter_get_item(player_scores, 1)));
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

  Player *player0 = game->players[0];
  Player *player1 = game->players[1];

  generate_all_cross_sets(player_get_kwg(player0), player_get_kwg(player1),
                          gen_get_ld(game->gen), gen_get_board(game->gen),
                          game->data_is_shared[PLAYERS_DATA_TYPE_KWG]);
  update_all_anchors(gen_get_board(game->gen));

  if (game->consecutive_scoreless_turns >= MAX_SCORELESS_TURNS) {
    game->game_end_reason = GAME_END_REASON_CONSECUTIVE_ZEROS;
  } else if (bag_is_empty(gen_get_bag(game->gen)) &&
             (rack_is_empty(player_get_rack(player0)) ||
              rack_is_empty(player_get_rack(player1)))) {
    game->game_end_reason = GAME_END_REASON_STANDARD;
  } else {
    game->game_end_reason = GAME_END_REASON_NONE;
  }
  return cgp_parse_status;
}

int tiles_unseen(const Game *game) {
  Player *player_not_on_turn = game->players[1 - game->player_on_turn_index];
  Rack *player_not_on_turn_rack = player_get_rack(player_not_on_turn);
  int their_rack_tiles = get_number_of_letters(player_not_on_turn_rack);
  return (their_rack_tiles + get_tiles_remaining(gen_get_bag(game->gen)));
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
  LetterDistribution *ld = gen_get_ld(gen);
  uint32_t ld_size = letter_distribution_get_size(ld);
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    game->game_backups[i] = malloc_or_die(sizeof(MinimalGameBackup));
    game->game_backups[i]->bag = create_bag(ld);
    game->game_backups[i]->board = create_board();
    game->game_backups[i]->p0rack = create_rack(ld_size);
    game->game_backups[i]->p1rack = create_rack(ld_size);
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

void update_player(const Config *config, Player *player) {
  int player_index = player_get_index(player);
  PlayersData *players_data = config_get_players_data(config);
  player_set_name(player, players_data_get_name(players_data, player_index));
  player_set_move_sort_type(
      player, players_data_get_move_sort_type(players_data, player_index));
  player_move_record_type(
      player, players_data_get_move_record_type(players_data, player_index));
  player_set_kwg(player, players_data_get_kwg(players_data, player_index));
  player_set_klv(player, players_data_get_klv(players_data, player_index));
  Rack *player_rack = player_get_rack(player);
  update_or_create_rack(
      &player_rack,
      letter_distribution_get_size(config_get_letter_distribution(config)));
}

void update_game(const Config *config, Game *game) {
  // Player names are owned by config, so
  // we only need to update the move_list capacity.
  // In the future, we will need to update the board dimensions.
  for (int player_index = 0; player_index < 2; player_index++) {
    update_player(config, game->players[player_index]);
  }
  update_generator(config, game->gen);
}

Game *create_game(const Config *config) {
  Game *game = malloc_or_die(sizeof(Game));
  game->gen = create_generator(config, config_get_num_plays(config));
  for (int player_index = 0; player_index < 2; player_index++) {
    game->players[player_index] = create_player(config, player_index);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    game->data_is_shared[i] = players_data_get_is_shared(
        config_get_players_data(config), (players_data_t)i);
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
    board_copy(state->board, gen_get_board(game->gen));
    bag_copy(state->bag, gen_get_bag(game->gen));
    state->game_end_reason = game->game_end_reason;
    state->player_on_turn_index = game->player_on_turn_index;
    state->starting_player_index = game->starting_player_index;
    state->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
    Player *player0 = game->players[0];
    Player *player1 = game->players[1];
    rack_copy(state->p0rack, player_get_rack(player0));
    state->p0score = player_get_score(player0);
    rack_copy(state->p1rack, player_get_rack(player1));
    state->p1score = player_get_score(player1);

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

  Player *player0 = game->players[0];
  Player *player1 = game->players[1];
  player_set_score(player0, state->p0score);
  player_set_score(player1, state->p1score);
  rack_copy(player_get_rack(player0), state->p0rack);
  rack_copy(player_get_rack(player1), state->p1rack);
  bag_copy(gen_get_bag(game->gen), state->bag);
  board_copy(gen_get_board(game->gen), state->board);
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
