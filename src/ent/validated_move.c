#include "validated_move.h"

#include <ctype.h>

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/validated_move_defs.h"

#include "game.h"
#include "move.h"
#include "static_eval.h"
#include "words.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

typedef struct ValidatedMove {
  Move *move;
  FormedWords *formed_words;
  Rack *rack;
  Rack *leave;
  int challenge_points;
  bool challenge_turn_loss;
  bool unknown_exchange;
  double leave_value;
  move_validation_status_t status;
} ValidatedMove;

struct ValidatedMoves {
  ValidatedMove **moves;
  int number_of_moves;
  move_validation_status_t final_status;
};

int get_letter_coords(const char c) {
  if (isupper(c)) {
    return c - 'A';
  }
  return c - 'a';
}

bool is_exchange_allowed(const Bag *bag) {
  return bag_get_tiles(bag) >= RACK_SIZE;
}

move_validation_status_t validate_coordinates(Move *move,
                                              const char *coords_string) {
  int row_start = 0;
  int col_start = 0;
  int coords_string_length = string_length(coords_string);
  bool started_row_parse = false;
  bool started_col_parse = false;
  for (int i = 0; i < coords_string_length; i++) {
    char position_char = coords_string[i];
    if (isdigit(position_char)) {
      if (i == 0) {
        move_set_dir(move, BOARD_HORIZONTAL_DIRECTION);
        started_row_parse = true;
      } else if (isalpha(coords_string[i - 1])) {
        if (started_row_parse) {
          return MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION;
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
          return MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION;
        }
        started_col_parse = true;
      }
      col_start = get_letter_coords(position_char);
    } else {
      return MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION;
    }
  }
  // Convert the 1-index row start into 0-indexed row start
  row_start--;

  if (col_start < 0 || col_start >= BOARD_DIM || row_start < 0 ||
      row_start >= BOARD_DIM || !started_row_parse || !started_col_parse) {
    return MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION;
  }

  move_set_row_start(move, row_start);
  move_set_col_start(move, col_start);

  return MOVE_VALIDATION_STATUS_SUCCESS;
}

bool play_connects(const Board *board, int row, int col) {
  return !board_are_all_adjacent_squares_empty(board, row, col) ||
         (board_get_tiles_played(board) == 0 &&
          (board_get_anchor(board, row, col, BOARD_HORIZONTAL_DIRECTION) ||
           board_get_anchor(board, row, col, BOARD_VERTICAL_DIRECTION)));
}

move_validation_status_t validate_tiles_played_with_mls(
    const Board *board, const uint8_t *machine_letters,
    int number_of_machine_letters, Move *move, Rack *tiles_played_rack,
    bool allow_playthrough) {

  move_validation_status_t status = MOVE_VALIDATION_STATUS_SUCCESS;

  // Set all the tiles and later overwrite them with
  // playthrough markers if it was a tile placement move
  for (int i = 0; i < number_of_machine_letters; i++) {
    uint8_t ml = machine_letters[i];
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
        return MOVE_VALIDATION_STATUS_TILES_PLAYED_OUT_OF_BOUNDS;
      }
      if (board_get_is_brick(board, current_row, current_col)) {
        return MOVE_VALIDATION_STATUS_TILES_PLAYED_OVER_BRICK;
      }
      uint8_t board_letter = board_get_letter(board, current_row, current_col);
      uint8_t ml = machine_letters[i];
      if (board_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        move_set_tile(move, ml, i);
        tiles_played++;
        if (!connected && play_connects(board, current_row, current_col)) {
          connected = true;
        }
      } else if (board_letter == ml || allow_playthrough) {
        move_set_tile(move, PLAYED_THROUGH_MARKER, i);
        if (ml != PLAYED_THROUGH_MARKER) {
          if (get_is_blanked(board_letter)) {
            rack_take_letter(tiles_played_rack, BLANK_MACHINE_LETTER);
          } else {
            rack_take_letter(tiles_played_rack, board_letter);
          }
        }
      } else {
        return MOVE_VALIDATION_STATUS_TILES_PLAYED_BOARD_MISMATCH;
      }
      current_row += move_dir;
      current_col += 1 - move_dir;
    }
    if (status == MOVE_VALIDATION_STATUS_SUCCESS && !connected) {
      return MOVE_VALIDATION_STATUS_TILES_PLAYED_DISCONNECTED;
    } else if (tiles_played > (RACK_SIZE)) {
      return MOVE_VALIDATION_STATUS_TILES_PLAYED_OVERFLOW;
    } else {
      move_set_tiles_played(move, tiles_played);
    }
  }
  return status;
}

