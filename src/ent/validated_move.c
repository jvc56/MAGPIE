#include "validated_move.h"

#include "../def/letter_distribution_defs.h"
#include "../def/validated_move_defs.h"

#include "game.h"
#include "move.h"
#include "words.h"

#include "../util/string_util.h"
#include "../util/util.h"

struct ValidatedMove {
  Move *move;
  FormedWords *formed_words;
  move_validation_status_t status;
};

ValidatedMove *validated_move_create(const Game *game, int player_index,
                                     int move_type, int row, int col, int dir,
                                     uint8_t *tiles, uint8_t *leave, int ntiles,
                                     int nleave) {
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Assume players are using the same kwg and klv
  const KWG *kwg = player_get_kwg(game_get_player(game, player_index));
  const KLV *klv = player_get_klv(game_get_player(game, player_index));

  int tiles_played = 0;
  for (int i = 0; i < ntiles; i++) {
    if (tiles[i] != ALPHABET_EMPTY_SQUARE_MARKER) {
      tiles_played++;
    }
  }

  // board_score_move assumes the play is always horizontal.
  if (board_is_dir_vertical(dir)) {
    board_transpose(board);
    int ph = row;
    row = col;
    col = ph;
  }
  int points = 0;
  double leave_value = 0.0;
  FormedWords *fw = NULL;
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

    fw = formed_words_create(board, tiles, 0, ntiles - 1, row, col, dir);
    // Assume that that kwg is shared
    formed_words_populate_validities(kwg, fw);
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
}

ValidatedMove *validated_move_create_from_ucgi_string(const Game *game,
                                                      const char *move_string) {

}

void validated_move_destroy(ValidatedMove *vm) {
  if (!vm) {
    return;
  }
  move_destroy(vm->move);
  formed_words_destroy(vm->formed_words);
  free(vm);
}

struct ValidatedMoves {
  ValidatedMove **moves;
  int number_of_moves;
};

ValidatedMoves *validated_moves_create(const Game *game, const char *moves) {
  ValidatedMoves *vms = malloc_or_die(sizeof(ValidatedMoves));

  StringSplitter *split_moves = split_string(moves, ',', false);
  int number_of_moves = string_splitter_get_number_of_items(split_moves);

  vms->moves = malloc_or_die(sizeof(ValidatedMove *) * number_of_moves);
  vms->number_of_moves = number_of_moves;

  for (int i = 0; i < number_of_moves; i++) {
    vms->moves[i] = validated_move_create_from_ucgi_string(
        game, string_splitter_get_item(split_moves, i));
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
  free(vms);
}