#include "validated_move.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/validated_move_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "bag.h"
#include "board.h"
#include "equity.h"
#include "game.h"
#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "player.h"
#include "rack.h"
#include "static_eval.h"
#include "words.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

enum { MOVE_MAX_FIELDS = 5 };

typedef struct ValidatedMove {
  Move *move;
  FormedWords *formed_words;
  Rack *rack;
  Rack *leave;
  Equity challenge_points;
  bool challenge_turn_loss;
  bool unknown_exchange;
  Equity leave_value;
} ValidatedMove;

struct ValidatedMoves {
  ValidatedMove **moves;
  int number_of_moves;
};

int get_letter_coords(const char c) {
  if (isupper(c)) {
    return c - 'A';
  }
  return c - 'a';
}

bool is_exchange_allowed(const Bag *bag) {
  return bag_get_letters(bag) >= RACK_SIZE;
}

void validate_coordinates(const Board *board, Move *move,
                          const char *coords_string, ErrorStack *error_stack) {
  int row_start = 0;
  int col_start = 0;
  size_t coords_string_length = string_length(coords_string);
  bool started_row_parse = false;
  bool started_col_parse = false;
  for (size_t i = 0; i < coords_string_length; i++) {
    char position_char = coords_string[i];
    if (isdigit(position_char)) {
      if (i == 0) {
        move_set_dir(move, BOARD_HORIZONTAL_DIRECTION);
        started_row_parse = true;
      } else if (isalpha(coords_string[i - 1])) {
        if (started_row_parse) {
          error_stack_push(
              error_stack,
              ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION,
              get_formatted_string("failed to parse move coordinates '%s'",
                                   coords_string));
          return;
        }
        started_row_parse = true;
      }
      // Build the 1-indexed row_start
      row_start = row_start * 10 + (position_char - '0');
    } else if (isalpha(position_char)) {
      if (i == 0) {
        move_set_dir(move, BOARD_VERTICAL_DIRECTION);
        started_col_parse = true;
      } else if (isdigit(coords_string[i - 1])) {
        if (started_col_parse) {
          error_stack_push(
              error_stack,
              ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION,
              get_formatted_string("failed to parse move coordinates: '%s'",
                                   coords_string));
          return;
        }
        started_col_parse = true;
      }
      col_start = get_letter_coords(position_char);
    } else {
      error_stack_push(
          error_stack,
          ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION,
          get_formatted_string("failed to parse move coordinates: '%s'",
                               coords_string));
      return;
    }
  }
  // Convert the 1-index row start into 0-indexed row start
  row_start--;

  if (col_start < 0 || col_start >= BOARD_DIM || row_start < 0 ||
      row_start >= BOARD_DIM || !started_row_parse || !started_col_parse) {
    error_stack_push(
        error_stack,
        ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION,
        get_formatted_string("failed to parse move coordinates: '%s'",
                             coords_string));
    return;
  }

  if ((board_is_dir_vertical(move_get_dir(move)) && row_start > 0 &&
       !board_is_empty_or_bricked(board, row_start - 1, col_start)) ||
      (!board_is_dir_vertical(move_get_dir(move)) && col_start > 0 &&
       !board_is_empty_or_bricked(board, row_start, col_start - 1))) {
    error_stack_push(
        error_stack, ERROR_STATUS_MOVE_VALIDATION_INVALID_START_COORDS,
        get_formatted_string(
            "move coordinates are not at the start of the play: %s",
            coords_string));
    return;
  }

  move_set_row_start(move, row_start);
  move_set_col_start(move, col_start);
}

bool play_connects(const Board *board, int row, int col) {
  return !board_are_all_adjacent_squares_empty(board, row, col) ||
         (board_get_tiles_played(board) == 0 &&
          (board_get_anchor(board, row, col, BOARD_HORIZONTAL_DIRECTION) ||
           board_get_anchor(board, row, col, BOARD_VERTICAL_DIRECTION)));
}

