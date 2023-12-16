#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/rack_defs.h"

#include "../str/game_string.h"
#include "../str/letter_distribution_string.h"
#include "../str/move_string.h"
#include "../util/string_util.h"

#include "../ent/bag.h"
#include "../ent/config.h"
#include "../ent/error_status.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/words.h"

#include "command.h"
#include "exec.h"
#include "gameplay.h"
#include "move_gen.h"

static CommandVars *wasm_command_vars = NULL;
static CommandVars *iso_command_vars = NULL;

void load_cgp_into_iso_command_vars(const char *cgp, int num_plays) {
  // Use a separate command vars to get
  // a game for score_play and static_evaluation
  if (!iso_command_vars) {
    iso_command_vars = create_command_vars();
  }
  char *cgp_command =
      get_formatted_string("position cgp %s numplays %d", cgp, num_plays);
  execute_command_sync(iso_command_vars, cgp_command);
  free(cgp_command);
}

// tiles must contain 0 for play-through tiles!
char *score_play(const char *cgpstr, int move_type, int row, int col, int dir,
                 uint8_t *tiles, uint8_t *leave, int ntiles, int nleave) {
  load_cgp_into_iso_command_vars(cgpstr, 1);
  Game *game = command_vars_get_game(iso_command_vars);
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

  // score_move assumes the play is always horizontal.
  if (dir_is_vertical(dir)) {
    transpose(board);
    int ph = row;
    row = col;
    col = ph;
  }
  int points = 0;
  double leave_value = 0.0;
  FormedWords *fw = NULL;
  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    // Assume that that kwg is shared
    points = score_move(board, ld, tiles, 0, ntiles - 1, row, col, tiles_played,
                        !dir, 0);

    if (dir_is_vertical(dir)) {
      // transpose back.
      transpose(board);
      int ph = row;
      row = col;
      col = ph;
    }

    fw = words_played(board, tiles, 0, ntiles - 1, row, col, dir);
    // Assume that that kwg is shared
    populate_word_validities(kwg, fw);
  }

  Rack *leave_rack = NULL;

  if (nleave > 0) {
    leave_rack = create_rack(letter_distribution_get_size(ld));
    for (int i = 0; i < nleave; i++) {
      add_letter_to_rack(leave_rack, leave[i]);
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
    free(fw);
  }

  // Return a simple string
  // result <scored|error> valid <true|false> invalid_words FU,BARZ
  // eq 123.45 sc 100 currmove f3.FU etc

  StringBuilder *return_string_builder = create_string_builder();
  StringBuilder *move_string_builder = create_string_builder();

  Move *move = create_move();
  set_move(move, tiles, 0, ntiles - 1, points, row, col, tiles_played, dir,
           move_type);

  string_builder_add_ucgi_move(move, board, ld, move_string_builder);
  destroy_move(move);

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
  // destroy_config(config);
  if (leave_rack) {
    destroy_rack(leave_rack);
  }
  char *return_string = string_builder_dump(return_string_builder, NULL);
  destroy_string_builder(return_string_builder);
  // Caller can use UTF8ToString on the returned pointer but it MUST FREE
  // this string after it's done with it!
  return return_string;
}

// a synchronous function to return a static eval of a position.
char *static_evaluation(const char *cgpstr, int num_plays) {
  load_cgp_into_iso_command_vars(cgpstr, num_plays);
  Game *game = command_vars_get_game(iso_command_vars);
  MoveGen *gen = command_vars_get_gen(iso_command_vars);
  generate_moves_for_game(game, gen, MOVE_RECORD_ALL, MOVE_SORT_EQUITY);
  sort_moves(gen_get_move_list(gen));

  // This pointer needs to be freed by the caller:
  char *val = ucgi_static_moves(game, gen_get_move_list(gen));
  return val;
}

// FIXME: what exactly allocates the char* here?
// I'm not sure about this part of WASM, it might
// need to be freed
int process_command_wasm(const char *cmd) {
  if (!wasm_command_vars) {
    wasm_command_vars = create_command_vars();
  }
  execute_command_async(wasm_command_vars, cmd);
  return 0;
}

char *get_search_status_wasm() {
  return command_search_status(wasm_command_vars, false);
}

char *get_stop_search_wasm() {
  return command_search_status(wasm_command_vars, true);
}