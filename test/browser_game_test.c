#include "browser_game_test.h"

#include "../src/def/board_defs.h"
#include "../src/impl/browser_game.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const char *const EMPTY_GAME =
    "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n";
static void assert_success(char *json) {
  assert(!has_substring(json, "\"error\":"));
  assert(has_substring(json, "\"positions\":["));
  free(json);
}

static void test_cgp_start(void) {
  StringBuilder *board = string_builder_create();
  for (int row = 0; row < BOARD_DIM; row++) {
    if (row) {
      string_builder_add_char(board, '/');
    }
    if (row == BOARD_DIM / 2) {
      string_builder_add_formatted_string(board, "%dA%d", BOARD_DIM / 2,
                                          BOARD_DIM / 2);
    } else {
      string_builder_add_int(board, BOARD_DIM);
    }
  }
  char *text = get_formatted_string("%s#cgp %s AEINRST/BCDFGHJ 100/80 1\n",
                                    EMPTY_GAME, string_builder_peek(board));
  char *loaded = browser_game_import_gcg(text, "CSW24");
  assert(has_substring(loaded, "100/80 1"));
  assert(has_substring(loaded, "\"index\":0"));
  assert(has_substring(loaded, "#cgp "));
  assert_success(loaded);
  char *move = get_formatted_string("%d%c .T", (BOARD_DIM / 2) + 1,
                                    'A' + (BOARD_DIM / 2));
  char *played = browser_game_action(text, "CSW24", 0, "AEINRST", move, "",
                                     BROWSER_GAME_COMMIT, "", 0, 0, 0);
  assert(has_substring(played, "+2 102"));
  assert(has_substring(played, "\"index\":1"));
  assert_success(played);
  char *record = get_formatted_string(
      "%s>A: AEINRST %s +2 102\n>B: BCDFGHJ - +0 80\n", text, move);
  char *reloaded = browser_game_import_gcg(record, "CSW24");
  assert(has_substring(reloaded, "\"index\":0"));
  assert(has_substring(reloaded, "100/80 1"));
  assert(has_substring(reloaded, "102/80 1"));
  assert_success(reloaded);
  // Replacing the first move must branch from the CGP, not an empty board.
  assert_success(browser_game_action(record, "CSW24", 0, "AEINRST", move, "",
                                     BROWSER_GAME_COMMIT, "", 0, 0, 0));
  char *invalid = get_formatted_string("%s#cgp bad\n", EMPTY_GAME);
  char *failure = browser_game_import_gcg(invalid, "CSW24");
  assert(has_substring(failure, "\"error\":"));
  free(failure);
  free(invalid);
  char *pass_start =
      get_formatted_string("%s#cgp %s AEINRST/BCDFGHJ 100/80 5\n", EMPTY_GAME,
                           string_builder_peek(board));
  assert_success(browser_game_action(pass_start, "CSW24", 0, "AEINRST", "pass",
                                     "", BROWSER_GAME_COMMIT, "", 0, 0, 0));
  free(pass_start);
  const char *opponent_racks[] = {"", "BC", "BCDFGHJ"};
  for (int index = 0; index < 3; index++) {
    char *cgp =
        get_formatted_string("%s AEINRST/%s 100/80 5",
                             string_builder_peek(board), opponent_racks[index]);
    char *pass_record = get_formatted_string("%s#cgp %s\n", EMPTY_GAME, cgp);
    char *result =
        browser_game_action(pass_record, "CSW24", 0, "AEINRST", "pass", "",
                            BROWSER_GAME_COMMIT, cgp, 42, 0, 0);
    if (index < 2) {
      assert(has_substring(result, "previous pass"));
      free(result);
    } else {
      assert(has_substring(result, "54/93"));
      assert_success(result);
    }
    free(pass_record);
    free(cgp);
  }
  // A normal import after a CGP/error must not inherit its starting board.
  assert_success(browser_game_import_gcg(EMPTY_GAME, "CSW24"));
  free(record);
  free(move);
  free(text);
  string_builder_destroy(board);
}

static void test_cgp_endgame_rack_retention(void) {
  if (BOARD_DIM != 15) {
    return;
  }
  const char *record =
      "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n#cgp "
      "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/"
      "1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/"
      "3PERJURY5/15/15/15 FV/AADIZ 442/388 0\n";
  char *played = browser_game_action(record, "CSW24", 0, "FV", "6B .F", "",
                                     BROWSER_GAME_COMMIT, "", 0, 0, 0);
  assert(has_substring(played, " AADIZ/V 388/452 0"));
  assert_success(played);
  char *completed = get_formatted_string("%s>A: FV 6B .F +10 452\n", record);
  char *loaded = browser_game_import_gcg(completed, "CSW24");
  assert(has_substring(loaded, " AADIZ/V 388/452 0"));
  assert_success(loaded);
  free(completed);
  const char *start = strstr(record, "#cgp ") + 5;
  char *unknown = string_duplicate(start);
  char *rack_start = strstr(unknown, "FV/AADIZ");
  memmove(rack_start + 3, rack_start + 8, strlen(rack_start + 8) + 1);
  char *dealt = browser_game_action(record, "CSW24", 0, "FV", "", "",
                                    BROWSER_GAME_DEAL, unknown, 42, 0, 0);
  assert(has_substring(dealt, " FV/AADIZ 442/388 0"));
  assert_success(dealt);
  free(unknown);
}

