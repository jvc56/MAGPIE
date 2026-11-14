// Tests for entering a game in annotation mode: an exchange or a pass
// committed with Enter is played into the game, and the editor moves on to
// the next player's turn, as after a placement. Drives the History cell
// editor the way the keyboard does.
//
// Also tests that the game state plays with the lexicon's pre-endgame
// adjustments: a kibitz of a late position has the table's amounts in its
// equities.
//
// Run from the repository root (reads data/).
// Build and run: make magpie_tui_app_test && ./bin/magpie_tui_app_test

#include "../../../src/def/equity_defs.h"
#include "../../../src/def/game_history_defs.h"
#include "../../../src/def/move_defs.h"
#include "../../../src/def/rack_defs.h"
#include "../../../src/ent/bag.h"
#include "../../../src/ent/data_filepaths.h"
#include "../../../src/ent/equity.h"
#include "../../../src/ent/game.h"
#include "../../../src/ent/letter_distribution.h"
#include "../../../src/ent/move.h"
#include "../../../src/ent/peg_adjust.h"
#include "../../../src/ent/player.h"
#include "../../../src/ent/players_data.h"
#include "../../../src/ent/rack.h"
#include "../../../src/impl/cgp.h"
#include "../../../src/impl/move_gen.h"
#include "../../../src/util/io_util.h"
#include "../../analysis_rows.h"
#include "../../bot_worker.h"
#include "../../game_state.h"
#include "../../input_cell_editor.h"
#include "../../input_load.h"
#include "../../tui_ui_state.h"
#include "../../tui_ui_types.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TEST_SEED = 7,
  // The tiles in the bag of LATE_POSITION_CGP.
  LATE_POSITION_BAG = 2,
  // More than the plays of LATE_POSITION_CGP: a list this long holds them
  // all.
  ALL_PLAYS_CAPACITY = 4096,
};

// A CSW24 position with two tiles in the bag, given as a turn of an
// annotated or loaded game is saved: with the rack of the player on turn
// and nothing of the opponent's, whose seven tiles the engine then counts
// in its bag.
static const char *const LATE_POSITION_CGP =
    "15/13T1/12DEW/FE5NARGHILE/OR11E1/OK11s1/T4NU5BA1/R5HM1ROPILY/"
    "AIYEE2EWE2ZE1/1FUNDING1Q5/9U5/2OVERLADE1GOV1/7MASTIX2/1BOrICUA1T5/"
    "7SISSOO2 AIJNNPT/ 476/394 0";

// How far a row's shown equity may be from the equity it shows.
static const double SHOWN_TOLERANCE = 1e-9;

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

