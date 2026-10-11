#include "browser_game.h"

#include "../def/board_defs.h"
#include "../def/config_defs.h"
#include "../def/game_defs.h"
#include "../def/rack_defs.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/validated_move.h"
#include "../impl/cgp.h"
#include "../impl/config.h"
#include "../impl/exec.h"
#include "../impl/gameplay.h"
#include "../impl/gcg.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static void run_game_command(Config *config, ErrorStack *error_stack,
                             const char *command, StringBuilder *warnings) {
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  char *output = NULL;
  run_str_api_command(config, error_stack, command, &output);
  if (warnings && output) {
    string_builder_add_string(warnings, output);
  }
  free(output);
}

typedef struct SnapshotWriter {
  StringBuilder *result;
  const Game *live_game;
} SnapshotWriter;

static bool write_snapshot(const GameHistory *game_history, const Game *game,
                           int event_index, void *user_data) {
  SnapshotWriter *writer = user_data;
  StringBuilder *result = writer->result;
  const Game *live_game = writer->live_game;
  const int event_count = game_history_get_num_events(game_history);
  if (event_index) {
    string_builder_add_char(result, ',');
  }
  string_builder_add_string(result, "{\"cgp\":");
  const Game *snapshot_game =
      live_game && event_index == event_count ? live_game : game;
  char *cgp = game_get_cgp(snapshot_game, true);
  string_builder_add_json_string(result, cgp);
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
      string_builder_add_int(result, board_get_square_owner(board, row, col));
    }
    string_builder_add_char(result, ']');
  }
  string_builder_add_string(result, "],\"note\":");
  string_builder_add_json_string(
      result, event_index ? game_event_get_note(game_history_get_event(
                                game_history, event_index - 1))
                          : "");
  string_builder_add_char(result, '}');
  return true;
}

static Config *browser_game_config;
static char *browser_game_lexicon;

void browser_game_cache_destroy(void) {
  config_destroy(browser_game_config);
  browser_game_config = NULL;
  free(browser_game_lexicon);
  browser_game_lexicon = NULL;
}

static bool valid_rack(const char *rack) {
  if (strlen(rack) > RACK_SIZE) {
    return false;
  }
  for (; *rack; rack++) {
    if (!((*rack >= 'A' && *rack <= 'Z') || *rack == '?')) {
      return false;
    }
  }
  return true;
}

static char *without_finished_marker(const char *text) {
  StringBuilder *clean = string_builder_create();
  StringSplitter *lines = split_string_by_newline(text, false);
  for (int line_idx = 0; line_idx < string_splitter_get_number_of_items(lines);
       line_idx++) {
    const char *line = string_splitter_get_item(lines, line_idx);
    if (!has_prefix("#description Magpie finished:", line)) {
      string_builder_add_string(clean, line);
      string_builder_add_char(clean, '\n');
    }
  }
  string_splitter_destroy(lines);
  char *result = string_builder_dump(clean, NULL);
  string_builder_destroy(clean);
  return result;
}

static void write_game_result(const Config *config, Game *live_game,
                              BrowserGameAction action, int event_index,
                              StringBuilder *result,
                              const StringBuilder *warnings,
                              ErrorStack *error_stack) {
  GameHistory *game_history = config_get_game_history(config);
  Game *game = config_get_game(config);
  StringBuilder *gcg = string_builder_create();
  if (live_game) {
    game_history_player_reset_last_rack(game_history, 0);
    game_history_player_reset_last_rack(game_history, 1);
  }
  string_builder_add_gcg(gcg, config_get_ld(config), game_history, false);
  if (live_game &&
      game_get_game_end_reason(live_game) == GAME_END_REASON_NONE) {
    {
      const int player_index = game_get_player_on_turn_index(live_game);
      string_builder_add_formatted_string(gcg, "\n#rack%d ", player_index + 1);
      string_builder_add_rack(
          gcg, player_get_rack(game_get_player(live_game, player_index)),
          config_get_ld(config), true);
    }
    string_builder_add_char(gcg, '\n');
  }
  string_builder_add_formatted_string(
      result, "{\"index\":%d,\"warning\":",
      game_history_get_num_played_events(game_history));
  string_builder_add_json_string(result, string_builder_peek(warnings));
  const int score =
      (action == BROWSER_GAME_COMMIT || action == BROWSER_GAME_LIVE_MOVE) &&
              event_index < game_history_get_num_events(game_history)
          ? equity_to_int(game_event_get_move_score(
                game_history_get_event(game_history, event_index)))
          : 0;
  string_builder_add_formatted_string(result, ",\"score\":%d,\"gcg\":", score);
  string_builder_add_json_string(result, string_builder_peek(gcg));
  string_builder_destroy(gcg);
  string_builder_add_string(result, ",\"players\":[");
  string_builder_add_json_string(result,
                                 game_history_player_get_name(game_history, 0));
  string_builder_add_char(result, ',');
  string_builder_add_json_string(result,
                                 game_history_player_get_name(game_history, 1));
  string_builder_add_formatted_string(
      result, "],\"ended\":%s,\"positions\":[",
      game_get_game_end_reason(live_game ? live_game : game) !=
              GAME_END_REASON_NONE
          ? "true"
          : "false");
  SnapshotWriter writer = {.result = result, .live_game = live_game};
  game_replay_history(game_history, game, write_snapshot, &writer, error_stack);
  string_builder_add_string(result, "]}");
}