// Sets the tiles_played_rack
move_validation_status_t validate_tiles_played(const LetterDistribution *ld,
                                               const Board *board, Move *move,
                                               const char *tiles_played,
                                               Rack *tiles_played_rack,
                                               bool allow_playthrough) {
  int machine_letters_size = string_length(tiles_played) + 1;
  uint8_t *machine_letters =
      malloc_or_die(sizeof(uint8_t) * machine_letters_size);
  int number_of_machine_letters =
      ld_str_to_mls(ld, tiles_played, allow_playthrough, machine_letters,
                    machine_letters_size);

  move_validation_status_t status = MOVE_VALIDATION_STATUS_SUCCESS;

  if (number_of_machine_letters < 1) {
    status = MOVE_VALIDATION_STATUS_INVALID_TILES_PLAYED;
  } else if (number_of_machine_letters > BOARD_DIM) {
    status = MOVE_VALIDATION_STATUS_TILES_PLAYED_OUT_OF_BOUNDS;
  } else {
    status = validate_tiles_played_with_mls(
        board, machine_letters, number_of_machine_letters, move,
        tiles_played_rack, allow_playthrough);
  }

  free(machine_letters);

  return status;
}

move_validation_status_t
validate_split_move(const StringSplitter *split_move, const Game *game,
                    ValidatedMove *vm, int player_index,
                    Rack *tiles_played_rack, bool allow_unknown_exchanges,
                    bool allow_playthrough) {
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
  move_validation_status_t status = MOVE_VALIDATION_STATUS_SUCCESS;
  int number_of_fields = string_splitter_get_number_of_items(split_move);

  if (number_of_fields > 5) {
    return MOVE_VALIDATION_STATUS_EXCESS_FIELDS;
  }

  // Validate move position
  const char *move_type_or_coords = string_splitter_get_item(split_move, 0);

  if (is_string_empty_or_whitespace(move_type_or_coords)) {
    return MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION;
  }

  if (strings_equal(move_type_or_coords, UCGI_PASS_MOVE)) {
    move_set_as_pass(vm->move);
  } else if (strings_equal(move_type_or_coords, UCGI_EXCHANGE_MOVE)) {
    if (!is_exchange_allowed(bag)) {
      return MOVE_VALIDATION_STATUS_EXCHANGE_INSUFFICIENT_TILES;
    }
    // Equity is set later for tile placement moves
    move_set_type(vm->move, GAME_EVENT_EXCHANGE);
    move_set_score(vm->move, 0);
  } else {
    // Score and equity are set later for tile placement moves
    move_set_type(vm->move, GAME_EVENT_TILE_PLACEMENT_MOVE);
    status = validate_coordinates(vm->move, move_type_or_coords);
  }

  if (status != MOVE_VALIDATION_STATUS_SUCCESS) {
    return status;
  }

  game_event_t move_type = move_get_type(vm->move);

  if (number_of_fields == 1) {
    if (move_type == GAME_EVENT_PASS) {
      return MOVE_VALIDATION_STATUS_SUCCESS;
    } else {
      return MOVE_VALIDATION_STATUS_MISSING_FIELDS;
    }
  }

  // Validate tiles played or number exchanged
  const char *tiles_or_exchange_or_pass_rack =
      string_splitter_get_item(split_move, 1);

  if (is_string_empty_or_whitespace(tiles_or_exchange_or_pass_rack)) {
    return MOVE_VALIDATION_STATUS_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED;
  }

  if (is_all_digits_or_empty(tiles_or_exchange_or_pass_rack)) {
    if (move_type != GAME_EVENT_EXCHANGE) {
      return MOVE_VALIDATION_STATUS_NONEXCHANGE_NUMERIC_TILES;
    }
    int number_exchanged = string_to_int(tiles_or_exchange_or_pass_rack);
    if (number_exchanged < 1 || number_exchanged > (RACK_SIZE)) {
      return MOVE_VALIDATION_STATUS_INVALID_NUMBER_EXCHANGED;
    }
    if (!allow_unknown_exchanges) {
      return MOVE_VALIDATION_STATUS_UNKNOWN_EXCHANGE_DISALLOWED;
    }
    move_set_tiles_played(vm->move, number_exchanged);
    move_set_tiles_length(vm->move, number_exchanged);
    vm->unknown_exchange = true;
  } else if (move_type != GAME_EVENT_PASS) {
    status = validate_tiles_played(ld, board, vm->move,
                                   tiles_or_exchange_or_pass_rack,
                                   tiles_played_rack, allow_playthrough);
  } else if (number_of_fields > 2) {
    return MOVE_VALIDATION_STATUS_EXCESS_PASS_FIELDS;
  }

  if (status != MOVE_VALIDATION_STATUS_SUCCESS) {
    return status;
  }

  // Validate rack
  int rack_string_index = 2;
  if (move_type == GAME_EVENT_PASS) {
    rack_string_index = 1;
  }

  if (rack_string_index > number_of_fields - 1) {
    return status;
  }

  const char *rack_string =
      string_splitter_get_item(split_move, rack_string_index);

  if (is_string_empty_or_whitespace(rack_string)) {
    return MOVE_VALIDATION_STATUS_EMPTY_RACK;
  }

  int dist_size = ld_get_size(ld);

  if (!vm->rack) {
    vm->rack = rack_create(dist_size);
  }

  int number_of_rack_letters_set =
      rack_set_to_string(ld, vm->rack, rack_string);

  if (number_of_rack_letters_set < 0 ||
      number_of_rack_letters_set > (RACK_SIZE)) {
    return MOVE_VALIDATION_STATUS_INVALID_RACK;
  }

  // Check if the rack is in the bag
  Rack *game_player_rack = player_get_rack(game_get_player(game, player_index));
  for (int i = 0; i < dist_size; i++) {
    if (rack_get_letter(vm->rack, i) >
        bag_get_letter(bag, i) + rack_get_letter(game_player_rack, i)) {
      return MOVE_VALIDATION_STATUS_RACK_NOT_IN_BAG;
    }
  }

  vm->leave = rack_duplicate(vm->rack);

  // Check if the play is in the rack and
  // set the leave value if it is.
  if (!rack_subtract(vm->leave, tiles_played_rack)) {
    status = MOVE_VALIDATION_STATUS_TILES_PLAYED_NOT_IN_RACK;
  } else {
    vm->leave_value = klv_get_leave_value(
        player_get_klv(game_get_player(game, player_index)), vm->leave);
  }

  if (status != MOVE_VALIDATION_STATUS_SUCCESS || number_of_fields <= 3) {
    return status;
  }

  if (move_type != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return MOVE_VALIDATION_STATUS_EXCESS_EXCHANGE_FIELDS;
  }

  // Validate challenge points
  const char *challenge_points = string_splitter_get_item(split_move, 3);

  if (is_string_empty_or_whitespace(challenge_points)) {
    return MOVE_VALIDATION_STATUS_EMPTY_CHALLENGE_POINTS;
  }

  if (!is_all_digits_or_empty(challenge_points)) {
    return MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_POINTS;
  }

  vm->challenge_points = string_to_int(challenge_points);

  // Validate challenge turn loss

  const char *challenge_turn_loss = string_splitter_get_item(split_move, 4);

  if (is_string_empty_or_whitespace(challenge_turn_loss)) {
    return MOVE_VALIDATION_STATUS_EMPTY_CHALLENGE_TURN_LOSS;
  }

  if (strings_equal(challenge_turn_loss, "0")) {
    vm->challenge_turn_loss = false;
  } else if (strings_equal(challenge_turn_loss, "1")) {
    vm->challenge_turn_loss = true;
  } else {
    return MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_TURN_LOSS;
  }

  return status;
}

