// Tests for entering a game in annotation mode: an exchange or a pass
// committed with Enter is played into the game, and the editor moves on to
// the next player's turn, as after a placement. Drives the History cell
// editor the way the keyboard does. Run from the repository root (reads
// data/).
// Build and run: make magpie_tui_app_test && ./bin/magpie_tui_app_test

#include "../../../src/ent/game.h"
#include "../../../src/ent/player.h"
#include "../../../src/ent/rack.h"
#include "../../bot_worker.h"
#include "../../game_state.h"
#include "../../input_cell_editor.h"
#include "../../tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_SEED = 7 };

static int failures = 0;

static void check(bool ok, const char *what) {
  if (!ok) {
    (void)fprintf(stderr, "FAIL: %s\n", what);
    failures++;
  }
}

static void check_int(int got, int want, const char *what) {
  if (got != want) {
    (void)fprintf(stderr, "FAIL: %s: got %d, want %d\n", what, got, want);
    failures++;
  }
}

static void check_str(const char *got, const char *want, const char *what) {
  if (strcmp(got, want) != 0) {
    (void)fprintf(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", what, got,
                  want);
    failures++;
  }
}

static void press(TuiGameState *state, uint32_t key) {
  ncinput input;
  memset(&input, 0, sizeof(input));
  input.id = key;
  if (key < 0x80) {
    input.utf8[0] = (char)key;
  }
  (void)tui_input_cell_editor(state, key, input);
}

static void type_text(TuiGameState *state, const char *text) {
  for (const char *ch = text; *ch != '\0'; ch++) {
    press(state, (uint32_t)(unsigned char)*ch);
  }
}

// Starts annotating a new game the way Play setup does: one pending turn
// for the first player, with the editor open on its move.
static void start_annotation(TuiGameState *state) {
  state->app_mode = TUI_APP_MODE_ANNOTATE;
  tui_game_state_reset_game_for_annotation(state);
  tui_bot_worker_append_pending_history(state, 0, NULL,
                                        state->time_per_side_seconds);
  state->edit_history_idx = 0;
  state->edit_field = TUI_EDIT_FIELD_MOVE;
  state->edit_move_buf[0] = '\0';
  state->edit_move_len = 0;
  state->edit_move_cursor = 0;
  state->edit_rack_buf[0] = '\0';
  state->edit_rack_len = 0;
  state->edit_rack_cursor = 0;
  state->edit_rack_user_modified = false;
  tui_game_state_parse_edit_buf(state);
  state->focused_panel = TUI_FOCUS_HISTORY;
  state->history_cursor = 0;
}

static void test_exchange_then_pass(TuiGameState *state) {
  start_annotation(state);

  // Turn 1: the first player exchanges CCDLV from ECCDLSV, keeping ES.
  type_text(state, "-CCDLV");
  press(state, NCKEY_ENTER);
  type_text(state, "ES");
  press(state, NCKEY_ENTER);
  check_int(state->history_count, 2, "exchange appends turn 2");
  check(!state->history[0].pending, "exchange turn is committed");
  check_str(state->history[0].error_str, "", "exchange turn error");
  check(state->history[0].cgp_before[0] != '\0',
        "exchange turn has a position to analyze");
  check_int(state->history[1].player_idx, 1, "turn 2 player");
  check(state->history[1].pending, "turn 2 waits for entry");
  check_int(state->edit_history_idx, 1, "editor moves to turn 2");
  check_int(state->history_cursor, 1, "History cursor moves to turn 2");
  const Rack *exchanger_rack = player_get_rack(game_get_player(state->game, 0));
  check_int(rack_get_total_letters(exchanger_rack), 2,
            "exchanger keeps only the leave");
  const char *reason =
      tui_analysis_unavailable_reason(state, 0, TUI_ANALYSIS_SIM);
  check(reason == NULL ||
            strcmp(reason, "this turn has no position to analyze") != 0,
        "exchange turn can be simmed");

  // Turn 2: the second player, whose rack wasn't entered, passes.
  type_text(state, "pass");
  press(state, NCKEY_ENTER);
  press(state, NCKEY_ENTER);
  check_int(state->history_count, 3, "pass appends turn 3");
  check(!state->history[1].pending, "pass turn is committed");
  check_str(state->history[1].error_str, "", "pass turn error");
  check_int(state->history[2].player_idx, 0, "turn 3 player");
  check_int(state->edit_history_idx, 2, "editor moves to turn 3");
  check_str(state->edit_rack_buf, "ES",
            "turn 3 starts from the exchanger's leave");
}

