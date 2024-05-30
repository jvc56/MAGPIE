#include <ctype.h>

#include "../def/game_defs.h"

#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"

#include "../impl/gameplay.h"

#include "../str/rack_string.h"

cgp_parse_status_t place_letters_on_board(Game *game, const char *letters,
                                          int row_start,
                                          int *current_column_index) {
  size_t letters_length = string_length(letters);
  uint8_t *machine_letters = malloc_or_die(sizeof(uint8_t) * letters_length);
  const LetterDistribution *ld = game_get_ld(game);
  Bag *bag = game_get_bag(game);
  Board *board = game_get_board(game);
  int number_of_machine_letters =
      ld_str_to_mls(ld, letters, false, machine_letters, letters_length);
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  int col_start = *current_column_index;

  if (number_of_machine_letters < 0) {
    cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_BOARD_LETTERS;
  } else {
    for (int i = 0; i < number_of_machine_letters; i++) {
      board_set_letter(board, row_start, col_start + i, machine_letters[i]);
      // When placing letters on the board, we
      // assume that player 0 placed all of the tiles
      // for convenience.
      bool success = bag_draw_letter(bag, machine_letters[i], 0);
      if (!success) {
        cgp_parse_status = CGP_PARSE_STATUS_BOARD_LETTERS_NOT_IN_BAG;
        break;
      }
      board_increment_tiles_played(board, 1);
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

cgp_parse_status_t
parse_cgp_racks_with_string_splitter(const StringSplitter *player_racks,
                                     Game *game) {
  const LetterDistribution *ld = game_get_ld(game);
  Bag *bag = game_get_bag(game);
  for (int player_index = 0; player_index < 2; player_index++) {
    int number_of_letters_added = draw_rack_string_from_bag(
        ld, bag, player_get_rack(game_get_player(game, player_index)),
        string_splitter_get_item(player_racks, player_index),
        game_get_player_draw_index(game, player_index));
    if (number_of_letters_added == -1) {
      return CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS;
    }
    if (number_of_letters_added == -2) {
      return CGP_PARSE_STATUS_RACK_LETTERS_NOT_IN_BAG;
    }
  }
  return CGP_PARSE_STATUS_SUCCESS;
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
  } else {
    for (int player_index = 0; player_index < 2; player_index++) {
      bool success = false;
      int player_score = string_to_int_or_set_error(
          string_splitter_get_item(player_scores, player_index), &success);
      if (!success) {
        cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_SCORES;
      }
      player_set_score(game_get_player(game, player_index), player_score);
    }
  }
  destroy_string_splitter(player_scores);
  return cgp_parse_status;
}

cgp_parse_status_t
parse_cgp_consecutive_zeros(Game *game, const char *cgp_consecutive_zeros) {
  if (!is_all_digits_or_empty(cgp_consecutive_zeros)) {
    return CGP_PARSE_STATUS_MALFORMED_CONSECUTIVE_ZEROS;
  }
  game_set_consecutive_scoreless_turns(game,
                                       string_to_int(cgp_consecutive_zeros));
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

cgp_parse_status_t game_load_cgp(Game *game, const char *cgp) {
  game_reset(game);
  cgp_parse_status_t cgp_parse_status = parse_cgp(game, cgp);
  if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
    return cgp_parse_status;
  }

  game_set_starting_player_index(game, 0);

  game_gen_all_cross_sets(game);
  board_update_all_anchors(game_get_board(game));

  if (game_get_consecutive_scoreless_turns(game) >= MAX_SCORELESS_TURNS) {
    game_set_game_end_reason(game, GAME_END_REASON_CONSECUTIVE_ZEROS);
  } else if (bag_is_empty(game_get_bag(game)) &&
             (rack_is_empty(player_get_rack(game_get_player(game, 0))) ||
              rack_is_empty(player_get_rack(game_get_player(game, 1))))) {
    game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
  } else {
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
  }
  return cgp_parse_status;
}

// Add a CGP to the string builder with only required args:
// - Board
// - Player racks
// - Player scores
// - Number of consecutive scoreless turns
void string_builder_add_cgp(const Game *game, StringBuilder *cgp_builder,
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

  string_builder_add_rack(player_get_rack(player0), ld, cgp_builder);
  string_builder_add_char(cgp_builder, '/');
  string_builder_add_rack(player_get_rack(player1), ld, cgp_builder);

  string_builder_add_char(cgp_builder, ' ');

  string_builder_add_int(cgp_builder, player_get_score(player0));
  string_builder_add_char(cgp_builder, '/');
  string_builder_add_int(cgp_builder, player_get_score(player1));

  string_builder_add_char(cgp_builder, ' ');

  string_builder_add_int(cgp_builder,
                         game_get_consecutive_scoreless_turns(game));
}

char *game_get_cgp(const Game *game, bool write_player_on_turn_first) {
  StringBuilder *cgp_builder = create_string_builder();
  string_builder_add_cgp(game, cgp_builder, write_player_on_turn_first);
  char *cgp = string_builder_dump(cgp_builder, NULL);
  destroy_string_builder(cgp_builder);
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
void string_builder_add_cgp_options(const Config *config,
                                    StringBuilder *cgp_options_builder) {
  PlayersData *players_data = config_get_players_data(config);
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

  int bingo_bonus = config_get_bingo_bonus(config);
  if (bingo_bonus != DEFAULT_BINGO_BONUS) {
    string_builder_add_formatted_string(cgp_options_builder, " bb %d;",
                                        bingo_bonus);
  }

  const BoardLayout *board_layout = config_get_board_layout(config);
  if (!board_layout_is_name_default(board_layout)) {
    string_builder_add_formatted_string(cgp_options_builder, " bdn %s;",
                                        board_layout_get_name(board_layout));
  }

  const char *ld_name = config_get_ld_name(config);
  bool write_ld = false;
  if (kwgs_are_shared) {
    char *default_ld_name = ld_get_default_name(lexicon_name);
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

  game_variant_t game_variant = config_get_game_variant(config);
  if (game_variant != GAME_VARIANT_CLASSIC) {
    char *game_variant_name = get_game_variant_name_from_type(game_variant);
    string_builder_add_formatted_string(cgp_options_builder, " var %s;",
                                        game_variant_name);
    free(game_variant_name);
  }
}

char *game_get_cgp_with_options(const Config *config, const Game *game,
                                bool write_player_on_turn_first) {
  StringBuilder *cgp_with_options_builder = create_string_builder();
  string_builder_add_cgp(game, cgp_with_options_builder,
                         write_player_on_turn_first);
  string_builder_add_cgp_options(config, cgp_with_options_builder);
  char *cgp_with_options = string_builder_dump(cgp_with_options_builder, NULL);
  destroy_string_builder(cgp_with_options_builder);
  return cgp_with_options;
}