void validate_tiles_played_with_mls(const Board *board,
                                    const MachineLetter *machine_letters,
                                    int number_of_machine_letters, Move *move,
                                    Rack *tiles_played_rack,
                                    bool allow_playthrough,
                                    ErrorStack *error_stack) {
  // Set all the tiles and later overwrite them with
  // playthrough markers if it was a tile placement move
  for (int i = 0; i < number_of_machine_letters; i++) {
    MachineLetter ml = machine_letters[i];
    move_set_tile(move, ml, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      if (get_is_blanked(ml)) {
        rack_add_letter(tiles_played_rack, BLANK_MACHINE_LETTER);
      } else {
        rack_add_letter(tiles_played_rack, ml);
      }
    }
  }
  move_set_tiles_length(move, number_of_machine_letters);

  if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
    move_set_tiles_played(move, number_of_machine_letters);
  } else { // This is guarantted to be a tiles played event type
    // Calculate tiles played
    int tiles_played = 0;
    int current_row = move_get_row_start(move);
    int current_col = move_get_col_start(move);
    int move_dir = move_get_dir(move);
    bool connected = false;
    for (int i = 0; i < number_of_machine_letters; i++) {
      if (!board_is_position_in_bounds(current_row, current_col)) {
        error_stack_push(
            error_stack,
            ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OUT_OF_BOUNDS,
            string_duplicate("move is out of bounds"));
        return;
      }
      if (board_get_is_brick(board, current_row, current_col)) {
        error_stack_push(
            error_stack, ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OVER_BRICK,
            string_duplicate("move is played over a bricked square"));
        return;
      }
      MachineLetter board_letter =
          board_get_letter(board, current_row, current_col);
      MachineLetter ml = machine_letters[i];
      if (board_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        move_set_tile(move, ml, i);
        tiles_played++;
        if (!connected && play_connects(board, current_row, current_col)) {
          connected = true;
        }
      } else if (board_letter == ml ||
                 (allow_playthrough && ml == PLAYED_THROUGH_MARKER)) {
        move_set_tile(move, PLAYED_THROUGH_MARKER, i);
        if (ml != PLAYED_THROUGH_MARKER) {
          if (get_is_blanked(board_letter)) {
            rack_take_letter(tiles_played_rack, BLANK_MACHINE_LETTER);
          } else {
            rack_take_letter(tiles_played_rack, board_letter);
          }
        }
      } else {
        error_stack_push(
            error_stack,
            ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_BOARD_MISMATCH,
            string_duplicate("move tiles do not match existing board tiles"));
        return;
      }
      current_row += move_dir;
      current_col += 1 - move_dir;
    }
    if (board_is_position_in_bounds(current_row, current_col) &&
        !board_is_empty_or_bricked(board, current_row, current_col)) {
      error_stack_push(
          error_stack, ERROR_STATUS_MOVE_VALIDATION_INCOMPLETE_TILE_PLACEMENT,
          get_formatted_string("tile placement move is missing connected tiles "
                               "at the end of the play"));
      return;
    }
    if (!connected) {
      error_stack_push(
          error_stack, ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_DISCONNECTED,
          string_duplicate("move tiles are not connected to existing board "
                           "tiles or do not occupy the start square"));
      return;
    }
    if (tiles_played > (RACK_SIZE)) {
      error_stack_push(
          error_stack, ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OVERFLOW,
          get_formatted_string(
              "more tiles played (%d) than can fit in the rack (%d)",
              tiles_played, RACK_SIZE));
      return;
    }
    move_set_tiles_played(move, tiles_played);
  }
}

// Sets the tiles_played_rack
void validate_tiles_played(const LetterDistribution *ld, const Board *board,
                           Move *move, const char *tiles_played,
                           Rack *tiles_played_rack, bool allow_playthrough,
                           ErrorStack *error_stack) {
  size_t machine_letters_size = string_length(tiles_played) + 1;
  MachineLetter *machine_letters =
      malloc_or_die(sizeof(MachineLetter) * machine_letters_size);
  int number_of_machine_letters =
      ld_str_to_mls(ld, tiles_played, allow_playthrough, machine_letters,
                    machine_letters_size);
  if (number_of_machine_letters < 1) {
    error_stack_push(
        error_stack, ERROR_STATUS_MOVE_VALIDATION_INVALID_TILES_PLAYED,
        get_formatted_string("failed to parse played tiles: %s", tiles_played));
  } else if (number_of_machine_letters > BOARD_DIM) {
    error_stack_push(
        error_stack, ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OUT_OF_BOUNDS,
        get_formatted_string(
            "played more tiles (%d) than can fit on the board (%d)",
            tiles_played, BOARD_DIM));
  } else {
    validate_tiles_played_with_mls(
        board, machine_letters, number_of_machine_letters, move,
        tiles_played_rack, allow_playthrough, error_stack);
  }
  free(machine_letters);
}