void test_browser_game(void) {
  test_cgp_start();
  test_cgp_endgame_rack_retention();
  const int center_row = (BOARD_DIM / 2) + 1;
  const char center_col = 'A' + (BOARD_DIM / 2);
  char *opening_move =
      get_formatted_string("%d%c TRAIN", center_row, center_col);
  char *opening_game = get_formatted_string(
      "%s#description Magpie finished: resigned\n>A: AEINRST %s +12 12\n",
      EMPTY_GAME, opening_move);
  StringBuilder *board = string_builder_create();
  for (int row = 0; row < BOARD_DIM; row++) {
    string_builder_add_formatted_string(board, "%s%d", row ? "/" : "",
                                        BOARD_DIM);
  }
  string_builder_add_string(board, " AEINRST/ 0/0 0");
  const char *initial_cgp = string_builder_peek(board);

  // A manual review can have no rack for the next player. Do not export an
  // empty rack pragma, which makes the next edit or import fail.
  char *manual =
      browser_game_action(EMPTY_GAME, "CSW24", 0, "AEINRST", opening_move, "",
                          BROWSER_GAME_COMMIT, initial_cgp, 0, 0, 0);
  assert(!has_substring(manual, "#rack2"));
  assert_success(manual);

  for (int cycle_idx = 0; cycle_idx < 3; cycle_idx++) {
    assert_success(browser_game_import_gcg(EMPTY_GAME, "CSW24"));
    assert_success(browser_game_import_gcg(opening_game, "CSW24"));
  }
  char *changed =
      browser_game_action(opening_game, "CSW24", 0, "AEINRST", opening_move, "",
                          BROWSER_GAME_COMMIT, "", 0, 0, 0);
  assert(!has_substring(changed, "Magpie finished:"));
  assert_success(changed);
  char *note = browser_game_action(opening_game, "CSW24", 1, "", "",
                                   "quote \" and newline\n", BROWSER_GAME_NOTE,
                                   "", 0, 0, 0);
  assert(has_substring(note, "quote \\\""));
  assert_success(note);
  const char *bad_notes[] = {"\nsecond line", "Plan:\n#identify trouble",
                             "a\n>B: BCDFGHJ - +0 0", "a\n#rack2 BCDFGHJ"};
  for (int index = 0; index < 4; index++) {
    char *rejected =
        browser_game_action(opening_game, "CSW24", 1, "", "", bad_notes[index],
                            BROWSER_GAME_NOTE, "", 0, 0, 0);
    assert(has_substring(rejected, "\"error\":"));
    free(rejected);
    assert_success(browser_game_import_gcg(opening_game, "CSW24"));
  }
  assert_success(browser_game_action(EMPTY_GAME, "CSW24", 0, "", "", "",
                                     BROWSER_GAME_DEAL, initial_cgp, 42, 0, 0));
  assert_success(browser_game_action(EMPTY_GAME, "CSW24", 0, "AEINRST",
                                     opening_move, "", BROWSER_GAME_LIVE_MOVE,
                                     initial_cgp, 42, 0, 0));
  char *phony = get_formatted_string("%s>A: II %d%c II +4 4\n", EMPTY_GAME,
                                     center_row, center_col);
  assert_success(browser_game_action(phony, "CSW24", 1, "", "", "",
                                     BROWSER_GAME_CHALLENGE, "", 0, 0, 0));
  const char *bad_moves[] = {"8H TRAIN -lex NWL23", "pass -threads 32",
                             "ex AE -lex NWL23"};
  for (int move_idx = 0;
       move_idx < (int)(sizeof(bad_moves) / sizeof(bad_moves[0])); move_idx++) {
    char *bad = browser_game_action(EMPTY_GAME, "CSW24", 0, "AEINRST",
                                    bad_moves[move_idx], "",
                                    BROWSER_GAME_COMMIT, "", 0, 0, 0);
    assert(has_substring(bad, "\"error\":"));
    free(bad);
  }
  char *bad = browser_game_import_gcg("not a game", "CSW24");
  assert(has_substring(bad, "\"error\":"));
  free(bad);
  assert_success(browser_game_import_gcg(opening_game, "CSW24"));
  char *large = malloc_or_die(1024 * 1024 + 2);
  memset(large, 'a', 1024 * 1024 + 1);
  large[1024 * 1024 + 1] = '\0';
  bad = browser_game_import_gcg(large, "CSW24");
  assert(has_substring(bad, "size limit"));
  free(bad);
  free(large);
  free(phony);
  free(opening_game);
  free(opening_move);
  string_builder_destroy(board);
  browser_game_cache_destroy();
}
