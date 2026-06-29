#include "json_api_test.h"

#include "../src/impl/config.h"
#include "../src/impl/json_api.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void assert_json_contains(const char *json, const char *needle) {
  if (!json || !strstr(json, needle)) {
    (void)fprintf(stderr, "expected JSON to contain '%s' but it did not:\n%s\n",
                  needle, json ? json : "(null)");
    assert(0);
  }
}

static void assert_json_absent(const char *json, const char *needle) {
  if (json && strstr(json, needle)) {
    (void)fprintf(stderr, "expected JSON to NOT contain '%s' but it did:\n%s\n",
                  needle, json);
    assert(0);
  }
}

void test_json_api(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 30 -wmp "
      "false");

  // A board with HELLO across row 8 and a blank-played h at row 9, plus a rack.
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/4HELLO6/4h10/15/15/15/15/15/15 "
              "AEINRST/ 12/7 0");

  // State view: structure, metadata, the on-board letters, premiums, the blank.
  char *state = json_api_get_state(config);
  assert_json_contains(state, "\"ok\":true");
  assert_json_contains(state, "\"dim\":15");
  assert_json_contains(state, "\"lexicon\":\"CSW21\"");
  assert_json_contains(state, "\"center\":{\"row\":7,\"col\":7}");
  assert_json_contains(state, "\"board\":[");
  assert_json_contains(state, "\"players\":[");
  assert_json_contains(state, "\"rack\":\"AEINRST\"");
  assert_json_contains(state, "\"score\":12");
  assert_json_contains(state, "\"cgp\":\"");
  // A premium square code and the played blank flag must be present.
  assert_json_contains(state, "\"b\":\"tws\"");
  assert_json_contains(state, "\"k\":true");
  // The blank h is lowercase; the real tiles are uppercase.
  assert_json_contains(state, "\"l\":\"H\"");
  assert_json_contains(state, "\"l\":\"h\"");
  free(state);

  // No moves generated yet.
  char *no_moves = json_api_get_moves(config);
  assert_json_contains(no_moves, "\"hasMoves\":false");
  free(no_moves);

  // No endgame solved yet.
  char *no_endgame = json_api_get_endgame(config);
  assert_json_contains(no_endgame, "\"valid\":false");
  free(no_endgame);

  // Generate and re-check: structured moves with geometry, score, equity,
  // leave.
  load_and_exec_config_or_die(config, "generate");
  char *moves = json_api_get_moves(config);
  assert_json_contains(moves, "\"hasMoves\":true");
  assert_json_contains(moves, "\"hasSim\":false");
  assert_json_contains(moves, "\"moves\":[{");
  assert_json_contains(moves, "\"type\":\"play\"");
  assert_json_contains(moves, "\"placed\":[");
  assert_json_contains(moves, "\"score\":");
  assert_json_contains(moves, "\"equity\":");
  assert_json_contains(moves, "\"leave\":");
  // Sim-only fields must not appear without a simulation.
  assert_json_absent(moves, "\"win\":");
  free(moves);

  config_destroy(config);
}
