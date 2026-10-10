#include "browser_game.h"

#include "../src/def/board_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/exec.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/gcg.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void add_json_string(StringBuilder *builder, const char *text) {
  string_builder_add_char(builder, '"');
  if (text) {
    for (const unsigned char *character = (const unsigned char *)text;
         *character; character++) {
      if (*character == '"' || *character == '\\') {
        string_builder_add_char(builder, '\\');
        string_builder_add_char(builder, (char)*character);
      } else if (*character < 32) {
        string_builder_add_formatted_string(builder, "\\u%04x", *character);
      } else {
        string_builder_add_char(builder, (char)*character);
      }
    }
  }
  string_builder_add_char(builder, '"');
}

static void run_game_command(Config *config, ErrorStack *errors,
                             const char *command, StringBuilder *warnings) {
  if (!error_stack_is_empty(errors)) {
    return;
  }
  char *output = NULL;
  run_str_api_command(config, errors, command, &output);
  if (warnings && output) {
    string_builder_add_string(warnings, output);
  }
  free(output);
}

typedef struct SnapshotWriter {
  StringBuilder *result;
  const Game *live_game;
  int previous_letters[BOARD_DIM][BOARD_DIM];
  int owners[BOARD_DIM][BOARD_DIM];
} SnapshotWriter;

static void write_snapshot(const GameHistory *history, const Game *game,
                           int event_index, void *context) {
  SnapshotWriter *state = context;
  StringBuilder *result = state->result;
  const Game *live_game = state->live_game;
  const int count = game_history_get_num_events(history);
  if (event_index) {
    string_builder_add_char(result, ',');
  }
  string_builder_add_string(result, "{\"cgp\":");
  const Game *snapshot_game =
      live_game && event_index == count ? live_game : game;
  char *cgp = game_get_cgp(snapshot_game, true);
  add_json_string(result, cgp);
  free(cgp);
  string_builder_add_formatted_string(
      result, ",\"onTurn\":%d,\"owners\":[",
      game_get_player_on_turn_index(snapshot_game));
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    if (row) {
      string_builder_add_char(result, ',');
    }
    string_builder_add_char(result, '[');
    for (int col = 0; col < BOARD_DIM; col++) {
      if (col) {
        string_builder_add_char(result, ',');
      }
      const int letter = board_get_letter(board, row, col);
      if (!letter) {
        state->owners[row][col] = BOARD_OWNER_UNKNOWN;
      } else if (letter != state->previous_letters[row][col]) {
        state->owners[row][col] =
            event_index ? game_event_get_player_index(
                              game_history_get_event(history, event_index - 1))
                        : BOARD_OWNER_UNKNOWN;
      }
      state->previous_letters[row][col] = letter;
      string_builder_add_int(result, state->owners[row][col]);
    }
    string_builder_add_char(result, ']');
  }
  string_builder_add_string(result, "],\"note\":");
  add_json_string(result, event_index
                              ? game_event_get_note(game_history_get_event(
                                    history, event_index - 1))
                              : NULL);
  string_builder_add_char(result, '}');
}

static Config *game_config;
static char *game_lexicon;

void wasm_game_cache_destroy(void) {
  config_destroy(game_config);
  game_config = NULL;
  free(game_lexicon);
  game_lexicon = NULL;
}

static bool valid_rack(const char *rack) {
  if (strlen(rack) > 7) {
    return false;
  }
  for (; *rack; rack++) {
    if (!((*rack >= 'A' && *rack <= 'Z') || *rack == '?')) {
      return false;
    }
  }
  return true;
}

