#include "peg_validate_test.h"

#include "../src/compat/ctime.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Checks the complete PEG solve (no time limit, no -pegtopk) against the
// variants it must agree with, on committed fixtures (notes/peg_positions)
// with the opponent's rack taken out. Each check compares two solves at the
// same endgame depth, so it holds at any depth; they run at 2 plies, below
// the default of 3, to keep the test to about two hours:
//
// 1. Early cutoff is sound: with it off, the best play is the same (up to
//    ties) and has the same win%.
// 2. First-win endgames give the same verdicts as full-window ones: every
//    candidate that empties the bag has the same wins, draws and losses as
//    in a full-window solve of the same candidates (-pegtopk all). Those that
//    leave tiles are left out: with a full window, a nested player ranks its
//    plays by spread rather than by wins and draws, by design. Only the
//    1-in-the-bag positions run it: a play that empties the bag leads to the
//    same endgames at any bag size, and at 2 in the bag the full-window
//    solve's nested PEGs took over 45 minutes on one position.
// 3. The pessimistic opponent is a lower bound: no candidate that leaves
//    tiles in the bag has a higher win% with it than with the rational one.
//
// Prints each position's verdicts and times, and fails on a disagreement.

enum {
  PEG_VALIDATE_VARIANTS = 4,
};

typedef enum {
  PEG_VALIDATE_DEFAULT,
  PEG_VALIDATE_NO_CUTOFF,
  PEG_VALIDATE_FULL_WINDOW,
  PEG_VALIDATE_RATIONAL,
} peg_validate_variant_t;

static const char *const peg_validate_names[PEG_VALIDATE_VARIANTS] = {
    "default", "no cutoff", "full window", "rational"};

// The complete solve's arguments, changed for `variant`.
static void peg_validate_set_variant(PegArgs *args,
                                     peg_validate_variant_t variant) {
  switch (variant) {
  case PEG_VALIDATE_DEFAULT:
    break;
  case PEG_VALIDATE_NO_CUTOFF:
    args->early_cutoff = false;
    break;
  case PEG_VALIDATE_FULL_WINDOW:
    args->early_cutoff = false;
    args->first_win = false;
    break;
  case PEG_VALIDATE_RATIONAL:
    args->early_cutoff = false;
    args->opp_model = PEG_OPP_RATIONAL;
    args->nested_pessimistic = false;
    break;
  }
}

// The candidate in `result` that is `move`, or NULL.
static const PegRankedCand *peg_validate_find(const PegResult *result,
                                              const Move *move) {
  for (int cand_idx = 0; cand_idx < result->n_top_cands; cand_idx++) {
    if (compare_moves_without_equity(&result->top_cands[cand_idx].move, move,
                                     true) == -1) {
      return &result->top_cands[cand_idx];
    }
  }
  return NULL;
}

static bool peg_validate_same_win(double first, double second) {
  return first - second < 1e-9 && second - first < 1e-9;
}

static void peg_validate_print_move(const Game *game, const Move *move) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          false);
  printf("%s", string_builder_peek(sb));
  string_builder_destroy(sb);
}

