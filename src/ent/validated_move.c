#include "validated_move.h"

#include "../def/letter_distribution_defs.h"
#include "../def/validated_move_defs.h"

#include "game.h"
#include "move.h"
#include "words.h"

#include "../util/string_util.h"
#include "../util/util.h"

typedef struct ValidatedMove {
  Move *move;
  FormedWords *formed_words;
  Rack *rack;
  int challenge_points;
  bool challenge_turn_loss;
  move_validation_status_t status;
} ValidatedMove;

struct ValidatedMoves {
  ValidatedMove **moves;
  int number_of_moves;
};

move_validation_status_t validate_split_move(StringSplitter *split_move,
                                             int player_index,
                                             bool allow_phonies) {
  int number_of_fields = string_splitter_get_number_of_items(split_move);
}

move_validation_status_t *
validated_move_load(ValidatedMove *vm, const Game *game, int player_index,
                    const char *ucgi_move_string, bool allow_phonies) {
  if (is_all_whitespace_or_empty(ucgi_move_string)) {
    return MOVE_VALIDATION_STATUS_NULL_INPUT;
  }
  if (player_index != 0 && player_index != 1) {
    return MOVE_VALIDATION_STATUS_INVALID_PLAYER_INDEX;
  }

  StringSplitter *split_move = split_string(ucgi_move_string, '.', false);

  move_validation_status_t status =
      validate_split_move(split_move, player_index, allow_phonies);
  if (move_fields < 2) {
    return MOVE_VALIDATION_STATUS_MISSING_FIELDS;
  }

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

  if (strings_equal(ucgi_move_string, UCGI_PASS_MOVE)) {
    vm->move = move_create();
    move_set_as_pass(vm->move);
    return MOVE_VALIDATION_STATUS_SUCCESS;
  }

  StringSplitter *split_move = split_string(ucgi_move_string, '.', false);
  int move_fields = string_splitter_get_number_of_items(split_move);

  if (move_fields < 2) {
    return MOVE_VALIDATION_STATUS_MISSING_FIELDS;
  }

  game_event_t game_event_type;

  if (strings_equal(string_splitter_get_item(split_move, 0),
                    UCGI_EXCHANGE_MOVE)) {
    game_event_type = GAME_EVENT_EXCHANGE;
  } else {
  }

  for (int i = 0; i < split_move; i++) {
    vms->moves[i] = validated_move_create(
        game, string_splitter_get_item(split_moves, i), allow_phonies);
  }

  // Perform validations first
  if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM || dir < 0 ||
      dir > 1 || ntiles < 0 || nleave < 0) {
    vm->status = MOVE_VALIDATION_STATUS_MALFORMED;
    return;
  }

  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Assume players are using the same kwg and klv
  const KWG *kwg = player_get_kwg(game_get_player(game, player_index));
  const KLV *klv = player_get_klv(game_get_player(game, player_index));

  // board_score_move assumes the play is always horizontal.
  if (board_get_transposed(board)) {
    board_transpose(board);
    int ph = row;
    row = col;
    col = ph;
  }

  int points = 0;
  double leave_value = 0.0;

  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    // Assume that that kwg is shared
    points = board_score_move(board, ld, tiles, 0, ntiles - 1, row, col,
                              tiles_played, !dir, 0);

    if (board_is_dir_vertical(dir)) {
      // board_transpose back.
      board_transpose(board);
      int ph = row;
      row = col;
      col = ph;
    }

    vm->formed_words =
        formed_words_create(board, tiles, 0, ntiles - 1, row, col, dir);
    // Assume that that kwg is shared
    formed_words_populate_validities(kwg, vm->formed_words);
  }

  Rack *leave_rack = NULL;

  if (nleave > 0) {
    leave_rack = rack_create(ld_get_size(ld));
    for (int i = 0; i < nleave; i++) {
      rack_add_letter(leave_rack, leave[i]);
    }
    // Assume that that klv is shared
    leave_value = klv_get_leave_value(klv, leave_rack);
  }

  vm->move = move_create();
  move_set_all(vm->move, tiles, 0, ntiles, points, row, col, ntiles, dir,
               move_type, leave_value);

  return vm;
}

ValidatedMove *validated_move_create(const Game *game, int player_index,
                                     const char *ucgi_move_string,
                                     bool allow_phonies) {
  ValidatedMove *vm = malloc_or_die(sizeof(ValidatedMove));
  vm->move = NULL;
  vm->formed_words = NULL;
  vm->rack = NULL;
  vm->status = validated_move_load(game, ucgi_move_string, allow_phonies);
  return vm;
}

void validated_move_destroy(ValidatedMove *vm) {
  if (!vm) {
    return;
  }
  move_destroy(vm->move);
  formed_words_destroy(vm->formed_words);
  rack_destroy(vm->rack);
  free(vm);
}

ValidatedMoves *validated_moves_create(const Game *game, int player_index,
                                       const char *ucgi_move_string,
                                       bool allow_phonies) {
  ValidatedMoves *vms = malloc_or_die(sizeof(ValidatedMoves));

  StringSplitter *split_moves = split_string(moves, ',', false);
  vms->number_of_moves = string_splitter_get_number_of_items(split_moves);

  vms->moves = malloc_or_die(sizeof(ValidatedMove *) * vms->number_of_moves);

  for (int i = 0; i < vms->number_of_moves; i++) {
    vms->moves[i] = validated_move_create(
        game, string_splitter_get_item(split_moves, i), allow_phonies);
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
  free(vms);
}

int validated_moves_get_moves(ValidatedMoves *vms) {
  return vms->number_of_moves;
}

// Returns success if all moves are valid or
// the first occurrence of a nonsuccess status if not.
move_validation_status_t
validated_moves_get_validation_status(ValidatedMoves *vms) {
  for (int i = 0; i < vms->number_of_moves; i++) {
    if (vms->moves[i]->status != MOVE_VALIDATION_STATUS_SUCCESS) {
      return vms->moves[i]->status;
    }
  }
  return MOVE_VALIDATION_STATUS_SUCCESS;
}

// This function takes ownership of vms2.
bool validated_moves_combine(ValidatedMoves *vms1, ValidatedMoves *vms2) {
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