move_validation_status_t validate_move(ValidatedMove *vm, const Game *game,
                                       int player_index,
                                       const char *ucgi_move_string,
                                       bool allow_unknown_exchanges,
                                       bool allow_playthrough) {
  StringSplitter *split_move = split_string(ucgi_move_string, '.', false);
  Rack *tiles_played_rack = rack_create(ld_get_size(game_get_ld(game)));

  move_validation_status_t status =
      validate_split_move(split_move, game, vm, player_index, tiles_played_rack,
                          allow_unknown_exchanges, allow_playthrough);

  string_splitter_destroy(split_move);
  rack_destroy(tiles_played_rack);

  return status;
}

move_validation_status_t
validated_move_load(ValidatedMove *vm, const Game *game, int player_index,
                    const char *ucgi_move_string, bool allow_phonies,
                    bool allow_unknown_exchanges, bool allow_playthrough) {

  if (is_string_empty_or_whitespace(ucgi_move_string)) {
    return MOVE_VALIDATION_STATUS_EMPTY_MOVE;
  }
  if (player_index != 0 && player_index != 1) {
    return MOVE_VALIDATION_STATUS_INVALID_PLAYER_INDEX;
  }

  move_validation_status_t status =
      validate_move(vm, game, player_index, ucgi_move_string,
                    allow_unknown_exchanges, allow_playthrough);

  if (status != MOVE_VALIDATION_STATUS_SUCCESS) {
    return status;
  }

  game_event_t move_type = move_get_type(vm->move);
  const LetterDistribution *ld = game_get_ld(game);
  const Player *player = game_get_player(game, player_index);
  const KLV *klv = player_get_klv(player);
  Board *board = game_get_board(game);
  int score = 0;

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
          return MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED;
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
              bag_get_tiles(game_get_bag(game))));
    } else {
      move_set_equity(vm->move, score);
    }
  } else {
    move_set_equity(vm->move, PASS_MOVE_EQUITY);
  }

  return MOVE_VALIDATION_STATUS_SUCCESS;
}