// Solves one position in every variant, skipping the full-window one unless
// `full_window`, and checks them. Returns how many checks failed.
static int peg_validate_position(Config *config, int position_idx,
                                 bool full_window) {
  const Game *game = config_get_game(config);
  const int bag_size = peg_compute_bag_size(game);
  PegResult results[PEG_VALIDATE_VARIANTS];
  ErrorStack *error_stack = error_stack_create();
  printf("  position %d (%d in the bag):", position_idx, bag_size);
  for (int variant = 0; variant < PEG_VALIDATE_VARIANTS; variant++) {
    if (variant == PEG_VALIDATE_FULL_WINDOW && !full_window) {
      continue;
    }
    PegArgs args;
    config_fill_peg_args(config, &args);
    peg_validate_set_variant(&args, (peg_validate_variant_t)variant);
    Timer timer;
    ctimer_start(&timer);
    peg_solve(&args, &results[variant], error_stack);
    assert(error_stack_is_empty(error_stack));
    printf(" %s %.1fs", peg_validate_names[variant],
           ctimer_elapsed_seconds(&timer));
    (void)fflush(stdout);
  }
  printf("\n");
  error_stack_destroy(error_stack);

  int failures = 0;
  const PegResult *base = &results[PEG_VALIDATE_DEFAULT];
  const PegResult *no_cutoff = &results[PEG_VALIDATE_NO_CUTOFF];
  const PegResult *full_window_result = &results[PEG_VALIDATE_FULL_WINDOW];
  const PegResult *rational = &results[PEG_VALIDATE_RATIONAL];

  // 1. Early cutoff.
  const bool same_best =
      compare_moves_without_equity(&base->best_move, &no_cutoff->best_move,
                                   true) == -1;
  if (!peg_validate_same_win(base->best_win, no_cutoff->best_win)) {
    printf("    DIFFERS: cutoff best ");
    peg_validate_print_move(game, &base->best_move);
    printf(" %.4f, without cutoff ", base->best_win);
    peg_validate_print_move(game, &no_cutoff->best_move);
    printf(" %.4f\n", no_cutoff->best_win);
    failures++;
  } else {
    printf("    cutoff: %s best, %.4f\n", same_best ? "same" : "tied",
           base->best_win);
  }

  // 2. First-win against full-window verdicts, for plays that empty the bag.
  int compared = 0;
  for (int cand_idx = 0; full_window && cand_idx < no_cutoff->n_top_cands;
       cand_idx++) {
    const PegRankedCand *first_win = &no_cutoff->top_cands[cand_idx];
    if (!peg_move_empties_bag(&first_win->move, bag_size)) {
      continue;
    }
    const PegRankedCand *full =
        peg_validate_find(full_window_result, &first_win->move);
    assert(full != NULL);
    compared++;
    if (first_win->win_count != full->win_count ||
        first_win->tie_count != full->tie_count ||
        first_win->weight_sum != full->weight_sum) {
      printf("    DIFFERS: ");
      peg_validate_print_move(game, &first_win->move);
      printf(" first-win %lld/%lld/%lld, full window %lld/%lld/%lld\n",
             (long long)first_win->win_count, (long long)first_win->tie_count,
             (long long)(first_win->weight_sum - first_win->win_count -
                         first_win->tie_count),
             (long long)full->win_count, (long long)full->tie_count,
             (long long)(full->weight_sum - full->win_count - full->tie_count));
      failures++;
    }
  }
  if (full_window) {
    printf("    first-win against full window: %d plays that empty the bag\n",
           compared);
  }

  // 3. Pessimistic against rational, for plays that leave tiles.
  compared = 0;
  for (int cand_idx = 0; cand_idx < no_cutoff->n_top_cands; cand_idx++) {
    const PegRankedCand *pessimistic = &no_cutoff->top_cands[cand_idx];
    if (peg_move_empties_bag(&pessimistic->move, bag_size)) {
      continue;
    }
    const PegRankedCand *rational_cand =
        peg_validate_find(rational, &pessimistic->move);
    assert(rational_cand != NULL);
    compared++;
    if (pessimistic->win_pct > rational_cand->win_pct + 1e-9) {
      printf("    DIFFERS: ");
      peg_validate_print_move(game, &pessimistic->move);
      printf(" pessimistic %.4f above rational %.4f\n", pessimistic->win_pct,
             rational_cand->win_pct);
      failures++;
    }
  }
  printf("    pessimistic at most rational: %d plays that leave tiles\n",
         compared);

  for (int variant = 0; variant < PEG_VALIDATE_VARIANTS; variant++) {
    if (variant != PEG_VALIDATE_FULL_WINDOW || full_window) {
      peg_result_destroy(&results[variant]);
    }
  }
  return failures;
}

// Validates positions [0, max_positions) of `cgp_file`, with the full-window
// check when `full_window`.
static int peg_validate_file(const char *cgp_file, int max_positions,
                             int num_threads, bool full_window) {
  log_set_level(LOG_FATAL);
  char settings[128];
  (void)snprintf(settings, sizeof(settings),
                 "set -lex CSW24 -threads %d -pegoutcomes false -pegplies 2",
                 num_threads);
  Config *config = config_create_or_die(settings);
  load_and_exec_config_or_die(config, "new");
  FILE *file = fopen_or_die(cgp_file, "re");
  printf("  PEG validation [%s]: up to %d positions, %d threads\n", cgp_file,
         max_positions, num_threads);
  char line[4096];
  int position_idx = 0;
  int failures = 0;
  while (position_idx < max_positions && fgets(line, sizeof(line), file)) {
    if (line[0] == '\n' || line[0] == '\0') {
      continue;
    }
    strip_cgp_opp_rack(line);
    ErrorStack *error_stack = error_stack_create();
    game_load_cgp(config_get_game(config), line, error_stack);
    assert(error_stack_is_empty(error_stack));
    error_stack_destroy(error_stack);
    failures += peg_validate_position(config, position_idx, full_window);
    position_idx++;
  }
  (void)fclose(file);
  config_destroy(config);
  return failures;
}

// About two hours on 12 threads. The third 2-in-the-bag fixture is left
// out: its rational solve ran over 95 minutes.
void test_peg_validate(void) {
  int failures = 0;
  failures +=
      peg_validate_file("notes/peg_positions/random_2peg.txt", 2, 12, false);
  failures +=
      peg_validate_file("notes/peg_positions/random_1peg.txt", 10, 12, true);
  printf("  PEG validation: %d disagreements\n", failures);
  assert(failures == 0);
}
