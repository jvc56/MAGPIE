#include <ctype.h>

#include "../def/config_defs.h"
#include "../def/game_defs.h"

#include "../ent/bag.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"

#include "../util/io_util.h"

#include "../impl/gameplay.h"

#include "../str/game_string.h"
#include "../str/rack_string.h"

void place_letters_on_board(Game *game, const char *letters, int row_start,
                            int *current_column_index,
                            ErrorStack *error_stack) {
  size_t letters_length = string_length(letters);
  uint8_t *machine_letters = malloc_or_die(sizeof(uint8_t) * letters_length);
  const LetterDistribution *ld = game_get_ld(game);
  Bag *bag = game_get_bag(game);
  Board *board = game_get_board(game);
  int number_of_machine_letters =
      ld_str_to_mls(ld, letters, false, machine_letters, letters_length);
  int col_start = *current_column_index;

  if (number_of_machine_letters < 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_CGP_PARSE_MALFORMED_BOARD_LETTERS,
        get_formatted_string("failed to parse letters for cgp: %s", letters));
  } else {
    for (int i = 0; i < number_of_machine_letters; i++) {
      board_set_letter(board, row_start, col_start + i, machine_letters[i]);
      // When placing letters on the board, we
      // assume that player 0 placed all of the tiles
      // for convenience.
      bool success = bag_draw_letter(bag, machine_letters[i], 0);
      if (!success) {
        char *human_readable_ml_string = ld_ml_to_hl(ld, machine_letters[i]);
        error_stack_push(
            error_stack, ERROR_STATUS_CGP_PARSE_BOARD_LETTERS_NOT_IN_BAG,
            get_formatted_string("cgp contains more of letter %s than is "
                                 "available in the distribution",
                                 human_readable_ml_string));
        free(human_readable_ml_string);
        break;
      }
      board_increment_tiles_played(board, 1);
    }
    *current_column_index = *current_column_index + number_of_machine_letters;
  }
  free(machine_letters);
}

