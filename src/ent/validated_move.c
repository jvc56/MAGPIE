#include "validated_move.h"

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/validated_move_defs.h"

#include "game.h"
#include "move.h"
#include "words.h"

// FIXME: remove this
#include "../str/move_string.h"

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

// FIXME: use is_digit
bool is_number(const char c) { return c >= '0' && c <= '9'; }

// FIXME: use some string util function
bool is_letter(const char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int get_letter_coords(const char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  return c - 'a';
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
    if (is_number(position_char)) {
      if (i == 0) {
        move_set_dir(move, BOARD_HORIZONTAL_DIRECTION);
        started_row_parse = true;
      } else if (is_letter(coords_string[i - 1])) {
        if (started_row_parse) {
          return MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION;
        }
        started_row_parse = true;
      }
      // Build the 1-indexed row_start
      row_start = row_start * 10 + (position_char - '0');
    } else if (is_letter(position_char)) {
      if (i == 0) {
        move_set_dir(move, BOARD_VERTICAL_DIRECTION);
        started_col_parse = true;
      } else if (is_number(coords_string[i - 1])) {
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
    int number_of_machine_letters, Move *move, Rack *tiles_played_rack) {

  move_validation_status_t status = MOVE_VALIDATION_STATUS_SUCCESS;

  // Set all the tiles and later overwrite them with
  // playthrough markers if it was a tile placement move
  for (int i = 0; i < number_of_machine_letters; i++) {
    uint8_t ml = machine_letters[i];
    move_set_tile(move, ml, i);
    if (get_is_blanked(ml)) {
      rack_add_letter(tiles_played_rack, BLANK_MACHINE_LETTER);
    } else {
      rack_add_letter(tiles_played_rack, ml);
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
      if (!board_is_position_valid(current_row, current_col)) {
        return MOVE_VALIDATION_STATUS_TILES_PLAYED_OUT_OF_BOUNDS;
      }
      uint8_t board_letter = board_get_letter(board, current_row, current_col);
      uint8_t ml = machine_letters[i];
      if (board_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        move_set_tile(move, ml, i);
        tiles_played++;
        if (!connected && play_connects(board, current_row, current_col)) {
          connected = true;
        }
      } else if (board_letter == move_get_tile(move, i)) {
        move_set_tile(move, PLAYED_THROUGH_MARKER, i);
        if (get_is_blanked(ml)) {
          rack_take_letter(tiles_played_rack, BLANK_MACHINE_LETTER);
        } else {
          rack_take_letter(tiles_played_rack, ml);
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
                                               Rack *tiles_played_rack) {
  int machine_letters_size = string_length(tiles_played) + 1;
  uint8_t *machine_letters =
      malloc_or_die(sizeof(uint8_t) * machine_letters_size);
  int number_of_machine_letters = ld_str_to_mls(
      ld, tiles_played, false, machine_letters, machine_letters_size);

  move_validation_status_t status = MOVE_VALIDATION_STATUS_SUCCESS;

  if (number_of_machine_letters < 1) {
    status = MOVE_VALIDATION_STATUS_INVALID_TILES_PLAYED;
  } else if (number_of_machine_letters > BOARD_DIM) {
    status = MOVE_VALIDATION_STATUS_TILES_PLAYED_OUT_OF_BOUNDS;
  } else {
    status = validate_tiles_played_with_mls(board, machine_letters,
                                            number_of_machine_letters, move,
                                            tiles_played_rack);
  }

  free(machine_letters);

  return status;
}

move_validation_status_t validate_split_move(const StringSplitter *split_move,
                                             Game *game, ValidatedMove *vm,
                                             int player_index,
                                             Rack *tiles_played_rack) {
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
  move_validation_status_t status = MOVE_VALIDATION_STATUS_SUCCESS;
  int number_of_fields = string_splitter_get_number_of_items(split_move);

  if (number_of_fields > 5) {
    return MOVE_VALIDATION_STATUS_EXCESS_FIELDS;
  }

  // Validate move position
  const char *move_type_or_coords = string_splitter_get_item(split_move, 0);

  if (is_all_whitespace_or_empty(move_type_or_coords)) {
    return MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION;
  }

  if (strings_equal(move_type_or_coords, UCGI_PASS_MOVE)) {
    // This will also set the equity
    move_set_as_pass(vm->move);
  } else if (strings_equal(move_type_or_coords, UCGI_EXCHANGE_MOVE)) {
    move_set_type(vm->move, GAME_EVENT_EXCHANGE);
    move_set_score(vm->move, 0);
  } else {
    move_set_type(vm->move, GAME_EVENT_TILE_PLACEMENT_MOVE);
    status = validate_coordinates(vm->move, move_type_or_coords);
  }

  if (status != MOVE_VALIDATION_STATUS_SUCCESS) {
    return status;
  }

  if (number_of_fields == 1) {
    if (move_get_type(vm->move) != GAME_EVENT_PASS) {
      return MOVE_VALIDATION_STATUS_MISSING_FIELDS;
    } else {
      return MOVE_VALIDATION_STATUS_SUCCESS;
    }
  } else if (move_get_type(vm->move) == GAME_EVENT_PASS) {
    return MOVE_VALIDATION_STATUS_EXCESS_PASS_FIELDS;
  }

  // Validate tiles played or number exchanged
  const char *played_tiles_or_number_exchanged =
      string_splitter_get_item(split_move, 1);

  if (is_all_whitespace_or_empty(played_tiles_or_number_exchanged)) {
    return MOVE_VALIDATION_STATUS_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED;
  }

  // FIXME: The rack should be partially set with the played tiles.
  if (is_all_digits_or_empty(played_tiles_or_number_exchanged)) {
    if (move_get_type(vm->move) != GAME_EVENT_EXCHANGE) {
      return MOVE_VALIDATION_STATUS_NONEXCHANGE_NUMERIC_TILES;
    }
    int number_exchanged = string_to_int(played_tiles_or_number_exchanged);
    if (number_exchanged < 1 || number_exchanged > (RACK_SIZE)) {
      return MOVE_VALIDATION_STATUS_INVALID_NUMBER_EXCHANGED;
    }
    move_set_tiles_played(vm->move, number_exchanged);
    move_set_tiles_length(vm->move, number_exchanged);
    vm->unknown_exchange = true;
  } else {
    status = validate_tiles_played(ld, board, vm->move,
                                   played_tiles_or_number_exchanged,
                                   tiles_played_rack);
  }

  if (status != MOVE_VALIDATION_STATUS_SUCCESS || number_of_fields == 2) {
    return status;
  }

  // Validate rack
  const char *rack_string = string_splitter_get_item(split_move, 2);

  if (is_all_whitespace_or_empty(rack_string)) {
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
  Bag *bag = game_get_bag(game);
  for (int i = 0; i < dist_size; i++) {
    if (rack_get_letter(vm->rack, i) > bag_get_letter(bag, i)) {
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

  if (status != MOVE_VALIDATION_STATUS_SUCCESS || number_of_fields == 3) {
    return status;
  }

  if (move_get_type(vm->move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return MOVE_VALIDATION_STATUS_EXCESS_EXCHANGE_FIELDS;
  }

  // Validate challenge points
  const char *challenge_points = string_splitter_get_item(split_move, 3);

  if (is_all_whitespace_or_empty(challenge_points)) {
    return MOVE_VALIDATION_STATUS_EMPTY_CHALLENGE_POINTS;
  }

  if (!is_all_digits_or_empty(challenge_points)) {
    return MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_POINTS;
  }

  vm->challenge_points = string_to_int(challenge_points);

  // Validate challenge turn loss

  const char *challenge_turn_loss = string_splitter_get_item(split_move, 4);

  if (is_all_whitespace_or_empty(challenge_turn_loss)) {
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

move_validation_status_t validate_move(ValidatedMove *vm, Game *game,
                                       int player_index,
                                       const char *ucgi_move_string) {
  StringSplitter *split_move = split_string(ucgi_move_string, '.', false);
  Rack *tiles_played_rack = rack_create(ld_get_size(game_get_ld(game)));

  move_validation_status_t status = validate_split_move(
      split_move, game, vm, player_index, tiles_played_rack);

  string_splitter_destroy(split_move);
  rack_destroy(tiles_played_rack);

  return status;
}

int score_move(const LetterDistribution *ld, const Move *move, Board *board,
               int cross_set_index) {
  int ls;
  int main_word_score = 0;
  int cross_scores = 0;
  int bingo_bonus = 0;

  int tiles_played = move_get_tiles_played(move);
  int tiles_length = move_get_tiles_length(move);
  int row_start = move_get_row_start(move);
  int col_start = move_get_col_start(move);
  int move_dir = move_get_dir(move);
  int cross_dir = 1 - move_dir;

  bool board_was_transposed = false;
  if (!board_matches_dir(board, move_dir)) {
    board_transpose(board);
    board_was_transposed = true;
  }

  if (move_dir == BOARD_VERTICAL_DIRECTION) {
    int tmp_start = row_start;
    row_start = col_start;
    col_start = tmp_start;
  }

  if (tiles_played == RACK_SIZE) {
    bingo_bonus = DEFAULT_BINGO_BONUS;
  }
  int word_multiplier = 1;
  for (int idx = 0; idx < tiles_length; idx++) {
    uint8_t ml = move_get_tile(move, idx);
    uint8_t bonus_square =
        board_get_bonus_square(board, row_start, col_start + idx);
    int letter_multiplier = 1;
    int this_word_multiplier = 1;
    bool fresh_tile = false;
    if (ml == PLAYED_THROUGH_MARKER) {
      ml = board_get_letter(board, row_start, col_start + idx);
    } else {
      fresh_tile = true;
      this_word_multiplier = bonus_square >> 4;
      letter_multiplier = bonus_square & 0x0F;
      word_multiplier *= this_word_multiplier;
    }
    int cs = board_get_cross_score(board, row_start, col_start + idx, cross_dir,
                                   cross_set_index);
    if (get_is_blanked(ml)) {
      ls = 0;
    } else {
      ls = ld_get_score(ld, ml);
    }

    main_word_score += ls * letter_multiplier;
    bool actual_cross_word =
        (row_start > 0 &&
         !board_is_empty(board, row_start - 1, col_start + idx)) ||
        ((row_start < BOARD_DIM - 1) &&
         !board_is_empty(board, row_start + 1, col_start + idx));
    if (fresh_tile && actual_cross_word) {
      cross_scores += ls * letter_multiplier * this_word_multiplier +
                      cs * this_word_multiplier;
    }
  }

  if (board_was_transposed) {
    board_transpose(board);
  }

  return main_word_score * word_multiplier + cross_scores + bingo_bonus;
}

move_validation_status_t validated_move_load(ValidatedMove *vm, Game *game,
                                             int player_index,
                                             const char *ucgi_move_string,
                                             bool allow_phonies) {
  if (is_all_whitespace_or_empty(ucgi_move_string)) {
    return MOVE_VALIDATION_STATUS_EMPTY_MOVE;
  }
  if (player_index != 0 && player_index != 1) {
    return MOVE_VALIDATION_STATUS_INVALID_PLAYER_INDEX;
  }

  move_validation_status_t status =
      validate_move(vm, game, player_index, ucgi_move_string);

  if (status != MOVE_VALIDATION_STATUS_SUCCESS) {
    return status;
  }

  if (move_get_type(vm->move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    move_set_score(
        vm->move,
        score_move(game_get_ld(game), vm->move, game_get_board(game),
                   board_get_cross_set_index(
                       game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG),
                       player_index)));

    // FIXME: set the equity here somewhere

    vm->formed_words = formed_words_create(game_get_board(game), vm->move);

    formed_words_populate_validities(
        player_get_kwg(game_get_player(game, player_index)), vm->formed_words);

    if (!allow_phonies) {
      for (int i = 0; i < formed_words_get_num_words(vm->formed_words); i++) {
        if (!formed_words_get_word_valid(vm->formed_words, i)) {
          return MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED;
        }
      }
    }
  }

  return MOVE_VALIDATION_STATUS_SUCCESS;
}

ValidatedMove *validated_move_create(Game *game, int player_index,
                                     const char *ucgi_move_string,
                                     bool allow_phonies) {
  ValidatedMove *vm = malloc_or_die(sizeof(ValidatedMove));
  vm->formed_words = NULL;
  vm->rack = NULL;
  vm->leave = NULL;
  vm->challenge_points = 0;
  vm->challenge_turn_loss = false;
  vm->leave_value = 0;
  vm->unknown_exchange = false;
  vm->move = move_create();
  vm->status = validated_move_load(vm, game, player_index, ucgi_move_string,
                                   allow_phonies);
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

ValidatedMoves *validated_moves_create(Game *game, int player_index,
                                       const char *ucgi_moves_string,
                                       bool allow_phonies) {
  ValidatedMoves *vms = malloc_or_die(sizeof(ValidatedMoves));
  vms->moves = NULL;
  vms->number_of_moves = 0;
  vms->final_status = MOVE_VALIDATION_STATUS_SUCCESS;

  // FIXME: maybe make empty, null, or whitespace check in string util
  if (is_string_empty_or_null(ucgi_moves_string) ||
      is_all_whitespace_or_empty(ucgi_moves_string)) {
    vms->final_status = MOVE_VALIDATION_STATUS_EMPTY_MOVE;
  } else {
    StringSplitter *split_moves = split_string(ucgi_moves_string, ',', true);
    vms->number_of_moves = string_splitter_get_number_of_items(split_moves);

    vms->moves = malloc_or_die(sizeof(ValidatedMove *) * vms->number_of_moves);

    for (int i = 0; i < vms->number_of_moves; i++) {
      vms->moves[i] = validated_move_create(
          game, player_index, string_splitter_get_item(split_moves, i),
          allow_phonies);

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

ValidatedMoves *validated_moves_create_empty() {
  ValidatedMoves *vms = malloc_or_die(sizeof(ValidatedMoves));
  vms->moves = NULL;
  vms->number_of_moves = 0;
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

int validated_moves_get_number_of_moves(ValidatedMoves *vms) {
  return vms->number_of_moves;
}

// FIXME: should be const
Move *validated_moves_get_move(ValidatedMoves *vms, int i) {
  return vms->moves[i]->move;
}

// FIXME: should be const
FormedWords *validated_moves_get_formed_words(ValidatedMoves *vms, int i) {
  return vms->moves[i]->formed_words;
}

// FIXME: should be const
Rack *validated_moves_get_rack(ValidatedMoves *vms, int i) {
  return vms->moves[i]->rack;
}

// FIXME: should be const
Rack *validated_moves_get_leave(ValidatedMoves *vms, int i) {
  return vms->moves[i]->leave;
}

bool validated_moves_get_unknown_exchange(ValidatedMoves *vms, int i) {
  return vms->moves[i]->unknown_exchange;
}

int validated_moves_get_challenge_points(ValidatedMoves *vms, int i) {
  return vms->moves[i]->challenge_points;
}

bool validated_moves_get_challenge_turn_loss(ValidatedMoves *vms, int i) {
  return vms->moves[i]->challenge_turn_loss;
}

// Returns success if all moves are valid or
// the first occurrence of a nonsuccess status if not.
move_validation_status_t
validated_moves_get_validation_status(ValidatedMoves *vms) {
  return vms->final_status;
}

// This function takes ownership of vms2.
void validated_moves_combine(ValidatedMoves *vms1, ValidatedMoves *vms2) {
  if (vms2->number_of_moves == 0) {
    return;
  }
  if (vms1->number_of_moves == 0) {
    vms1->moves = vms2->moves;
    vms1->number_of_moves = vms2->number_of_moves;
    free(vms2);
    return;
  }

  // Moves already exist, so we need to resize
  // the existing ValidatedMove array in vms1 to accommodate
  // the new moves.
  realloc_or_die(vms1->moves, vms1->number_of_moves + vms2->number_of_moves);
  for (int i = 0; i < vms2->number_of_moves; i++) {
    vms1->moves[i + vms1->number_of_moves] = vms2->moves[i];
  }
  vms1->number_of_moves += vms2->number_of_moves;

  // Free the container structure of validated moves that were added
  // but do not free the underlying ValidatedMove structs, as they
  // are now owned by vms1.
  free(vms2->moves);
  free(vms2);
}