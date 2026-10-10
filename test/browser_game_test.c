#include "browser_game_test.h"

#include "../src/def/board_defs.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "../wasmentry/browser_game.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const char *const EMPTY =
    "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n";
static const char *const ONE =
    "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n#description Magpie "
    "finished: resigned\n>A: AEINRST 8H TRAIN +12 12\n";
static const char *const CGP =
    "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0";

static void assert_success(char *json) {
  assert(!has_substring(json, "\"error\":"));
  assert(has_substring(json, "\"positions\":["));
  free(json);
}

void test_browser_game(void) {
  if (BOARD_DIM != 15) {
    return;
  }
  for (int repeat = 0; repeat < 3; repeat++) {
    assert_success(wasm_import_gcg(EMPTY, "CSW24"));
    assert_success(wasm_import_gcg(ONE, "CSW24"));
  }
  char *changed = wasm_game_action(ONE, "CSW24", 0, "AEINRST", "8H TRAIN", "",
                                   1, "", 0, 0, 0);
  assert(!has_substring(changed, "Magpie finished:"));
  assert_success(changed);
  char *note = wasm_game_action(ONE, "CSW24", 1, "", "",
                                "quote \" and newline\n", 2, "", 0, 0, 0);
  assert(has_substring(note, "quote \\\""));
  assert_success(note);
  assert_success(
      wasm_game_action(EMPTY, "CSW24", 0, "", "", "", 4, CGP, 42, 0, 0));
  assert_success(wasm_game_action(EMPTY, "CSW24", 0, "AEINRST", "8H TRAIN", "",
                                  5, CGP, 42, 0, 0));
  const char *phony =
      "#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n>A: II 8H II +4 4\n";
  assert_success(
      wasm_game_action(phony, "CSW24", 1, "", "", "", 3, "", 0, 0, 0));
  const char *bad_moves[] = {"8H TRAIN -lex NWL23", "pass -threads 32",
                             "ex AE -lex NWL23"};
  for (int index = 0; index < 3; index++) {
    char *bad = wasm_game_action(EMPTY, "CSW24", 0, "AEINRST", bad_moves[index],
                                 "", 1, "", 0, 0, 0);
    assert(has_substring(bad, "\"error\":"));
    free(bad);
  }
  char *bad = wasm_import_gcg("not a game", "CSW24");
  assert(has_substring(bad, "\"error\":"));
  free(bad);
  assert_success(wasm_import_gcg(ONE, "CSW24"));
  char *large = malloc_or_die(1024 * 1024 + 2);
  memset(large, 'a', 1024 * 1024 + 1);
  large[1024 * 1024 + 1] = '\0';
  bad = wasm_import_gcg(large, "CSW24");
  assert(has_substring(bad, "size limit"));
  free(bad);
  free(large);
  wasm_game_cache_destroy();
}