// Each edit is transactional: adopt only after validation/confirmation.
char *browser_game_action(const char *text, const char *lexicon,
                          const int event_index, const char *rack,
                          const char *move, const char *note,
                          const BrowserGameAction action,
                          const char *current_cgp, const unsigned int seed,
                          const int current_player, const int allow_phonies) {
  int event_count = 0;
  for (const char *cursor = text; *cursor; cursor++) {
    if (*cursor == '>' && (cursor == text || cursor[-1] == '\n')) {
      event_count++;
    }
  }
  if (strlen(text) > 1024UL * 1024 || event_count > 1000 ||
      action < BROWSER_GAME_IMPORT || action > BROWSER_GAME_LIVE_MOVE ||
      !(strings_equal(lexicon, "CSW24") || strings_equal(lexicon, "NWL23")) ||
      ((action == BROWSER_GAME_COMMIT || action == BROWSER_GAME_LIVE_MOVE) &&
       !valid_rack(rack))) {
    return string_duplicate("{\"error\":\"Invalid game action or import "
                            "exceeds its size limit.\"}");
  }
  ErrorStack *error_stack = error_stack_create();
  const ConfigArgs args = {.data_paths = DEFAULT_DATA_PATHS,
                           .settings_filename = NULL,
                           .use_wmp = false};
  if (browser_game_config && !strings_equal(browser_game_lexicon, lexicon)) {
    browser_game_cache_destroy();
  }
  const bool first_use = browser_game_config == NULL;
  if (first_use) {
    browser_game_config = config_create(&args, error_stack);
    browser_game_lexicon = string_duplicate(lexicon);
  }
  Config *config = browser_game_config;
  StringBuilder *result = string_builder_create();
  StringBuilder *warnings = string_builder_create();
  Game *live_game = NULL;
  if (first_use && error_stack_is_empty(error_stack)) {
    char *command = get_formatted_string("set -lex %s", lexicon);
    char *output = NULL;
    run_str_api_command(config, error_stack, command, &output);
    free(output);
    free(command);
  }
  if (error_stack_is_empty(error_stack)) {
    char *clean =
        (action == BROWSER_GAME_COMMIT || action >= BROWSER_GAME_CHALLENGE)
            ? without_finished_marker(text)
            : string_duplicate(text);
    config_parse_gcg_string(config, clean, config_get_game_history(config),
                            error_stack);
    free(clean);
  }
  if (action != BROWSER_GAME_IMPORT && error_stack_is_empty(error_stack)) {
    bool live_phony = false;
    // Parsing leaves the board at the end but the game_history cursor at zero.
    // Navigate via the end so Config initializes its commit/challenge writer.
    run_game_command(config, error_stack, "goto end", NULL);
    char *command = get_formatted_string("goto %d", event_index);
    run_game_command(config, error_stack, command, NULL);
    free(command);
    if ((action == BROWSER_GAME_DEAL || action == BROWSER_GAME_LIVE_MOVE ||
         (action == BROWSER_GAME_COMMIT && *current_cgp)) &&
        error_stack_is_empty(error_stack)) {
      Game *game = config_get_game(config);
      const int on_turn = current_player == 1 ? 1 : 0;
      if (*current_cgp) {
        game_load_cgp(game, current_cgp, error_stack);
      }
      game_set_player_on_turn_index(game, on_turn);
      game_seed(game, seed);
      if (action == BROWSER_GAME_DEAL && error_stack_is_empty(error_stack)) {
        game_history_truncate_to_played_events(config_get_game_history(config));
        draw_to_full_rack(game, 1 - on_turn);
        draw_to_full_rack(game, on_turn);
        live_game = game_duplicate(game);
      } else if (error_stack_is_empty(error_stack)) {
        ValidatedMoves *moves = validated_moves_create(
            game, on_turn, move,
            action == BROWSER_GAME_COMMIT || allow_phonies != 0, true,
            error_stack);
        if (error_stack_is_empty(error_stack)) {
          live_game = game_duplicate(game);
          live_phony = validated_moves_is_phony(moves, 0);
          if (live_phony && action == BROWSER_GAME_LIVE_MOVE) {
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
    if (action == BROWSER_GAME_COMMIT || action == BROWSER_GAME_LIVE_MOVE) {
      if (!*current_cgp) {
        command = get_formatted_string("rack %s", *rack ? rack : "-");
        run_game_command(config, error_stack, command, NULL);
        free(command);
      }
      if (error_stack_is_empty(error_stack)) {
        // Replay must use the actual ending racks, not a random draw from
        // tiles whose owner was omitted from the public game record.
        if (live_game) {
          for (int player_index = 0; player_index < 2; player_index++) {
            const Rack *actual =
                player_get_rack(game_get_player(live_game, player_index));
            rack_copy(game_history_player_get_last_rack(
                          config_get_game_history(config), player_index),
                      actual);
            rack_copy(
                game_history_player_get_rack_to_draw_before_pass_out_game_end(
                    config_get_game_history(config), player_index),
                actual);
          }
        }
        const Rack *exchange_rack =
            live_game ? player_get_rack(game_get_player(
                            live_game, current_player == 1 ? 1 : 0))
                      : NULL;
        char *warning =
            config_commit_move(config, move, exchange_rack, error_stack);
        string_builder_add_string(warnings, warning);
        free(warning);
      }
      if (action == BROWSER_GAME_LIVE_MOVE && live_phony) {
        run_game_command(config, error_stack, "challenge", NULL);
      }
      if (action == BROWSER_GAME_LIVE_MOVE && !live_phony &&
          error_stack_is_empty(error_stack) &&
          game_history_get_waiting_for_final_pass_or_challenge(
              config_get_game_history(config))) {
        run_game_command(config, error_stack, "commit pass", NULL);
      }
    } else if (action == BROWSER_GAME_CHALLENGE) {
      run_game_command(config, error_stack, "challenge", warnings);
    }
    if ((action == BROWSER_GAME_COMMIT || action == BROWSER_GAME_NOTE) &&
        error_stack_is_empty(error_stack) &&
        (action == BROWSER_GAME_NOTE || *note)) {
      if (game_history_get_num_played_events(config_get_game_history(config)) >
          0) {
        game_history_set_note_for_most_recent_event(
            config_get_game_history(config), note);
        // Accept a note only if the exported record remains importable. Keep
        // validation isolated from the live configuration and history cursor.
        StringBuilder *record = string_builder_create();
        string_builder_add_gcg(record, config_get_ld(config),
                               config_get_game_history(config), false);
        Config *check = config_create(&args, error_stack);
        if (error_stack_is_empty(error_stack)) {
          config_parse_gcg_string(check, string_builder_peek(record),
                                  config_get_game_history(check), error_stack);
        }
        if (error_stack_is_empty(error_stack)) {
          const GameHistory *original = config_get_game_history(config);
          const GameHistory *roundtrip = config_get_game_history(check);
          const int count = game_history_get_num_events(original);
          char *expected = string_duplicate(note);
          trim_whitespace(expected);
          const int target = game_history_get_num_played_events(original) - 1;
          char *actual =
              string_duplicate(target < game_history_get_num_events(roundtrip)
                                   ? game_event_get_note(game_history_get_event(
                                         roundtrip, target))
                                   : "");
          trim_whitespace(actual);
          const bool same = count == game_history_get_num_events(roundtrip) &&
                            strings_equal(expected, actual);
          free(actual);
          free(expected);
          if (!same) {
            error_stack_push(
                error_stack, ERROR_STATUS_NOTE_NO_GAME_EVENTS,
                string_duplicate("This note cannot be saved without changing "
                                 "the game record. Remove lines beginning with "
                                 "GCG commands or moves."));
          }
        }
        config_destroy(check);
        string_builder_destroy(record);
      } else {
        error_stack_push(
            error_stack, ERROR_STATUS_NOTE_NO_GAME_EVENTS,
            string_duplicate("Select an event before adding a note."));
      }
    }
  }
  if (error_stack_is_empty(error_stack)) {
    write_game_result(config, live_game, action, event_index, result, warnings,
                      error_stack);
  }
  const bool failed = !error_stack_is_empty(error_stack);
  if (failed) {
    string_builder_clear(result);
    string_builder_add_string(result, "{\"error\":");
    char *error = error_stack_get_string_and_reset(error_stack);
    string_builder_add_json_string(result, error);
    free(error);
    string_builder_add_char(result, '}');
  }
  char *json = string_builder_dump(result, NULL);
  string_builder_destroy(result);
  string_builder_destroy(warnings);
  game_destroy(live_game);
  if (failed && first_use) {
    browser_game_cache_destroy();
  }
  error_stack_destroy(error_stack);
  return json;
}

char *browser_game_import_gcg(const char *text, const char *lexicon) {
  return browser_game_action(text, lexicon, 0, "", "", "", BROWSER_GAME_IMPORT,
                             "", 0, 0, 0);
}