// A placed turn's saved position, which /sim starts from, holds the rack
// entered after the turn opened.
static void test_placement_position_has_rack(TuiGameState *state) {
  start_annotation(state);
  type_text(state, "8D GRAZE");
  press(state, NCKEY_ENTER);
  // The rack starts with the tiles played; add the leave.
  check_str(state->edit_rack_buf, "AEGRZ", "rack starts with the tiles played");
  type_text(state, "OT");
  press(state, NCKEY_ENTER);
  check_int(state->history_count, 2, "placement appends turn 2");
  const TuiHistoryEntry *entry = &state->history[0];
  check(strstr(entry->cgp_before, " AEGORTZ/") != NULL,
        "placement turn's position has its rack");
  check(entry->rack_before != NULL &&
            rack_get_total_letters(entry->rack_before) == 7,
        "placement turn shows its rack");
  check(entry->bag_before_known, "placement turn's bag is known");
  check_int(entry->bag_before, 86, "placement turn's bag");
}

// A turn with only its rack entered can be kibitzed and simmed before
// its play is entered.
static void test_rack_only_turn_can_be_analyzed(TuiGameState *state) {
  start_annotation(state);
  check_str(tui_analysis_unavailable_reason(state, 0, TUI_ANALYSIS_KIBITZ),
            "enter this turn's rack first", "kibitz before the rack");
  press(state, NCKEY_ENTER);
  type_text(state, "AEOGRTZ");
  press(state, NCKEY_ENTER);
  const TuiHistoryEntry *entry = &state->history[0];
  check(entry->pending, "rack-only turn stays open");
  check(strstr(entry->cgp_before, " AEGORTZ/") != NULL,
        "rack-only turn's position has its rack");
  check_int(
      rack_get_total_letters(player_get_rack(game_get_player(state->game, 0))),
      7, "Rack panel shows the rack entered");
  const char *sim_reason =
      tui_analysis_unavailable_reason(state, 0, TUI_ANALYSIS_SIM);
  check(sim_reason == NULL, "rack-only turn can be simmed");
  check(tui_analysis_kibitz(state, 0), "rack-only turn is kibitzed");
  check(entry->analysis_snapshot.num_rows > 1, "kibitz ranks several plays");

  // Choosing the top play enters it as the turn's play.
  char top_play[sizeof(entry->analysis_snapshot.rows[0].move)];
  (void)snprintf(top_play, sizeof(top_play), "%s",
                 entry->analysis_snapshot.rows[0].move);
  check(tui_cell_editor_enter_play(state, 0, top_play),
        "the chosen play is entered");
  check(!entry->pending, "the turn is committed");
  check_str(entry->error_str, "", "the chosen play's error");
  check_int(state->history_count, 2, "the chosen play appends turn 2");
  check_int(state->edit_history_idx, 1, "editor moves to turn 2");
  check(entry->analysis_snapshot.num_rows > 1, "the turn keeps its kibitz");
}

// An exchange chosen from the analysis, which shows it as "(exch ...)",
// is entered as typed exchanges are.
static void test_chosen_exchange_is_entered(TuiGameState *state) {
  start_annotation(state);
  press(state, NCKEY_ENTER);
  type_text(state, "ECCDLSV");
  press(state, NCKEY_ENTER);
  check(tui_cell_editor_enter_play(state, 0, "(exch CCDLV)"),
        "the chosen exchange is entered");
  check_str(state->history[0].move_str, "-CCDLV", "the chosen exchange");
  check_str(state->history[0].error_str, "", "the chosen exchange's error");
  check_int(state->history_count, 2, "the chosen exchange appends turn 2");
}

int main(void) {
  TuiGameState *state = calloc(1, sizeof(*state));
  if (state == NULL) {
    return 1;
  }
  char error_message[256];
  if (!tui_game_state_init("CSW24", TEST_SEED, false, state, error_message,
                           sizeof(error_message))) {
    (void)fprintf(stderr, "could not start a game: %s\n", error_message);
    free(state);
    return 1;
  }
  test_exchange_then_pass(state);
  test_placement_position_has_rack(state);
  test_rack_only_turn_can_be_analyzed(state);
  test_chosen_exchange_is_entered(state);
  tui_game_state_destroy(state);
  free(state);
  if (failures > 0) {
    (void)fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  (void)printf("annotate: all tests passed\n");
  return 0;
}
