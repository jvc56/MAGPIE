#include "peg_cascade_bench_test.h"

#include "../src/compat/ctime.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Measures the timed cascade's refinements (PegArgs.first_win_intermediate,
// skip_noncutting_stages, final_stage_cutoff) against the cascade without
// them. Each position of a committed notes/peg_positions fixture, with the
// opponent's rack taken out so the solve sees what a player sees, is solved by
// the peg command's cascade (the default schedule, no time limit) once per
// variant. Prints each variant's seconds and best move and win%; a variant
// should match the base's best move and win% (up to ties) and take less time.
// Every variant sets all three refinements, so the base is the cascade
// without them whatever the defaults. An untimed cascade at 3 or 4 in the bag
// can take an hour or more.
//
// As in peg_speed_bench_test.c there are no environment-variable knobs: edit
// the entry point to sweep other fixtures or counts.

enum {
  PEG_CASCADE_BENCH_VARIANTS = 6,
};

static const char *const peg_cascade_bench_names[] = {
    "base",         "fw-intermediate", "skip-stages",
    "final-cutoff", "all three",       "stride 5/7"};

// Variant 5 samples the scenarios as the nested solve does (1/5 at 3 in the
// bag, 1/7 at 4): the -pegstride-by-bag-size candidate.
static void peg_cascade_bench_set_variant(PegArgs *args, int variant,
                                          int bag_size) {
  args->first_win_intermediate = variant == 1 || variant == 4;
  args->skip_noncutting_stages = variant == 2 || variant == 4;
  args->final_stage_cutoff = variant == 3 || variant == 4;
  if (variant == 5) {
    args->scenario_stride = bag_size >= 4 ? 7 : (bag_size == 3 ? 5 : 1);
  }
}

// Cuts the opponent's rack out of a fixture line ("<board> AB/CD ..." becomes
// "<board> AB/ ...") in place.
static void peg_cascade_bench_strip_opp_rack(char *line) {
  char *space = strchr(line, ' ');
  if (space == NULL) {
    return;
  }
  char *slash = strchr(space + 1, '/');
  char *end = slash ? strchr(slash, ' ') : NULL;
  if (slash == NULL || end == NULL) {
    return;
  }
  memmove(slash + 1, end, strlen(end) + 1);
}

// Solves positions [first, first + max_positions) of `cgp_file` with the
// variants whose bits are set in `variant_mask`.
static void run_peg_cascade_bench(const char *cgp_file, int first,
                                  int max_positions, int num_threads,
                                  int variant_mask) {
  log_set_level(LOG_FATAL);
  char settings[160];
  (void)snprintf(settings, sizeof(settings),
                 "set -lex CSW24 -threads %d -pegoutcomes false -pegtopk "
                 "32,16,8,4,2",
                 num_threads);
  Config *config = config_create_or_die(settings);
  load_and_exec_config_or_die(config, "new");
  FILE *file = fopen_or_die(cgp_file, "re");
  printf("  PEG cascade refinements [%s]: up to %d positions, %d threads\n",
         cgp_file, max_positions, num_threads);
  double totals[PEG_CASCADE_BENCH_VARIANTS] = {0};
  int agree[PEG_CASCADE_BENCH_VARIANTS] = {0};
  char line[4096];
  int index = 0;
  int line_idx = 0;
  while (index < max_positions && fgets(line, sizeof(line), file)) {
    if (line[0] == '\n' || line[0] == '\0') {
      continue;
    }
    if (line_idx++ < first) {
      continue;
    }
    peg_cascade_bench_strip_opp_rack(line);
    ErrorStack *error_stack = error_stack_create();
    game_load_cgp(config_get_game(config), line, error_stack);
    assert(error_stack_is_empty(error_stack));
    Move base_move;
    double base_win = 0;
    const int bag_size = peg_compute_bag_size(config_get_game(config));
    for (int variant = 0; variant < PEG_CASCADE_BENCH_VARIANTS; variant++) {
      // Sampling applies only at 3 or more in the bag.
      if ((variant == 5 && bag_size < 3) || !(variant_mask & (1 << variant))) {
        continue;
      }
      PegArgs args;
      config_fill_peg_args(config, &args);
      peg_cascade_bench_set_variant(&args, variant, bag_size);
      PegResult result;
      Timer timer;
      ctimer_start(&timer);
      peg_solve(&args, &result, error_stack);
      const double seconds = ctimer_elapsed_seconds(&timer);
      assert(error_stack_is_empty(error_stack));
      totals[variant] += seconds;
      StringBuilder *move_sb = string_builder_create();
      string_builder_add_move(move_sb, game_get_board(config_get_game(config)),
                              &result.best_move,
                              game_get_ld(config_get_game(config)), false);
      if (variant == 0) {
        base_move = result.best_move;
        base_win = result.best_win;
      }
      const bool same = compare_moves_without_equity(&result.best_move,
                                                     &base_move, true) == -1;
      const bool same_win = result.best_win - base_win < 1e-9 &&
                            base_win - result.best_win < 1e-9;
      if (same || same_win) {
        agree[variant]++;
      }
      printf("  %4d  %-16s  %8.2fs  %-18s  %6.2f%%%s\n", index,
             peg_cascade_bench_names[variant], seconds,
             string_builder_peek(move_sb), 100.0 * result.best_win,
             same ? "" : (same_win ? "  (tied)" : "  DIFFERS"));
      string_builder_destroy(move_sb);
      peg_result_destroy(&result);
    }
    error_stack_destroy(error_stack);
    index++;
  }
  fclose_or_die(file);
  for (int variant = 0; variant < PEG_CASCADE_BENCH_VARIANTS; variant++) {
    printf("  total %-16s  %8.2fs  agrees %d/%d\n",
           peg_cascade_bench_names[variant], totals[variant], agree[variant],
           index);
  }
  config_destroy(config);
}