void validate_split_move(const StringSplitter *split_move, const Game *game,
                         ValidatedMove *vm, int player_index,
                         Rack *tiles_played_rack, bool allow_unknown_exchanges,
                         bool allow_playthrough, ErrorStack *error_stack) {
  // This function handles the following UCGI move string types.
  // Any strings not conforming to the UCGI standard will result
  // in an error status.

  // horizontal, full rack unknown. No challenge pts, no challenge turn loss
  // h4.HADJI.ADHIJ.0.0

  // same as above
  // h4.HADJI

  // vertical play, through a letter;
  // letter will need to be determined by the history
  // firefang was wrongly challenged and so the opponent loses their turn
  // 11d.FIREFANG.AEFGINR.0.1

  // chthonic was played with a blank O, through
  // some letter. It gets an extra 5-pt bonus.
  // 3m.CHTHoNIC.CCHIN?T.5.0

  // exchange ABC from a rack of ABCDEFG
  // ex.ABC.ABCDEFG

  // exchange 4 unknown tiles.
  // ex.4

  // exchange ABC from a rack of ABC
  // ex.ABC

  // A pass move
  // pass

  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);
  const Bag *bag = game_get_bag(game);
  int number_of_fields = string_splitter_get_number_of_items(split_move);

  if (number_of_fields > MOVE_MAX_FIELDS) {
    error_stack_push(error_stack, ERROR_STATUS_MOVE_VALIDATION_EXCESS_FIELDS,
                     get_formatted_string(
                         "move has too %d fields but expected a maxmimum of %d",
                         number_of_fields, MOVE_MAX_FIELDS));
    return;
  }

  // Validate move position
  const char *move_type_or_coords = string_splitter_get_item(split_move, 0);

  if (is_string_empty_or_whitespace(move_type_or_coords)) {
    error_stack_push(error_stack,
                     ERROR_STATUS_MOVE_VALIDATION_EMPTY_MOVE_TYPE_OR_POSITION,
                     string_duplicate("missing move type or position"));
    return;
  }

  if (strings_iequal(move_type_or_coords, UCGI_PASS_MOVE)) {
    move_set_as_pass(vm->move);
  } else if (strings_iequal(move_type_or_coords, UCGI_EXCHANGE_MOVE)) {
    if (!is_exchange_allowed(bag)) {
      error_stack_push(
          error_stack, ERROR_STATUS_MOVE_VALIDATION_EXCHANGE_INSUFFICIENT_TILES,
          get_formatted_string(
              "cannot exchange with fewer than %d tiles in the bag",
              RACK_SIZE));
      return;
    }
    // Equity is set later for tile placement moves
    move_set_type(vm->move, GAME_EVENT_EXCHANGE);
    move_set_score(vm->move, 0);
  } else {
    // Score and equity are set later for tile placement moves
    move_set_type(vm->move, GAME_EVENT_TILE_PLACEMENT_MOVE);
    validate_coordinates(board, vm->move, move_type_or_coords, error_stack);
  }

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  game_event_t move_type = move_get_type(vm->move);

  if (number_of_fields == 1) {
    if (move_type != GAME_EVENT_PASS) {
      error_stack_push(error_stack, ERROR_STATUS_MOVE_VALIDATION_MISSING_FIELDS,
                       string_duplicate("move is missing required fields"));
    }
    return;
  }

  // Validate tiles played or number exchanged
  const char *tiles_or_exchange_or_pass_rack =
      string_splitter_get_item(split_move, 1);

  if (is_string_empty_or_whitespace(tiles_or_exchange_or_pass_rack)) {
    error_stack_push(
        error_stack,
        ERROR_STATUS_MOVE_VALIDATION_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED,
        string_duplicate("move is missing tiles played or number exchanged"));
    return;
  }

  if (is_all_digits_or_empty(tiles_or_exchange_or_pass_rack)) {
    if (move_type != GAME_EVENT_EXCHANGE) {
      error_stack_push(
          error_stack, ERROR_STATUS_MOVE_VALIDATION_NONEXCHANGE_NUMERIC_TILES,
          get_formatted_string("got a numeric value for non-exchange move: %s",
                               tiles_or_exchange_or_pass_rack));
      return;
    }
    int number_exchanged =
        string_to_int(tiles_or_exchange_or_pass_rack, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_MOVE_VALIDATION_INVALID_NUMBER_EXCHANGED,
          get_formatted_string("exchanged an invalid number of tiles: %d",
                               number_exchanged));
      return;
    }
    if (number_exchanged < 1 || number_exchanged > (RACK_SIZE)) {
      error_stack_push(
          error_stack, ERROR_STATUS_MOVE_VALIDATION_INVALID_NUMBER_EXCHANGED,
          get_formatted_string("exchanged an invalid number of tiles: %d",
                               number_exchanged));
      return;
    }
    if (!allow_unknown_exchanges) {
      error_stack_push(error_stack,
                       ERROR_STATUS_MOVE_VALIDATION_UNKNOWN_EXCHANGE_DISALLOWED,
                       string_duplicate("got an numberic value for exchange "
                                        "when the exact tiles were expected"));
      return;
    }
    move_set_tiles_played(vm->move, number_exchanged);
    move_set_tiles_length(vm->move, number_exchanged);
    vm->unknown_exchange = true;
  } else if (move_type != GAME_EVENT_PASS) {
    validate_tiles_played(ld, board, vm->move, tiles_or_exchange_or_pass_rack,
                          tiles_played_rack, allow_playthrough, error_stack);
  } else if (number_of_fields > 2) {
    error_stack_push(error_stack,
                     ERROR_STATUS_MOVE_VALIDATION_EXCESS_PASS_FIELDS,
                     string_duplicate("pass move contains too many fields"));
    return;
  }

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Validate rack
  int rack_string_index = 2;
  if (move_type == GAME_EVENT_PASS) {
    rack_string_index = 1;
  }

  if (rack_string_index > number_of_fields - 1) {
    return;
  }

  const char *rack_string =
      string_splitter_get_item(split_move, rack_string_index);

  if (is_string_empty_or_whitespace(rack_string)) {
    error_stack_push(error_stack, ERROR_STATUS_MOVE_VALIDATION_EMPTY_RACK,
                     string_duplicate("expected rack to be nonempty"));
    return;
  }

  int dist_size = ld_get_size(ld);

  if (!vm->rack) {
    vm->rack = rack_create(dist_size);
  }

  int number_of_rack_letters_set =
      rack_set_to_string(ld, vm->rack, rack_string);

  if (number_of_rack_letters_set < 0 ||
      number_of_rack_letters_set > (RACK_SIZE)) {
    error_stack_push(error_stack, ERROR_STATUS_MOVE_VALIDATION_INVALID_RACK,
                     get_formatted_string("invalid rack: %s", rack_string));
    return;
  }

  // Check if the rack is in the bag
  const Rack *game_player_rack =
      player_get_rack(game_get_player(game, player_index));
  for (int i = 0; i < dist_size; i++) {
    if (rack_get_letter(vm->rack, i) >
        bag_get_letter(bag, i) + rack_get_letter(game_player_rack, i)) {
      error_stack_push(
          error_stack, ERROR_STATUS_MOVE_VALIDATION_RACK_NOT_IN_BAG,
          get_formatted_string("rack '%s' is not available in the bag",
                               rack_string));
      return;
    }
  }

  vm->leave = rack_duplicate(vm->rack);

  // Check if the play is in the rack and
  // set the leave value if it is.
  if (!rack_subtract(vm->leave, tiles_played_rack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_NOT_IN_RACK,
        get_formatted_string("tiles played '%s' are not on the rack '%s'",
                             tiles_or_exchange_or_pass_rack, rack_string));
  } else {
    vm->leave_value = klv_get_leave_value(
        player_get_klv(game_get_player(game, player_index)), vm->leave);
  }

  if (!error_stack_is_empty(error_stack) || number_of_fields <= 3) {
    return;
  }

  if (move_type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    error_stack_push(error_stack,
                     ERROR_STATUS_MOVE_VALIDATION_EXCESS_EXCHANGE_FIELDS,
                     string_duplicate("exchange move has too many fields"));
    return;
  }

  // Validate challenge points
  const char *challenge_points = string_splitter_get_item(split_move, 3);

  if (is_string_empty_or_whitespace(challenge_points)) {
    error_stack_push(
        error_stack, ERROR_STATUS_MOVE_VALIDATION_EMPTY_CHALLENGE_POINTS,
        string_duplicate("expected challenge points to be nonempty"));
    return;
  }

  if (!is_all_digits_or_empty(challenge_points)) {
    error_stack_push(error_stack,
                     ERROR_STATUS_MOVE_VALIDATION_INVALID_CHALLENGE_POINTS,
                     get_formatted_string("invalid challenge points '%s'",
                                          challenge_points));
    return;
  }

  const int challenge_points_int = string_to_int(challenge_points, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_push(error_stack,
                     ERROR_STATUS_MOVE_VALIDATION_INVALID_CHALLENGE_POINTS,
                     get_formatted_string("invalid challenge points '%s'",
                                          challenge_points));
    return;
  }

  vm->challenge_points = int_to_equity(challenge_points_int);

  // Validate challenge turn loss

  const char *challenge_turn_loss = string_splitter_get_item(split_move, 4);

  if (is_string_empty_or_whitespace(challenge_turn_loss)) {
    error_stack_push(
        error_stack, ERROR_STATUS_MOVE_VALIDATION_EMPTY_CHALLENGE_TURN_LOSS,
        string_duplicate("expected challenge turn loss to be nonempty"));
    return;
  }

  if (strings_equal(challenge_turn_loss, "0")) {
    vm->challenge_turn_loss = false;
  } else if (strings_equal(challenge_turn_loss, "1")) {
    vm->challenge_turn_loss = true;
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_MOVE_VALIDATION_INVALID_CHALLENGE_TURN_LOSS,
        get_formatted_string(
            "invalid challenge turn loss '%s' (value must be either 0 or 1)",
            challenge_turn_loss));
    return;
  }
}