static bool valid_move(const char *move) {
  if (strings_equal(move, "pass")) {
    return true;
  }
  if (strncmp(move, "ex ", 3) == 0) {
    return move[3] && valid_rack(move + 3);
  }
  const char *cursor = move;
  if (*cursor >= 'A' && *cursor <= 'O') {
    cursor++;
    if (!isdigit((unsigned char)*cursor)) {
      return false;
    }
    while (isdigit((unsigned char)*cursor)) {
      cursor++;
    }
  } else {
    if (!isdigit((unsigned char)*cursor)) {
      return false;
    }
    while (isdigit((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor < 'A' || *cursor > 'O') {
      return false;
    }
    cursor++;
  }
  if (*cursor++ != ' ' || !*cursor) {
    return false;
  }
  for (; *cursor; cursor++) {
    if (!isalpha((unsigned char)*cursor) && *cursor != '.') {
      return false;
    }
  }
  return true;
}

static char *without_finished_marker(const char *text) {
  StringBuilder *clean = string_builder_create();
  for (const char *line = text; *line;) {
    const char *end = strchr(line, '\n');
    if (!end) {
      end = line + strlen(line);
    }
    if (strncmp(line, "#description Magpie finished:", 28) != 0) {
      for (const char *ch = line; ch < end; ch++) {
        string_builder_add_char(clean, *ch);
      }
      string_builder_add_char(clean, '\n');
    }
    line = *end ? end + 1 : end;
  }
  char *result = string_builder_dump(clean, NULL);
  string_builder_destroy(clean);
  return result;
}

// Actions: 0 import, 1 commit, 2 note, 3 challenge, 4 deal/branch, 5 live move.
// Each edit is transactional: adopt only after validation/confirmation.
char *wasm_game_action(const char *text, const char *lexicon,
                       const int event_index, const char *rack,
                       const char *move, const char *note, const int action,
                       const char *current_cgp, const unsigned int seed,
                       const int current_player, const int allow_phonies) {
  int event_count = 0;
  for (const char *ch = text; *ch; ch++) {
    if (*ch == '>' && (ch == text || ch[-1] == '\n')) {
      event_count++;
    }
  }
  if (strlen(text) > 1024 * 1024 || event_count > 1000 || action < 0 ||
      action > 5 ||
      !(strings_equal(lexicon, "CSW24") || strings_equal(lexicon, "NWL23")) ||
      ((action == 1 || action == 5) &&
       (!valid_rack(rack) || !valid_move(move)))) {
    return string_duplicate("{\"error\":\"Invalid game action or import "
                            "exceeds its size limit.\"}");
  }
  ErrorStack *errors = error_stack_create();
  const ConfigArgs args = {
      .data_paths = "data", .settings_filename = NULL, .use_wmp = false};
  if (game_config && !strings_equal(game_lexicon, lexicon)) {
    wasm_game_cache_destroy();
  }
  const bool first_use = game_config == NULL;
  if (first_use) {
    game_config = config_create(&args, errors);
    game_lexicon = string_duplicate(lexicon);
  }
  Config *config = game_config;
  StringBuilder *result = string_builder_create();
  StringBuilder *warnings = string_builder_create();
  Game *live_game = NULL;
  bool live_phony = false;
  if (error_stack_is_empty(errors)) {
    char *command = get_formatted_string("set -lex %s", lexicon);
    char *output = NULL;
    run_str_api_command(config, errors, command, &output);
    free(output);
    free(command);
  }
  if (error_stack_is_empty(errors)) {
    char *clean = (action == 1 || action >= 3) ? without_finished_marker(text)
                                               : string_duplicate(text);
    config_parse_gcg_string(config, clean, config_get_game_history(config),
                            errors);
    free(clean);
  }
  if (action && error_stack_is_empty(errors)) {
    // Parsing leaves the board at the end but the history cursor at zero.
    // Navigate via the end so Config initializes its commit/challenge state.
    run_game_command(config, errors, "goto end", NULL);
    char *command = get_formatted_string("goto %d", event_index);
    run_game_command(config, errors, command, NULL);
    free(command);
    if ((action == 4 || action == 5) && error_stack_is_empty(errors)) {
      Game *game = config_get_game(config);
      const int on_turn = current_player == 1 ? 1 : 0;
      if (*current_cgp) {
        game_load_cgp(game, current_cgp, errors);
      }
      game_set_player_on_turn_index(game, on_turn);
      game_seed(game, seed);
      if (action == 4 && error_stack_is_empty(errors)) {
        game_history_truncate_to_played_events(config_get_game_history(config));
        draw_to_full_rack(game, on_turn);
        draw_to_full_rack(game, 1 - on_turn);
        live_game = game_duplicate(game);
      } else if (error_stack_is_empty(errors)) {
        ValidatedMoves *moves = validated_moves_create(
            game, on_turn, move, allow_phonies != 0, true, errors);
        if (error_stack_is_empty(errors)) {
          live_game = game_duplicate(game);
          live_phony = validated_moves_is_phony(moves, 0);
          if (live_phony) {
            Move pass;
            move_set_as_pass(&pass);
            play_move(&pass, live_game, NULL);
          } else {
            play_move(validated_moves_get_move(moves, 0), live_game, NULL);
          }
        }
        validated_moves_destroy(moves);
      }
    }
    if (action == 1 || action == 5) {
      command = get_formatted_string("rack %s", *rack ? rack : "-");
      run_game_command(config, errors, command, NULL);
      free(command);
      command = get_formatted_string("commit %s", move);
      run_game_command(config, errors, command, warnings);
      free(command);
      if (action == 5 && live_phony) {
        run_game_command(config, errors, "challenge", NULL);
      }
      if (action == 5 && !live_phony && error_stack_is_empty(errors) &&
          game_history_get_waiting_for_final_pass_or_challenge(
              config_get_game_history(config))) {
        run_game_command(config, errors, "commit pass", NULL);
      }
    } else if (action == 3) {
      run_game_command(config, errors, "challenge", warnings);
    }
    if ((action == 1 || action == 2) && error_stack_is_empty(errors) &&
        (action == 2 || *note)) {
      if (game_history_get_num_played_events(config_get_game_history(config)) >
          0) {
        game_history_set_note_for_most_recent_event(
            config_get_game_history(config), note);
      } else {
        error_stack_push(
            errors, ERROR_STATUS_NOTE_NO_GAME_EVENTS,
            string_duplicate("Select an event before adding a note."));
      }
    }
  }
  if (error_stack_is_empty(errors)) {
    GameHistory *history = config_get_game_history(config);
    Game *game = config_get_game(config);
    StringBuilder *gcg = string_builder_create();
    if (live_game) {
      game_history_player_reset_last_rack(history, 0);
      game_history_player_reset_last_rack(history, 1);
    }
    string_builder_add_gcg(gcg, config_get_ld(config), history, false);
    if (live_game &&
        game_get_game_end_reason(live_game) == GAME_END_REASON_NONE) {
      {
        const int player_index = game_get_player_on_turn_index(live_game);
        string_builder_add_formatted_string(gcg, "\n#rack%d ",
                                            player_index + 1);
        string_builder_add_rack(
            gcg, player_get_rack(game_get_player(live_game, player_index)),
            config_get_ld(config), true);
      }
      string_builder_add_char(gcg, '\n');
    }
    string_builder_add_formatted_string(
        result, "{\"index\":%d,\"warning\":",
        game_history_get_num_played_events(history));
    add_json_string(result, string_builder_peek(warnings));
    const int score = (action == 1 || action == 5) &&
                              event_index < game_history_get_num_events(history)
                          ? equity_to_int(game_event_get_move_score(
                                game_history_get_event(history, event_index)))
                          : 0;
    string_builder_add_formatted_string(result,
                                        ",\"score\":%d,\"gcg\":", score);
    add_json_string(result, string_builder_peek(gcg));
    string_builder_destroy(gcg);
    string_builder_add_string(result, ",\"players\":[");
    add_json_string(result, game_history_player_get_name(history, 0));
    string_builder_add_char(result, ',');
    add_json_string(result, game_history_player_get_name(history, 1));
    string_builder_add_formatted_string(
        result, "],\"ended\":%s,\"positions\":[",
        game_get_game_end_reason(live_game ? live_game : game) !=
                GAME_END_REASON_NONE
            ? "true"
            : "false");
    SnapshotWriter state = {.result = result, .live_game = live_game};
    game_replay_history(history, game, write_snapshot, &state, errors);
    string_builder_add_string(result, "]}");
  }
  const bool failed = !error_stack_is_empty(errors);
  if (failed) {
    string_builder_clear(result);
    string_builder_add_string(result, "{\"error\":");
    char *error = error_stack_get_string_and_reset(errors);
    add_json_string(result, error);
    free(error);
    string_builder_add_char(result, '}');
  }
  char *json = string_builder_dump(result, NULL);
  string_builder_destroy(result);
  string_builder_destroy(warnings);
  game_destroy(live_game);
  if (failed) {
    wasm_game_cache_destroy();
  }
  error_stack_destroy(errors);
  return json;
}

char *wasm_import_gcg(const char *text, const char *lexicon) {
  return wasm_game_action(text, lexicon, 0, "", "", "", 0, "", 0, 0, 0);
}
