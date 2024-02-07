#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/game_history_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"

#include "../ent/board.h"
#include "../ent/exec_state.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/static_eval.h"
#include "../ent/validated_move.h"
#include "../ent/words.h"

#include "exec.h"
#include "move_gen.h"

#include "../str/game_string.h"
#include "../str/letter_distribution_string.h"
#include "../str/move_string.h"

#include "../util/string_util.h"

static ExecState *wasm_exec_state = NULL;
static ExecState *iso_exec_state = NULL;

void wasm_destroy_exec_states() {
  exec_state_destroy(wasm_exec_state);
  exec_state_destroy(iso_exec_state);
}

void load_cgp_into_iso_exec_state(const char *cgp, int num_plays) {
  // Use a separate command vars to get
  // a game for static_eval_get_move_score and static_evaluation
  if (!iso_exec_state) {
    iso_exec_state = exec_state_create();
  }
  char *cgp_command =
      get_formatted_string("position cgp %s numplays %d", cgp, num_plays);
  execute_command_sync(iso_exec_state, cgp_command);
  free(cgp_command);
}

// tiles must contain 0 for play-through tiles!
char *wasm_score_move(const char *cgpstr, const char *ucgi_move_str) {
  load_cgp_into_iso_exec_state(cgpstr, 1);
  Game *game = exec_state_get_game(iso_exec_state);
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int player_on_turn_index = game_get_player_on_turn_index(game);

  ValidatedMoves *vms = validated_moves_create(game, player_on_turn_index,
                                               ucgi_move_str, true, false);

  if (validated_moves_get_number_of_moves(vms) > 1) {
    validated_moves_destroy(vms);
    return string_duplicate("wasm api can only process a single move\n");
  }

  // Return a simple string
  // result <scored|error> valid <true|false> invalid_words FU,BARZ
  // eq 123.45 sc 100 currmove f3.FU etc

  move_validation_status_t status = validated_moves_get_validation_status(vms);

  if (status != MOVE_VALIDATION_STATUS_SUCCESS) {
    validated_moves_destroy(vms);
    return get_formatted_string(
        "wasm api move validation failed with code %d\n", status);
  }

  const Move *move = validated_moves_get_move(vms, 0);
  char *phonies_string = validated_moves_get_phonies_string(ld, vms, 0);

  StringBuilder *return_string_builder = create_string_builder();
  StringBuilder *move_string_builder = create_string_builder();

  string_builder_add_ucgi_move(move, board, ld, move_string_builder);

  string_builder_add_formatted_string(return_string_builder, "currmove %s",
                                      string_builder_peek(move_string_builder));
  string_builder_add_formatted_string(return_string_builder,
                                      " result %s valid %s", "scored",
                                      phonies_string ? "false" : "true");
  if (phonies_string) {
    string_builder_add_formatted_string(return_string_builder,
                                        " invalid_words %s", phonies_string);
  }

  string_builder_add_formatted_string(return_string_builder, " sc %d eq %.3f",
                                      move_get_score(move),
                                      move_get_equity(move));

  validated_moves_destroy(vms);
  free(phonies_string);
  destroy_string_builder(move_string_builder);
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