// Tests for bingo_probs_run.
//
// Empty-board tests isolate the rack composition; the BOX/x/U test
// exercises a constrained mid-game where the board itself is the
// dominant factor in both opp and self bingo probabilities.

#include "bingo_probs_test.h"

#include "../src/ent/game.h"
#include "../src/impl/bingo_probs.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *EMPTY_BOARD_15X15 =
    "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15";

// Returns the first percent value appearing after `substring`. Aborts
// if the substring or the percent cannot be located. Specifically, for
// exhaustive output the percent comes from the "weighted: a/b = X%"
// line; for sampled output it comes from the "sampled: ... = X%" line.
static double extract_percent_after(const char *output, const char *substring) {
  const char *p = strstr(output, substring);
  assert(p != NULL);
  const char *pct = strchr(p, '%');
  assert(pct != NULL);
  const char *num_start = pct - 1;
  while (num_start > p &&
         (isdigit((unsigned char)*num_start) || *num_start == '.')) {
    num_start--;
  }
  num_start++;
  return strtod(num_start, NULL);
}

static char *run_bingo_probs_for_cgp(Config *config, const char *cgp,
                                     uint64_t sample_count) {
  load_and_exec_config_or_die(config, "set -lex CSW21 -wmp true -threads 4");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, cgp);
  ErrorStack *error_stack = error_stack_create();
  char *result = bingo_probs_run(game, 4, sample_count, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(result != NULL);
  error_stack_destroy(error_stack);
  game_destroy(game);
  return result;
}

// Empty board, our rack is SATINE. Replenishing 1 tile from the bag
// gives a 7-letter bingo for 91 of 94 unseen tiles -> ~96.8%.
static void test_bingo_probs_satine_exhaustive(void) {
  Config *config = config_create_default_test();
  char cgp[256];
  (void)snprintf(cgp, sizeof(cgp), "%s SATINE/ 0/0 0", EMPTY_BOARD_15X15);
  char *output = run_bingo_probs_for_cgp(config, cgp, 0);
  const double self_pct = extract_percent_after(output, "self_bingo");
  assert(self_pct > 96.5 && self_pct < 97.0);
  free(output);
  config_destroy(config);
}

// Same setup but Monte Carlo. With 50k samples the standard error on a
// ~97% probability is ~0.08%, so a tolerance of 0.5% is safely above
// the 6-sigma band.
static void test_bingo_probs_satine_sampled(void) {
  Config *config = config_create_default_test();
  char cgp[256];
  (void)snprintf(cgp, sizeof(cgp), "%s SATINE/ 0/0 0", EMPTY_BOARD_15X15);
  char *output = run_bingo_probs_for_cgp(config, cgp, 50000);
  const double self_pct = extract_percent_after(output, "self_bingo");
  assert(self_pct > 96.3 && self_pct < 97.3);
  free(output);
  config_destroy(config);
}

// Empty board, our rack is MSUUUU - a notoriously bad leave. Only a
// handful of tiles complete a 7-letter bingo, so self_bingo should be
// in the low single digits.
static void test_bingo_probs_msuuuu_exhaustive(void) {
  Config *config = config_create_default_test();
  char cgp[256];
  (void)snprintf(cgp, sizeof(cgp), "%s MSUUUU/ 0/0 0", EMPTY_BOARD_15X15);
  char *output = run_bingo_probs_for_cgp(config, cgp, 0);
  const double self_pct = extract_percent_after(output, "self_bingo");
  assert(self_pct < 10.0);
  free(output);
  config_destroy(config);
}

// A constrained board: HOX at 8G horizontally, with H(O)x at H7
// (vertical, blank x) and (X)U at I8 (vertical) hooked off it. HOX
// has essentially no useful cross-hooks (no HOXY/HOXA equivalents),
// so the only bingo on this board takes a specific 10-letter word:
// ZANTHOXYLS played through HOX. With LNSTYZ on rack, drawing any
// of the 9 A's or the 1 remaining blank completes that word — 10
// winning draws of 88 unseen tiles -> 10/88 = 5/44 = 11.364%.
static void test_bingo_probs_constrained_board(void) {
  Config *config = config_create_default_test();
  const char *cgp =
      "15/15/15/15/15/15/7H7/6HOX6/7xU6/15/15/15/15/15/15 LNSTYZ/ 22/0 0";
  char *output = run_bingo_probs_for_cgp(config, cgp, 0);
  const double opp_pct = extract_percent_after(output, "opp_bingo");
  const double self_pct = extract_percent_after(output, "self_bingo");
  assert(opp_pct > 0.3 && opp_pct < 0.6);
  assert(self_pct > 11.0 && self_pct < 11.7);
  free(output);
  config_destroy(config);
}

void test_bingo_probs(void) {
  test_bingo_probs_satine_exhaustive();
  test_bingo_probs_satine_sampled();
  test_bingo_probs_msuuuu_exhaustive();
  test_bingo_probs_constrained_board();
}