void validate_move(ValidatedMove *vm, const Game *game, int player_index,
                   const char *ucgi_move_string, bool allow_unknown_exchanges,
                   bool allow_playthrough, ErrorStack *error_stack) {
  StringSplitter *split_move =
      split_string(ucgi_move_string, UCGI_DELIMITER, false);
  Rack *tiles_played_rack = rack_create(ld_get_size(game_get_ld(game)));
  validate_split_move(split_move, game, vm, player_index, tiles_played_rack,
                      allow_unknown_exchanges, allow_playthrough, error_stack);
  string_splitter_destroy(split_move);
  rack_destroy(tiles_played_rack);
}

void validated_move_load(ValidatedMove *vm, const Game *game, int player_index,
                         const char *ucgi_move_string, bool allow_phonies,
                         bool allow_unknown_exchanges, bool allow_playthrough,
                         ErrorStack *error_stack) {

  if (is_string_empty_or_whitespace(ucgi_move_string)) {
    error_stack_push(error_stack, ERROR_STATUS_MOVE_VALIDATION_EMPTY_MOVE,
                     string_duplicate("expected move to be nonempty"));
    return;
  }
  if (player_index != 0 && player_index != 1) {
    error_stack_push(
        error_stack, ERROR_STATUS_MOVE_VALIDATION_INVALID_PLAYER_INDEX,
        get_formatted_string("invalid player index: %d", player_index + 1));
    return;
  }

  validate_move(vm, game, player_index, ucgi_move_string,
                allow_unknown_exchanges, allow_playthrough, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  game_event_t move_type = move_get_type(vm->move);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, player_index);
  const KLV *klv = player_get_klv(player);
  Board *board = game_get_board(game);
  Equity score = 0;

  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    const KWG *kwg = player_get_kwg(player);
    score = static_eval_get_move_score(
        game_get_ld(game), vm->move, board, game_get_bingo_bonus(game),
        board_get_cross_set_index(
            game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG),
            player_index));

    move_set_score(vm->move, score);

    vm->formed_words = formed_words_create(board, vm->move);

    formed_words_populate_validities(
        kwg, vm->formed_words, game_get_variant(game) == GAME_VARIANT_WORDSMOG);

    if (!allow_phonies) {
      for (int i = 0; i < formed_words_get_num_words(vm->formed_words); i++) {
        if (!formed_words_get_word_valid(vm->formed_words, i)) {
          error_stack_push(
              error_stack, ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED,
              get_formatted_string("expected valid play but detected at least "
                                   "one phony for play: %s",
                                   ucgi_move_string));
          return;
        }
      }
    }
  }

  if (move_type != GAME_EVENT_PASS) {
    if (player_get_move_sort_type(player) == MOVE_SORT_EQUITY) {
      move_set_equity(
          vm->move,
          static_eval_get_move_equity(
              ld, klv, vm->move, vm->leave,
              player_get_rack(game_get_player(game, 1 - player_index)),
              board_get_opening_move_penalties(board),
              board_get_tiles_played(board),
              bag_get_letters(game_get_bag(game))));
    } else {
      move_set_equity(vm->move, score);
    }
  } else {
    move_set_equity(vm->move, EQUITY_PASS_VALUE);
  }
}

