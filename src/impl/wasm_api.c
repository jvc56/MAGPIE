#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/move_defs.h"

#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/validated_move.h"

#include "exec.h"
#include "move_gen.h"

#include "../str/game_string.h"
#include "../str/move_string.h"

#include "../util/string_util.h"

static Config *iso_config = NULL;
static ErrorStack *iso_error_stack = NULL;
static Config *wasm_config = NULL;
static ErrorStack *wasm_error_stack = NULL;

void wasm_load_command(const char *cmd) {
  config_load_command(iso_config, cmd, iso_error_stack);
  if (!error_stack_is_empty(iso_error_stack)) {
    error_stack_print_and_reset(iso_error_stack);
    return;
  }
  config_load_command(wasm_config, cmd, wasm_error_stack);
  if (!error_stack_is_empty(wasm_error_stack)) {
    error_stack_print_and_reset(wasm_error_stack);
    return;
  }
}

void wasm_init_configs(const char *paths) {
  iso_error_stack = error_stack_create();
  iso_config = config_create_default(iso_error_stack);
  if (!error_stack_is_empty(iso_error_stack)) {
    error_stack_print_and_reset(iso_error_stack);
    return;
  }
  wasm_error_stack = error_stack_create();
  wasm_config = config_create_default(wasm_error_stack);
  if (!error_stack_is_empty(wasm_error_stack)) {
    error_stack_print_and_reset(wasm_error_stack);
    return;
  }
  char *cmd = get_formatted_string("set -path %s", paths);
  wasm_load_command(cmd);
  free(cmd);
}

void wasm_destroy_configs(void) {
  config_destroy(wasm_config);
  error_stack_destroy(wasm_error_stack);
  config_destroy(iso_config);
  error_stack_destroy(iso_error_stack);
}

void load_cgp_into_iso_config(const char *cgp, int num_plays) {
  // Use a separate config and error stack to get
  // a game for static_eval_get_move_score and static_evaluation
  char *cgp_command =
      get_formatted_string("cgp %s -numplays %d", cgp, num_plays);
  execute_command_sync(iso_config, iso_error_stack, cgp_command);
  free(cgp_command);
}

// tiles must contain 0 for play-through tiles!
char *wasm_score_move(const char *cgpstr, const char *ucgi_move_str) {
  load_cgp_into_iso_config(cgpstr, 1);

  if (!error_stack_is_empty(iso_error_stack)) {
    return error_stack_get_string_and_reset(iso_error_stack);
  }

  Game *game = config_get_game(iso_config);
  Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int player_on_turn_index = game_get_player_on_turn_index(game);

  ValidatedMoves *vms =
      validated_moves_create(game, player_on_turn_index, ucgi_move_str, true,
                             false, false, iso_error_stack);

  if (validated_moves_get_number_of_moves(vms) > 1) {
    validated_moves_destroy(vms);
    return string_duplicate("wasm api can only process a single move\n");
  }

  // Return a simple string
  // result <scored|error> valid <true|false> invalid_words FU,BARZ
  // eq 123.45 sc 100 currmove f3.FU etc

  if (!error_stack_is_empty(iso_error_stack)) {
    char *error_string = error_stack_get_string_and_reset(iso_error_stack);
    validated_moves_destroy(vms);
    return error_string;
  }

  const Move *move = validated_moves_get_move(vms, 0);
  char *phonies_string = validated_moves_get_phonies_string(ld, vms, 0);

  StringBuilder *return_string_builder = string_builder_create();
  StringBuilder *move_string_builder = string_builder_create();

  string_builder_add_ucgi_move(move_string_builder, move, board, ld);

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
                                      equity_to_int(move_get_score(move)),
                                      equity_to_double(move_get_equity(move)));

  validated_moves_destroy(vms);
  free(phonies_string);
  string_builder_destroy(move_string_builder);
  char *return_string = string_builder_dump(return_string_builder, NULL);
  string_builder_destroy(return_string_builder);
  // Caller can use UTF8ToString on the returned pointer but it MUST FREE
  // this string after it's done with it!
  return return_string;
}

// a synchronous function to return a static eval of a position.
char *static_evaluation(const char *cgpstr, int num_plays) {
  load_cgp_into_iso_config(cgpstr, num_plays);
  Game *game = config_get_game(iso_config);
  MoveList *move_list = config_get_move_list(iso_config);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .max_equity_diff = 0,
  };
  generate_moves(&args);

  // This pointer needs to be freed by the caller:
  char *val = ucgi_static_moves(game, move_list);
  return val;
}

int process_command_wasm(const char *cmd) {
  execute_command_async(wasm_config, wasm_error_stack, cmd);
  return 0;
}

char *get_search_status_wasm(void) {
  return command_search_status(wasm_config, false);
}

char *get_stop_search_wasm(void) {
  return command_search_status(wasm_config, true);
}