// Untimed, a cascade at 2 in the bag takes about three minutes on 12
// threads, and over ten at 4, so the counts here keep a run near an hour.
void test_peg_cascade_bench(void) {
  // Measured 2026-10-26 on 12 threads: first-win at the intermediate stages
  // took 98 s, 311 s against the base's 189 s, 754 s on the first two
  // 2-in-bag positions with the same best move; skipping stages and the
  // final-stage cutoff saved nothing on the first.
  run_peg_cascade_bench("notes/peg_positions/random_2peg.txt", 0, 1, 12,
                        (1 << PEG_CASCADE_BENCH_VARIANTS) - 1);
  run_peg_cascade_bench("notes/peg_positions/random_3peg.txt", 0, 1, 12,
                        (1 << 0) | (1 << 5));
}

// Times first-win endgame solves to the end of the game (25 plies), the
// depth analyze's endgame turns now use, on full-rack endgames from the
// endgame tests, each capped at two minutes. Prints the seconds, the depth
// reached and the result's sign per position.
void test_endgame_depth_bench(void) {
  log_set_level(LOG_FATAL);
  static const char *const positions[] = {
      "4EXODE6/1DOFF1KERATIN1U/1OHO8YEN/1POOJA1B3MEWS/5SQUINTY2A/4RHINO1e3V/"
      "2B4C2R3E/GOAT1D1E2ZIN1d/1URACILS2E4/1PIG1S4T4/2L2R4T4/2L2A1GENII3/"
      "2A2T1L7/5E1A7/5D1M7 AEEIRUW/V 410/409 0 -lex CSW21",
      "5CORDINER2/1G10E2/1O3RECOURED2/1A1B8I1G/1T1AW5L1S1I/1E1PAX4U1T1P/"
      "ZE2WO1NOSTRILS/I2HEAME2E1L1Y/T4N4T4/IF2JO3Ki4/1O2AN3NU4/FuDGY4AMI3/"
      "ET7R1N3/EH9I3/B9QADIS AAELOUV/V 392/466 0 -lex CSW21",
      "5LEX1AFFORD/3SNOWIER1Y3/2V8T3/1DO6J1T3/1AG6OKE3/1U7YE3N/1B8T3E/"
      "ERICA2GARTH2V/1I2MIAOW1L3U/PE6AZIONES/OS8n4/L6DINGBATS/I14/COULEE9/"
      "E3HARN7 ADEIIU?/MNPQRT 369/399 0 -lex CSW21",
      "5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/3QUOIT1PINNER/"
      "4RENEGADE2P AIINOOU/CDIOST? 392/450 0 -lex CSW24",
      "5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/3QUOIT1PINNER/"
      "4RENEGADE2P CDIOST?/AIINOOU 450/392 0 -lex CSW21",
  };
  Config *config = config_create_or_die("set -threads 12");
  printf("  first-win endgames to 25 plies, two-minute cap\n");
  for (int pos_idx = 0;
       pos_idx < (int)(sizeof(positions) / sizeof(positions[0])); pos_idx++) {
    char *command = get_formatted_string("cgp %s", positions[pos_idx]);
    load_and_exec_config_or_die(config, command);
    free(command);
    EndgameArgs args;
    config_fill_endgame_args(config, &args);
    args.plies = 25;
    args.first_win = true;
    args.soft_time_limit = 120.0;
    args.hard_time_limit = 120.0;
    EndgameResults *results = endgame_results_create();
    EndgameCtx *ctx = NULL;
    ErrorStack *error_stack = error_stack_create();
    Timer timer;
    ctimer_start(&timer);
    endgame_solve(&ctx, &args, results, error_stack);
    const double seconds = ctimer_elapsed_seconds(&timer);
    const int spread = endgame_results_get_spread(results, ENDGAME_RESULT_BEST,
                                                  config_get_game(config));
    printf("  %d  %8.2fs  depth %2d  result %+d\n", pos_idx, seconds,
           endgame_results_get_depth(results, ENDGAME_RESULT_BEST),
           (spread > 0) - (spread < 0));
    error_stack_destroy(error_stack);
    endgame_ctx_destroy(ctx);
    endgame_results_destroy(results);
  }
  config_destroy(config);
}