ValidatedMove *validated_move_create(const Game *game, int player_index,
                                     const char *ucgi_move_string,
                                     bool allow_phonies,
                                     bool allow_unknown_exchanges,
                                     bool allow_playthrough,
                                     ErrorStack *error_stack) {
  ValidatedMove *vm = malloc_or_die(sizeof(ValidatedMove));
  vm->formed_words = NULL;
  vm->rack = NULL;
  vm->leave = NULL;
  vm->challenge_points = 0;
  vm->challenge_turn_loss = false;
  vm->leave_value = 0;
  vm->unknown_exchange = false;
  vm->move = move_create();

  char *trimmed_ucgi_move_string = string_duplicate(ucgi_move_string);
  trim_whitespace(trimmed_ucgi_move_string);

  validated_move_load(vm, game, player_index, trimmed_ucgi_move_string,
                      allow_phonies, allow_unknown_exchanges, allow_playthrough,
                      error_stack);

  free(trimmed_ucgi_move_string);
  return vm;
}

ValidatedMove *validated_move_duplicate(const ValidatedMove *vm_orig) {
  ValidatedMove *vm_dupe = malloc_or_die(sizeof(ValidatedMove));
  if (vm_orig->formed_words) {
    vm_dupe->formed_words = formed_words_duplicate(vm_orig->formed_words);
  } else {
    vm_dupe->formed_words = NULL;
  }
  if (vm_orig->rack) {
    vm_dupe->rack = rack_duplicate(vm_orig->rack);
  } else {
    vm_dupe->rack = NULL;
  }
  if (vm_orig->leave) {
    vm_dupe->leave = rack_duplicate(vm_orig->leave);
  } else {
    vm_dupe->leave = NULL;
  }
  if (vm_orig->move) {
    vm_dupe->move = move_create();
    move_copy(vm_dupe->move, vm_orig->move);
  } else {
    vm_dupe->move = NULL;
  }
  vm_dupe->challenge_points = vm_orig->challenge_points;
  vm_dupe->challenge_turn_loss = vm_orig->challenge_turn_loss;
  vm_dupe->leave_value = vm_orig->leave_value;
  vm_dupe->unknown_exchange = vm_orig->unknown_exchange;
  return vm_dupe;
}