ValidatedMove *validated_move_create(const Game *game, int player_index,
                                     const char *ucgi_move_string,
                                     bool allow_phonies,
                                     bool allow_unknown_exchanges,
                                     bool allow_playthrough) {
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

  vm->status = validated_move_load(vm, game, player_index,
                                   trimmed_ucgi_move_string, allow_phonies,
                                   allow_unknown_exchanges, allow_playthrough);
  free(trimmed_ucgi_move_string);
  return vm;
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
                                       bool allow_playthrough) {
  ValidatedMoves *vms = malloc_or_die(sizeof(ValidatedMoves));
  vms->moves = NULL;
  vms->number_of_moves = 0;
  vms->final_status = MOVE_VALIDATION_STATUS_SUCCESS;

  if (is_string_empty_or_whitespace(ucgi_moves_string)) {
    vms->final_status = MOVE_VALIDATION_STATUS_EMPTY_MOVE;
  } else {
    StringSplitter *split_moves = split_string(ucgi_moves_string, ',', true);
    vms->number_of_moves = string_splitter_get_number_of_items(split_moves);

    vms->moves = malloc_or_die(sizeof(ValidatedMove *) * vms->number_of_moves);

    for (int i = 0; i < vms->number_of_moves; i++) {
      vms->moves[i] = validated_move_create(
          game, player_index, string_splitter_get_item(split_moves, i),
          allow_phonies, allow_unknown_exchanges, allow_playthrough);

      // For now, just set the final status to the first nonsuccess
      // status of the moves. Later we can implement more thorough
      // error reporting.
      if (vms->final_status == MOVE_VALIDATION_STATUS_SUCCESS &&
          vms->moves[i]->status != MOVE_VALIDATION_STATUS_SUCCESS) {
        vms->final_status = vms->moves[i]->status;
      }
    }

    string_splitter_destroy(split_moves);
  }

  return vms;
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

const Rack *validated_moves_get_leave(const ValidatedMoves *vms, int i) {
  return vms->moves[i]->leave;
}

bool validated_moves_get_unknown_exchange(const ValidatedMoves *vms, int i) {
  return vms->moves[i]->unknown_exchange;
}

int validated_moves_get_challenge_points(const ValidatedMoves *vms, int i) {
  return vms->moves[i]->challenge_points;
}

bool validated_moves_get_challenge_turn_loss(const ValidatedMoves *vms, int i) {
  return vms->moves[i]->challenge_turn_loss;
}

// Returns success if all moves are valid or
// the first occurrence of a nonsuccess status if not.
move_validation_status_t
validated_moves_get_validation_status(const ValidatedMoves *vms) {
  return vms->final_status;
}

// Adds moves in vms to ml that do not already exist in ml
void validated_moves_add_to_move_list(const ValidatedMoves *vms, MoveList *ml) {
  Move **moves = malloc_or_die(sizeof(Move *) * vms->number_of_moves);
  int number_of_new_moves = 0;
  for (int i = 0; i < vms->number_of_moves; i++) {
    if (!move_list_move_exists(ml, vms->moves[i]->move)) {
      moves[number_of_new_moves++] = vms->moves[i]->move;
    }
  }

  int current_capacity = move_list_get_capacity(ml);
  int current_number_of_moves = move_list_get_count(ml);
  int new_capacity = current_number_of_moves + number_of_new_moves;
  if (new_capacity > current_capacity) {
    move_list_resize(ml, new_capacity);
  }

  for (int i = 0; i < number_of_new_moves; i++) {
    Move *spare_move = move_list_get_spare_move(ml);
    move_copy(spare_move, moves[i]);
    move_list_insert_spare_move(ml, move_get_equity(spare_move));
  }

  free(moves);
}