void parse_cgp_board_row(Game *game, const char *cgp_board_row, int row_index,
                         ErrorStack *error_stack) {
  StringBuilder *tile_string_builder = string_builder_create();
  int row_length = string_length(cgp_board_row);

  int current_row_number_of_spaces = 0;
  int current_column_index = 0;
  for (int i = 0; i < row_length; i++) {
    char current_char = cgp_board_row[i];
    if (isdigit(current_char)) {
      current_row_number_of_spaces =
          (current_row_number_of_spaces * 10) + (current_char - '0');
      if (string_builder_length(tile_string_builder) > 0) {
        place_letters_on_board(game, string_builder_peek(tile_string_builder),
                               row_index, &current_column_index, error_stack);
        string_builder_clear(tile_string_builder);
        if (!error_stack_is_empty(error_stack)) {
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
    place_letters_on_board(game, string_builder_peek(tile_string_builder),
                           row_index, &current_column_index, error_stack);
  } else {
    current_column_index += current_row_number_of_spaces;
  }
  string_builder_destroy(tile_string_builder);

  if (current_column_index != BOARD_DIM && error_stack_is_empty(error_stack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_BOARD_COLUMNS,
        string_duplicate("cgp board has an invalid number of columns"));
    return;
  }
}

void parse_cgp_board(Game *game, const char *cgp_board,
                     ErrorStack *error_stack) {
  StringSplitter *board_rows = split_string(cgp_board, '/', true);

  if (string_splitter_get_number_of_items(board_rows) != BOARD_DIM) {
    error_stack_push(
        error_stack, ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_BOARD_ROWS,
        get_formatted_string("cgp board has an invalid number of rows: %d",
                             string_splitter_get_number_of_items(board_rows)));
  } else {
    for (int i = 0; i < BOARD_DIM; i++) {
      parse_cgp_board_row(game, string_splitter_get_item(board_rows, i), i,
                          error_stack);
      if (!error_stack_is_empty(error_stack)) {
        break;
      }
    }
  }
  string_splitter_destroy(board_rows);
}

void parse_cgp_racks_with_string_splitter(const StringSplitter *player_racks,
                                          Game *game, ErrorStack *error_stack) {
  for (int player_index = 0; player_index < 2; player_index++) {
    int number_of_letters_added = draw_rack_string_from_bag(
        game, player_index,
        string_splitter_get_item(player_racks, player_index));
    if (number_of_letters_added == -1) {
      error_stack_push(
          error_stack, ERROR_STATUS_CGP_PARSE_MALFORMED_RACK_LETTERS,
          get_formatted_string(
              "failed to parse rack for player %d: %s", player_index + 1,
              string_splitter_get_item(player_racks, player_index)));
      return;
    }
    if (number_of_letters_added == -2) {
      error_stack_push(
          error_stack, ERROR_STATUS_CGP_PARSE_RACK_LETTERS_NOT_IN_BAG,
          get_formatted_string(
              "rack not available in the bag for player %d: %s",
              player_index + 1,
              string_splitter_get_item(player_racks, player_index)));
      return;
    }
  }
}

void parse_cgp_racks(Game *game, const char *cgp_racks,
                     ErrorStack *error_stack) {
  StringSplitter *player_racks = split_string(cgp_racks, '/', false);
  if (string_splitter_get_number_of_items(player_racks) != 2) {
    error_stack_push(error_stack,
                     ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_PLAYER_RACKS,
                     get_formatted_string(
                         "cgp has an invalid number of racks: %s", cgp_racks));
  } else {
    parse_cgp_racks_with_string_splitter(player_racks, game, error_stack);
  }
  string_splitter_destroy(player_racks);
}

void parse_cgp_scores(Game *game, const char *cgp_scores,
                      ErrorStack *error_stack) {
  StringSplitter *player_scores = split_string(cgp_scores, '/', false);
  if (string_splitter_get_number_of_items(player_scores) != 2) {
    error_stack_push(error_stack,
                     ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_PLAYER_SCORES,
                     get_formatted_string(
                         "cgp has an invalid number of score: %s", cgp_scores));
  } else {
    for (int player_index = 0; player_index < 2; player_index++) {
      int player_score = string_to_int(
          string_splitter_get_item(player_scores, player_index), error_stack);
      if (!error_stack_is_empty(error_stack)) {
        error_stack_push(
            error_stack, ERROR_STATUS_CGP_PARSE_MALFORMED_SCORES,
            get_formatted_string(
                "cgp has invalid score for player %d: %s", player_index + 1,
                string_splitter_get_item(player_scores, player_index)));
        break;
      }
      player_set_score(game_get_player(game, player_index),
                       int_to_equity(player_score));
    }
  }
  string_splitter_destroy(player_scores);
}

void parse_cgp_consecutive_zeros(Game *game, const char *cgp_consecutive_zeros,
                                 ErrorStack *error_stack) {
  if (!is_all_digits_or_empty(cgp_consecutive_zeros)) {
    error_stack_push(error_stack,
                     ERROR_STATUS_CGP_PARSE_MALFORMED_CONSECUTIVE_ZEROS,
                     get_formatted_string(
                         "cgp has an invalid value for consecutive zeros: %s",
                         cgp_consecutive_zeros));
    return;
  }
  const int consecutive_zeros_int =
      string_to_int(cgp_consecutive_zeros, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_push(error_stack,
                     ERROR_STATUS_CGP_PARSE_MALFORMED_CONSECUTIVE_ZEROS,
                     get_formatted_string(
                         "cgp has an invalid value '%s' for consecutive zeros",
                         cgp_consecutive_zeros));
    return;
  }
  game_set_consecutive_scoreless_turns(game, consecutive_zeros_int);
}

void parse_cgp_with_cgp_fields(const StringSplitter *cgp_fields, Game *game,
                               ErrorStack *error_stack) {
  parse_cgp_board(game, string_splitter_get_item(cgp_fields, 0), error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  parse_cgp_racks(game, string_splitter_get_item(cgp_fields, 1), error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  parse_cgp_scores(game, string_splitter_get_item(cgp_fields, 2), error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  parse_cgp_consecutive_zeros(game, string_splitter_get_item(cgp_fields, 3),
                              error_stack);
}

void parse_cgp(Game *game, const char *cgp, ErrorStack *error_stack) {
  StringSplitter *cgp_fields = split_string_by_whitespace(cgp, true);

  if (string_splitter_get_number_of_items(cgp_fields) < 4) {
    error_stack_push(error_stack,
                     ERROR_STATUS_CGP_PARSE_MISSING_REQUIRED_FIELDS,
                     string_duplicate("cgp does not have exactly four fields"));
  } else {
    parse_cgp_with_cgp_fields(cgp_fields, game, error_stack);
  }
  string_splitter_destroy(cgp_fields);
}

void game_load_cgp(Game *game, const char *cgp, ErrorStack *error_stack) {
  game_reset(game);
  parse_cgp(game, cgp, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  game_set_starting_player_index(game, 0);

  game_gen_all_cross_sets(game);
  board_update_all_anchors(game_get_board(game));

  if (game_get_consecutive_scoreless_turns(game) >=
      game_get_max_scoreless_turns(game)) {
    game_set_game_end_reason(game, GAME_END_REASON_CONSECUTIVE_ZEROS);
  } else if (bag_is_empty(game_get_bag(game)) &&
             (rack_is_empty(player_get_rack(game_get_player(game, 0))) ||
              rack_is_empty(player_get_rack(game_get_player(game, 1))))) {
    game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
  } else {
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
  }
}

// Add a CGP to the string builder with only required args:
// - Board
// - Player racks
// - Player scores
// - Number of consecutive scoreless turns
void string_builder_add_cgp(StringBuilder *cgp_builder, const Game *game,
                            bool write_player_on_turn_first) {
  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    int consecutive_empty_squares = 0;
    for (int col = 0; col < BOARD_DIM; col++) {
      uint8_t ml = board_get_letter(board, row, col);
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        consecutive_empty_squares++;
      } else {
        if (consecutive_empty_squares > 0) {
          string_builder_add_int(cgp_builder, consecutive_empty_squares);
          consecutive_empty_squares = 0;
        }
        char *letter_str = ld_ml_to_hl(ld, ml);
        string_builder_add_string(cgp_builder, letter_str);
        free(letter_str);
      }
    }

    if (consecutive_empty_squares > 0) {
      string_builder_add_int(cgp_builder, consecutive_empty_squares);
    }

    if (row != BOARD_DIM - 1) {
      string_builder_add_char(cgp_builder, '/');
    }
  }

  string_builder_add_char(cgp_builder, ' ');

  const Player *player0 = game_get_player(game, 0);
  const Player *player1 = game_get_player(game, 1);

  if (write_player_on_turn_first && game_get_player_on_turn_index(game) == 1) {
    player1 = game_get_player(game, 0);
    player0 = game_get_player(game, 1);
  }

  string_builder_add_rack(cgp_builder, player_get_rack(player0), ld, false);
  string_builder_add_char(cgp_builder, '/');
  string_builder_add_rack(cgp_builder, player_get_rack(player1), ld, false);

  string_builder_add_char(cgp_builder, ' ');

  string_builder_add_int(cgp_builder, equity_to_int(player_get_score(player0)));
  string_builder_add_char(cgp_builder, '/');
  string_builder_add_int(cgp_builder, equity_to_int(player_get_score(player1)));

  string_builder_add_char(cgp_builder, ' ');

  string_builder_add_int(cgp_builder,
                         game_get_consecutive_scoreless_turns(game));
}

char *game_get_cgp(const Game *game, bool write_player_on_turn_first) {
  StringBuilder *cgp_builder = string_builder_create();
  string_builder_add_cgp(cgp_builder, game, write_player_on_turn_first);
  char *cgp = string_builder_dump(cgp_builder, NULL);
  string_builder_destroy(cgp_builder);
  return cgp;
}

// Always adds the following options:
//  - lexicon
//
// Adds the following options if they are not the default:
//  - bingo bonus
//  - board layout
//  - letter distribution
//  - variant
void string_builder_add_cgp_options(StringBuilder *cgp_options_builder,
                                    PlayersData *players_data, int bingo_bonus,
                                    const char *board_layout_name,
                                    const char *ld_name,
                                    game_variant_t game_variant) {
  bool kwgs_are_shared =
      players_data_get_is_shared(players_data, PLAYERS_DATA_TYPE_KWG);
  const char *lexicon_name = NULL;
  if (kwgs_are_shared) {
    lexicon_name =
        players_data_get_data_name(players_data, PLAYERS_DATA_TYPE_KWG, 0);
    string_builder_add_formatted_string(cgp_options_builder, " lex %s;",
                                        lexicon_name);
  } else {
    string_builder_add_formatted_string(
        cgp_options_builder, " l1 %s; l2 %s;",
        players_data_get_data_name(players_data, PLAYERS_DATA_TYPE_KWG, 0),
        players_data_get_data_name(players_data, PLAYERS_DATA_TYPE_KWG, 1));
  }

  if (bingo_bonus != DEFAULT_BINGO_BONUS) {
    string_builder_add_formatted_string(cgp_options_builder, " bb %d;",
                                        bingo_bonus);
  }

  if (!board_layout_is_name_default(board_layout_name)) {
    string_builder_add_formatted_string(cgp_options_builder, " bdn %s;",
                                        board_layout_name);
  }

  bool write_ld = false;
  if (kwgs_are_shared) {
    // Throwing a fatal error here is okay since this function is only called by
    // tests.
    ErrorStack *error_stack = error_stack_create();
    char *default_ld_name =
        ld_get_default_name_from_lexicon_name(lexicon_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      log_fatal("could not get default letter distribution name");
    }
    error_stack_destroy(error_stack);
    if (!strings_equal(ld_name, default_ld_name)) {
      write_ld = true;
    }
    free(default_ld_name);
  } else {
    write_ld = true;
  }

  if (write_ld) {
    string_builder_add_formatted_string(cgp_options_builder, " ld %s;",
                                        ld_name);
  }

  if (game_variant != GAME_VARIANT_CLASSIC) {
    string_builder_add_string(cgp_options_builder, " var ");
    string_builder_add_game_variant(cgp_options_builder, game_variant);
    string_builder_add_string(cgp_options_builder, ";");
  }
}

char *game_get_cgp_with_options(const Game *game,
                                bool write_player_on_turn_first,
                                PlayersData *players_data, int bingo_bonus,
                                const char *board_layout_name,
                                const char *ld_name,
                                game_variant_t game_variant) {
  StringBuilder *cgp_with_options_builder = string_builder_create();
  string_builder_add_cgp(cgp_with_options_builder, game,
                         write_player_on_turn_first);
  string_builder_add_cgp_options(cgp_with_options_builder, players_data,
                                 bingo_bonus, board_layout_name, ld_name,
                                 game_variant);
  char *cgp_with_options = string_builder_dump(cgp_with_options_builder, NULL);
  string_builder_destroy(cgp_with_options_builder);
  return cgp_with_options;
}