void validated_move_destroy(ValidatedMove *vm) {
  if (!vm) {
    return;
  }
  move_destroy(vm->move);
  formed_words_destroy(vm->formed_words);
  rack_destroy(vm->rack);
  rack_destroy(vm->leave);
  free(vm);
}

ValidatedMoves *validated_moves_create(const Game *game, int player_index,
                                       const char *ucgi_moves_string,
                                       bool allow_phonies,
                                       bool allow_unknown_exchanges,
                                       bool allow_playthrough,
                                       ErrorStack *error_stack) {
  ValidatedMoves *vms = malloc_or_die(sizeof(ValidatedMoves));
  vms->moves = NULL;
  vms->number_of_moves = 0;

  if (is_string_empty_or_whitespace(ucgi_moves_string)) {
    error_stack_push(error_stack, ERROR_STATUS_MOVE_VALIDATION_EMPTY_MOVE,
                     string_duplicate("expected move to be nonempty"));
  } else {
    StringSplitter *split_moves = split_string(ucgi_moves_string, ',', true);
    vms->number_of_moves = string_splitter_get_number_of_items(split_moves);

    vms->moves = (ValidatedMove **)malloc_or_die(sizeof(ValidatedMove *) *
                                                 vms->number_of_moves);

    for (int i = 0; i < vms->number_of_moves; i++) {
      vms->moves[i] = validated_move_create(
          game, player_index, string_splitter_get_item(split_moves, i),
          allow_phonies, allow_unknown_exchanges, allow_playthrough,
          error_stack);
    }

    string_splitter_destroy(split_moves);
  }

  return vms;
}