// The engine's bag holds no more tiles than the distribution has.
static void check_bag_fits(const TuiGameState *state, const char *what) {
  const int in_bag = bag_get_letters(game_get_bag(state->game));
  const int total = ld_get_total_tiles(state->ld);
  if (in_bag > total) {
    (void)fprintf(stderr, "FAIL: %s: %d tiles in a bag of %d\n", what, in_bag,
                  total);
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
  check_bag_fits(state, "bag after the first player's exchange");

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

// The second player draws from the other end of the bag, and exchanged
// tiles go back in at the exchanger's end.
static void test_second_player_exchange(TuiGameState *state) {
  start_annotation(state);
  type_text(state, "pass");
  press(state, NCKEY_ENTER);
  press(state, NCKEY_ENTER);
  type_text(state, "-CCDLV");
  press(state, NCKEY_ENTER);
  type_text(state, "ES");
  press(state, NCKEY_ENTER);
  check_int(state->history_count, 3, "second player's exchange appends turn 3");
  check_int(state->history[1].player_idx, 1, "the exchanger is player 2");
  check(!state->history[1].pending, "second player's exchange is committed");
  check_str(state->history[1].error_str, "", "second player's exchange error");
  check_bag_fits(state, "bag after the second player's exchange");
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

  state->history_cursor = 0;
  populate_frame_analysis_rows(state);
  check(state->last_rendered_analysis_row_count > 1,
        "the Plays panel shows the kibitz");

  // Choosing the top play enters it as the turn's play.
  char top_play[sizeof(state->last_rendered_analysis_rows[0].move)];
  (void)snprintf(top_play, sizeof(top_play), "%s",
                 state->last_rendered_analysis_rows[0].move);
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

// Whether the game state's data has the pre-endgame adjustments named
// `table_name`, by the lookup the engine loads a table with. Data fetched
// before the tables were added to it has none.
static bool data_has_peg_adjust(const TuiGameState *state,
                                const char *table_name) {
  ErrorStack *error_stack = error_stack_create();
  char *file_name = data_filepaths_get_readable_filename(
      state->data_paths, table_name, DATA_FILEPATH_TYPE_PEG_ADJUST,
      error_stack);
  const bool has_table = file_name != NULL;
  free(file_name);
  error_stack_destroy(error_stack);
  return has_table;
}

// The game state plays with its lexicon's own pre-endgame adjustments,
// named `table_name`: the table the command line program plays with by
// default. A lexicon whose data has no such table is played without one.
static void test_lexicon_peg_adjust_is_loaded(const TuiGameState *state,
                                              const char *table_name) {
  if (!data_has_peg_adjust(state, table_name)) {
    (void)printf("annotate: the data has no %s: checking that the game is "
                 "played without a table\n",
                 table_name);
    for (int player_idx = 0; player_idx < 2; player_idx++) {
      check(players_data_get_peg_adjust(state->players_data, player_idx) ==
                NULL,
            "data without the table gives no player one");
      check(player_get_peg_adjust(game_get_player(state->game, player_idx)) ==
                NULL,
            "the game's players play without a table");
    }
    return;
  }
  const PegAdjust *table = players_data_get_peg_adjust(state->players_data, 0);
  check(table != NULL, "the lexicon's pre-endgame adjustments are loaded");
  if (table == NULL) {
    return;
  }
  check_str(peg_adjust_get_name(table), table_name, "the table's name");
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    check(players_data_get_peg_adjust(state->players_data, player_idx) == table,
          "both players have the one table");
    check(player_get_peg_adjust(game_get_player(state->game, player_idx)) ==
              table,
          "the game's players play with the table");
  }
}

// A game state of another lexicon, as a change of lexicon starts one, plays
// with that lexicon's table.
static void test_other_lexicon_has_its_table(void) {
  TuiGameState *state = calloc(1, sizeof(*state));
  if (state == NULL) {
    check(false, "memory for a second game state");
    return;
  }
  char error_message[256];
  if (tui_game_state_init("NWL23", TEST_SEED, false, state, error_message,
                          sizeof(error_message))) {
    test_lexicon_peg_adjust_is_loaded(state, "NWL23_pegadjust");
    tui_game_state_destroy(state);
  } else {
    (void)fprintf(stderr, "FAIL: could not start an NWL23 game: %s\n",
                  error_message);
    failures++;
  }
  free(state);
}

// Opens `cgp` the way Load position does, in annotation mode, where the
// turn a position opens with can be analyzed before its play is entered.
static void load_position(TuiGameState *state, const char *cgp) {
  TuiUiState *ui = calloc(1, sizeof(*ui));
  if (ui == NULL) {
    check(false, "memory for the Load position text");
    return;
  }
  state->app_mode = TUI_APP_MODE_ANNOTATE;
  ui->modal = TUI_MODAL_LOAD_POSITION;
  (void)snprintf(ui->load_position_buf, sizeof(ui->load_position_buf), "%s",
                 cgp);
  ui->load_position_len = (int)strlen(ui->load_position_buf);
  ui->load_position_dirty = true;
  tui_load_position_live_parse(state, ui);
  check(ui->load_position_parse_ok, "the position loads");
  free(ui);
}

// Finds `move` in `plays`, every play of its position, and sets `*equity`
// to the equity it has there.
static bool find_play_equity(const MoveList *plays, const Move *move,
                             Equity *equity) {
  const int number_of_plays = move_list_get_count(plays);
  for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
    const Move *play = move_list_get_move(plays, play_idx);
    if (compare_moves_without_equity(play, move, true) == -1) {
      *equity = move_get_equity(play);
      return true;
    }
  }
  return false;
}

// A kibitz of a late position has the table's amounts in its equities:
// each tile placement shown has the equity it gets without a table plus
// the table's amount for the tiles it plays. The amount is read by the
// tiles really in the bag, which the opponent's unknown rack is not part
// of.
static void test_kibitz_has_peg_adjust(TuiGameState *state) {
  const PegAdjust *table = players_data_get_peg_adjust(state->players_data, 0);
  if (table == NULL) {
    // The data has no table, or test_lexicon_peg_adjust_is_loaded has
    // reported that it is not loaded.
    return;
  }
  load_position(state, LATE_POSITION_CGP);
  check_int(state->history_count, 1, "the position opens one turn");
  if (state->history_count != 1) {
    return;
  }
  const TuiHistoryEntry *entry = &state->history[0];
  check(strstr(entry->cgp_before, " AIJNNPT/ ") != NULL,
        "the turn's position has the rack on turn only");
  check(tui_analysis_kibitz(state, 0), "the late position is kibitzed");
  const TuiAnalysisSnapshot *snap = &entry->analysis_snapshot;
  check(snap->valid && snap->is_static, "the turn has its kibitz");
  check(snap->num_rows > 1, "the kibitz ranks several plays");
  check_int(entry->static_moves_saved_count, snap->num_rows,
            "the kibitz keeps a move for each row");
  if (!snap->valid || entry->static_moves_saved_count != snap->num_rows) {
    return;
  }

  // The same position played without a table, and every play's equity
  // there.
  Game *plain_position = game_duplicate(state->game);
  ErrorStack *error_stack = error_stack_create();
  game_load_cgp(plain_position, entry->cgp_before, error_stack);
  check(error_stack_is_empty(error_stack), "the turn's position loads");
  error_stack_destroy(error_stack);
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    player_set_peg_adjust(game_get_player(plain_position, player_idx), NULL);
  }
  const int tiles_in_bag = game_get_real_bag_size(plain_position);
  check_int(tiles_in_bag, LATE_POSITION_BAG, "tiles in the bag");
  check_int(bag_get_letters(game_get_bag(plain_position)),
            LATE_POSITION_BAG + RACK_SIZE,
            "the engine's bag also holds the unknown rack");
  MoveList *plain_plays = move_list_create(ALL_PLAYS_CAPACITY);
  const MoveGenArgs args = {
      .game = plain_position,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .move_list = plain_plays,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&args);
  check(move_list_get_count(plain_plays) < ALL_PLAYS_CAPACITY,
        "the list without a table holds every play");

  int adjusted_rows = 0;
  for (int row_idx = 0; row_idx < snap->num_rows; row_idx++) {
    const Move *move = &entry->static_moves_saved[row_idx];
    const AnalysisRow *row = &snap->rows[row_idx];
    if (move_get_type(move) == GAME_EVENT_PASS) {
      // The pass has a fixed value in place of an equity.
      continue;
    }
    Equity want = 0;
    if (!find_play_equity(plain_plays, move, &want)) {
      (void)fprintf(stderr, "FAIL: %s is not a play without a table\n",
                    row->move);
      failures++;
      continue;
    }
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      const Equity amount = peg_adjust_get_placement_value(
          table, tiles_in_bag, move_get_tiles_played(move));
      want += amount;
      if (amount != 0) {
        adjusted_rows++;
      }
    }
    const double shown_difference =
        row->secondary_value - equity_to_double(want);
    if (move_get_equity(move) != want || shown_difference > SHOWN_TOLERANCE ||
        shown_difference < -SHOWN_TOLERANCE) {
      (void)fprintf(stderr,
                    "FAIL: equity of %s: got %.3f, shown as %.3f, want %.3f\n",
                    row->move, equity_to_double(move_get_equity(move)),
                    row->secondary_value, equity_to_double(want));
      failures++;
    }
  }
  check(adjusted_rows > 0, "the table changes equities the kibitz shows");
  move_list_destroy(plain_plays);
  game_destroy(plain_position);
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
  test_second_player_exchange(state);
  test_placement_position_has_rack(state);
  test_rack_only_turn_can_be_analyzed(state);
  test_chosen_exchange_is_entered(state);
  test_lexicon_peg_adjust_is_loaded(state, "CSW24_pegadjust");
  test_kibitz_has_peg_adjust(state);
  tui_game_state_destroy(state);
  free(state);
  test_other_lexicon_has_its_table();
  if (failures > 0) {
    (void)fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  (void)printf("annotate: all tests passed\n");
  return 0;
}
