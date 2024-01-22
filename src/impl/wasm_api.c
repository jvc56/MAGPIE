#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/game_history_defs.h"
#include "../def/move_defs.h"

#include "../ent/board.h"
#include "../ent/exec_state.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/words.h"

#include "exec.h"
#include "move_gen.h"

#include "../str/game_string.h"
#include "../str/letter_distribution_string.h"
#include "../str/move_string.h"

#include "../util/string_util.h"

static ExecState *wasm_exec_state = NULL;
static ExecState *iso_exec_state = NULL;

void destroy_wasm_exec_states() {
  exec_state_destroy(wasm_exec_state);
  exec_state_destroy(iso_exec_state);
}

void load_cgp_into_iso_exec_state(const char *cgp, int num_plays) {
  // Use a separate command vars to get
  // a game for score_play and static_evaluation
  if (!iso_exec_state) {
    iso_exec_state = exec_state_create();
  }
  char *cgp_command =
      get_formatted_string("position cgp %s numplays %d", cgp, num_plays);
  execute_command_sync(iso_exec_state, cgp_command);
  free(cgp_command);
}

// tiles must contain 0 for play-through tiles!
char *score_play(const char *cgpstr, int move_type, int row, int col, int dir,
                 uint8_t *tiles, uint8_t *leave, int ntiles, int nleave) {
  load_cgp_into_iso_exec_state(cgpstr, 1);
  Game *game = exec_state_get_game(iso_exec_state);
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Assume players are using the same kwg and klv
  const KWG *kwg = player_get_kwg(game_get_player(game, 0));
  const KLV *klv = player_get_klv(game_get_player(game, 0));

  int tiles_played = 0;
  for (int i = 0; i < ntiles; i++) {
    if (tiles[i] != 0) {
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

  bool phonies_exist = false;
  StringBuilder *phonies_string_builder = create_string_builder();
  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    int number_of_words = formed_words_get_num_words(fw);
    for (int i = 0; i < number_of_words; i++) {
      if (!formed_words_get_word_valid(fw, i)) {
        phonies_exist = true;
        for (int mli = 0; mli < formed_words_get_word_length(fw, i); mli++) {
          string_builder_add_user_visible_letter(
              ld, phonies_string_builder,
              formed_words_get_word_letter(fw, i, mli));
        }
        if (i < number_of_words - 1) {
          string_builder_add_string(phonies_string_builder, ",");
        }
      }
    }
    formed_words_destroy(fw);
  }

  // Return a simple string
  // result <scored|error> valid <true|false> invalid_words FU,BARZ
  // eq 123.45 sc 100 currmove f3.FU etc

  StringBuilder *return_string_builder = create_string_builder();
  StringBuilder *move_string_builder = create_string_builder();

  Move *move = move_create();
  move_set_all(move, tiles, 0, ntiles - 1, points, row, col, tiles_played, dir,
               move_type);

  string_builder_add_ucgi_move(move, board, ld, move_string_builder);
  move_destroy(move);

  string_builder_add_formatted_string(return_string_builder, "currmove %s",
                                      string_builder_peek(move_string_builder));
  string_builder_add_formatted_string(return_string_builder,
                                      " result %s valid %s", "scored",
                                      phonies_exist ? "false" : "true");
  if (phonies_exist) {
    string_builder_add_formatted_string(
        return_string_builder, " invalid_words %s",
        string_builder_peek(phonies_string_builder));
  }
  string_builder_add_formatted_string(return_string_builder, " sc %d eq %.3f",
                                      points, (double)points + leave_value);

  destroy_string_builder(phonies_string_builder);
  destroy_string_builder(move_string_builder);
  // keep config around for next call.
  // config_destroy(config);
  rack_destroy(leave_rack);
  char *return_string = string_builder_dump(return_string_builder, NULL);
  destroy_string_builder(return_string_builder);
  // Caller can use UTF8ToString on the returned pointer but it MUST FREE
  // this string after it's done with it!
  return return_string;
}

// a synchronous function to return a static eval of a position.
char *static_evaluation(const char *cgpstr, int num_plays) {
  load_cgp_into_iso_exec_state(cgpstr, num_plays);
  Game *game = exec_state_get_game(iso_exec_state);
  MoveList *move_list = NULL;
  generate_moves(game, MOVE_RECORD_ALL, MOVE_SORT_EQUITY, 0, move_list);
  move_list_sort_moves(move_list);

  // This pointer needs to be freed by the caller:
  char *val = ucgi_static_moves(game, move_list);
  return val;
}

int process_command_wasm(const char *cmd) {
  if (!wasm_exec_state) {
    wasm_exec_state = exec_state_create();
  }
  execute_command_async(wasm_exec_state, cmd);
  return 0;
}

char *get_search_status_wasm() {
  return command_search_status(wasm_exec_state, false);
}

char *get_stop_search_wasm() {
  return command_search_status(wasm_exec_state, true);
}