ValidatedMoves *validated_moves_duplicate(const ValidatedMoves *vms_orig) {
  if (!vms_orig) {
    return NULL;
  }
  ValidatedMoves *vms_dupe = malloc_or_die(sizeof(ValidatedMoves));
  vms_dupe->moves = (ValidatedMove **)malloc_or_die(sizeof(ValidatedMove *) *
                                                    vms_orig->number_of_moves);
  vms_dupe->number_of_moves = vms_orig->number_of_moves;
  for (int i = 0; i < vms_orig->number_of_moves; i++) {
    vms_dupe->moves[i] = validated_move_duplicate(vms_orig->moves[i]);
  }
  return vms_dupe;
}

void validated_moves_destroy(ValidatedMoves *vms) {
  if (!vms) {
    return;
  }
  for (int i = 0; i < vms->number_of_moves; i++) {
    validated_move_destroy(vms->moves[i]);
  }
  free(vms->moves);
  free(vms);
}

int validated_moves_get_number_of_moves(const ValidatedMoves *vms) {
  return vms->number_of_moves;
}

const Move *validated_moves_get_move(const ValidatedMoves *vms, int i) {
  return vms->moves[i]->move;
}

const FormedWords *validated_moves_get_formed_words(const ValidatedMoves *vms,
                                                    int i) {
  return vms->moves[i]->formed_words;
}

const Rack *validated_moves_get_rack(const ValidatedMoves *vms, int i) {
  return vms->moves[i]->rack;
}

bool validated_moves_get_unknown_exchange(const ValidatedMoves *vms, int i) {
  return vms->moves[i]->unknown_exchange;
}

Equity validated_moves_get_challenge_points(const ValidatedMoves *vms, int i) {
  return vms->moves[i]->challenge_points;
}

bool validated_moves_get_challenge_turn_loss(const ValidatedMoves *vms, int i) {
  return vms->moves[i]->challenge_turn_loss;
}

// Adds moves in vms to ml that do not already exist in ml
// Assumes the movelist is sorted
void validated_moves_add_to_sorted_move_list(const ValidatedMoves *vms,
                                             MoveList *ml) {
  Move **moves_to_add =
      (Move **)malloc_or_die(sizeof(Move *) * vms->number_of_moves);
  int number_of_moves_to_add = 0;
  for (int i = 0; i < vms->number_of_moves; i++) {
    if (!move_list_move_exists(ml, vms->moves[i]->move)) {
      moves_to_add[number_of_moves_to_add++] = vms->moves[i]->move;
    }
  }

  int current_capacity = move_list_get_capacity(ml);
  int current_number_of_moves = move_list_get_count(ml);
  int new_capacity = current_number_of_moves + number_of_moves_to_add;
  if (new_capacity > current_capacity) {
    move_list_resize(ml, new_capacity);
  }

  for (int j = 0; j < number_of_moves_to_add; j++) {
    move_list_add_move_to_sorted_list(ml, moves_to_add[j]);
  }

  free(moves_to_add);
}

void validated_moves_set_rack_to_played_letters(const ValidatedMoves *vms,
                                                int i, Rack *rack_to_set) {
  if (i < 0 || i >= vms->number_of_moves) {
    log_fatal("attempted to get out of bounds move when setting rack to played "
              "tiles, have %d moves but tried to get move %d",
              vms->number_of_moves, i);
  }
  const Move *move = validated_moves_get_move(vms, i);
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    log_fatal("attempted to get set the rack to played tiles for a non-tile "
              "placement move");
  }
  rack_reset(rack_to_set);
  const int tiles_length = move_get_tiles_length(move);
  for (int j = 0; j < tiles_length; j++) {
    const MachineLetter tile = move_get_tile(move, j);
    if (tile != PLAYED_THROUGH_MARKER) {
      if (get_is_blanked(tile)) {
        rack_add_letter(rack_to_set, BLANK_MACHINE_LETTER);
      } else {
        rack_add_letter(rack_to_set, tile);
      }
    }
  }
}

// Returns true if the specified validated move contains
// a phony for the main word and any of the formed words.
bool validated_moves_is_phony(const ValidatedMoves *vms, int vm_index) {
  const Move *move = validated_moves_get_move(vms, vm_index);
  game_event_t move_type = move_get_type(move);
  if (move_type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return false;
  }
  const FormedWords *fw = validated_moves_get_formed_words(vms, vm_index);
  int number_of_words = formed_words_get_num_words(fw);
  for (int i = 0; i < number_of_words; i++) {
    if (!formed_words_get_word_valid(fw, i)) {
      return true;
    }
  }
  return false;
}