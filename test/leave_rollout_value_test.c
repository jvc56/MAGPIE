#include "leave_rollout_value_test.h"

#include "../src/def/game_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "pat_move_choice_test.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// Does KLV leave value add anything by steering a Monte Carlo rollout's own
// forward-play plies, beyond whatever it already contributed to picking the
// candidates the rollout is handed? Same question shape as
// pat_rollout_value_test.c's PAT experiment, but for leave instead of PAT,
// and deliberately kept as separate code/log files -- this is a different
// comparison (leave, not PAT), reached by a cheaper oracle, and should not
// share state, constants, or a log file with that experiment.
//
// Player A: the standard, unmodified rollout as it exists in production --
// no toggles set, real leave value throughout. Player B: the SAME frozen
// top-K candidate list (generated once, normally -- real KLV leave value
// and whatever PAT config production uses, exactly like the PAT
// experiment's candidate-freezing), but with leave value forced to zero
// during the rollout's own forward-play plies only (see
// player_set_rollout_zero_leave in player.h for the mechanism and why it
// is a KLV swap rather than a movegen flag). This file does NOT set
// rollout_disable_pat and does NOT carry over pat_rollout_value_test's
// custom mover-exposure PAT term (that lived entirely in
// src/impl/random_variable.c and has been reverted -- see
// notes/pat_experiment_archive/mover_exposure_pat_term.patch for that
// experiment's own record) -- this test exercises the vanilla production
// rollout mechanism plus exactly one new toggle.
//
// On a disagreement, both candidates are scored by the same on-disagreement
// nested-sim oracle pattern as the PAT experiment (still PAT-free -- see
// leave_nested_sim_config_create's assertions -- and using NORMAL,
// non-zeroed leave value for its own scoring regardless of which rollout
// condition produced the disputed candidates, since only the ROLLOUT
// POLICY differs between A/B here, not how the oracle judges the
// outcome), but at a shallower depth: LEAVE_NESTED_SIM_REFERENCE_PLIES=2
// outer continuation plies (unchanged) each resolved by an
// LEAVE_NESTED_SIM_PLIES=2 inner mini-sim (down from 4) -- a deliberately
// cheap, lower-precision oracle for this comparison; see the constants
// below for why 8 candidates / 75 iterations still make sense at that
// depth.
//
// Interleaved/incremental like pat_rollout_value_test.c's accumulate mode:
// each disagreement is scored immediately as found and logged before
// moving to the next position, so this is interruptible at any point with
// no found-but-unscored backlog. No time budget this time (none was
// specified) -- this runs until stopped externally, printing an hourly
// checkpoint with the running pooled aggregate in the meantime.
#define LEAVE_ROLLOUT_VALUE_LEXICON "CSW21"
#define LEAVE_ROLLOUT_VALUE_NUM_CANDIDATES 12
#define LEAVE_ROLLOUT_VALUE_PLIES 4
#define LEAVE_ROLLOUT_VALUE_ITERATIONS_PER_CANDIDATE 400
#define LEAVE_ROLLOUT_VALUE_THREADS 10
#define LEAVE_ROLLOUT_VALUE_NUM_WORLDS PAT_MOVE_CHOICE_DEFAULT_WORLDS
#define LEAVE_ROLLOUT_VALUE_POSITION_SEED_BASE 5200000000ULL
// A base distinct from pat_rollout_value_test.c's 5100000000-series (its
// own PAT experiment's seeds), so the two experiments' positions can never
// collide even if their logs are ever compared side by side.
#define LEAVE_ROLLOUT_VALUE_SIM_SEED_OFFSET 111000000ULL
#define LEAVE_ROLLOUT_VALUE_WORLD_SEED_OFFSET 500000000ULL
#define LEAVE_ROLLOUT_VALUE_CHECKPOINT_INTERVAL_SECONDS 3600.0

// Same validated utility blend pat_rollout_value_test.c established
// (config.c's own default: uwin=1.0, uspread=0.5, uspreadscale=100 --
// see that file's comment for the full derivation), applied here too for
// consistency, on both the outer round-robin config and the nested-sim
// oracle's own config. Named distinctly (LEAVE_ prefix) from that file's
// identically-valued macros so the two translation units never collide if
// ever included together, though as separate .c files they wouldn't
// anyway.
#define LEAVE_UTIL_W_WINPCT 1.0
#define LEAVE_UTIL_W_SPREAD 0.5
#define LEAVE_UTIL_SPREAD_SCALE 100.0

#define LEAVE_ROLLOUT_VALUE_SCORED_LOG_PATH                                    \
  "test/leave_rollout_value_scored_disagreements.log"

// ---------------------------------------------------------------------
// Nested-sim oracle: same design as pat_rollout_value_test.c's (PAT-free,
// normal leave value), duplicated here (not shared/imported) per this
// file's "clearly separate" mandate, with its own (shallower) depth
// constants.
//
// COST/DEPTH. 8 candidates and 75 iterations are unchanged from the PAT
// experiment's oracle -- still enough breadth and volume to be a real
// (if deliberately cheap) simulation rather than a single sample even at
// 2 plies; there is no reason a shallower inner sim needs MORE breadth or
// volume to stay sane, so the only depth change is LEAVE_NESTED_SIM_PLIES
// itself, 4 -> 2. LEAVE_NESTED_SIM_REFERENCE_PLIES (the outer continuation)
// stays at 2, unchanged. This oracle is explicitly a fast, low-precision
// sanity check for this comparison, not a precision instrument -- per
// the request, speed matters here, not inner-sim variance.
#define LEAVE_NESTED_SIM_NUM_CANDIDATES 8
#define LEAVE_NESTED_SIM_ITERATIONS_PER_CANDIDATE 75
#define LEAVE_NESTED_SIM_PLIES 2
#define LEAVE_NESTED_SIM_THREADS 4
#define LEAVE_NESTED_SIM_SEED_OFFSET 900000000ULL
#define LEAVE_NESTED_SIM_REFERENCE_PLIES 2

static Config *leave_nested_sim_config_create(void) {
  char *set_cmd = get_formatted_string(
      "set -lex %s -s1 equity -s2 equity -r1 all -r2 all -numplays %d "
      "-plies %d -threads %d -iter %d -sr rr -scond none -threshold none "
      "-sinfer false -uwin %f -uspread %f -uspreadscale %f",
      LEAVE_ROLLOUT_VALUE_LEXICON, LEAVE_NESTED_SIM_NUM_CANDIDATES,
      LEAVE_NESTED_SIM_PLIES, LEAVE_NESTED_SIM_THREADS,
      LEAVE_NESTED_SIM_NUM_CANDIDATES *
          LEAVE_NESTED_SIM_ITERATIONS_PER_CANDIDATE,
      LEAVE_UTIL_W_WINPCT, LEAVE_UTIL_W_SPREAD, LEAVE_UTIL_SPREAD_SCALE);
  Config *nested_config = config_create_or_die(set_cmd);
  free(set_cmd);
  load_and_exec_config_or_die(
      nested_config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  // PAT-neutrality, confirmed rather than assumed (same rigor as
  // pat_rollout_value_test.c's identical check): no "-pat" token is ever
  // passed to this config, and players_data.c explicitly nulls every
  // player's PAT slot at creation until an opt-in -pat flag sets it.
  assert(player_get_pat(game_get_player(config_get_game(nested_config), 0)) ==
         NULL);
  assert(player_get_pat(game_get_player(config_get_game(nested_config), 1)) ==
         NULL);
  if (player_get_pat(game_get_player(config_get_game(nested_config), 0)) !=
          NULL ||
      player_get_pat(game_get_player(config_get_game(nested_config), 1)) !=
          NULL) {
    fprintf(stderr, "[leave-oracle-verify] FATAL: nested_config has PAT "
                    "loaded, expected none\n");
    abort();
  }
  // This oracle uses NORMAL (non-zeroed) leave value: rollout_zero_leave
  // defaults to false and is never set on these players.
  const double actual_uwin = config_get_utility_w_winpct(nested_config);
  const double actual_uspread = config_get_utility_w_spread(nested_config);
  const double actual_uscale = config_get_utility_spread_scale(nested_config);
  fprintf(stderr,
          "[leave-oracle-verify] nested_config: uwin=%.4f uspread=%.4f "
          "uspreadscale=%.4f\n",
          actual_uwin, actual_uspread, actual_uscale);
  if (actual_uwin != LEAVE_UTIL_W_WINPCT ||
      actual_uspread != LEAVE_UTIL_W_SPREAD ||
      actual_uscale != LEAVE_UTIL_SPREAD_SCALE) {
    fprintf(stderr, "[leave-oracle-verify] FATAL: nested_config utility "
                    "weights did not take effect as requested\n");
    abort();
  }
  return nested_config;
}

static void leave_nested_sim_pick_move(Config *nested_config,
                                       const Game *rollout_game, uint64_t seed,
                                       Move *move_out) {
  char *cgp = game_get_cgp(rollout_game, /*write_player_on_turn_first=*/true);
  char *cgp_cmd = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(nested_config, cgp_cmd);
  free(cgp_cmd);
  free(cgp);
  char *seed_cmd =
      get_formatted_string("set -seed %llu", (unsigned long long)seed);
  load_and_exec_config_or_die(nested_config, seed_cmd);
  free(seed_cmd);
  load_and_exec_config_or_die(nested_config, "gen");
  MoveList *nested_move_list = config_get_move_list(nested_config);
  if (move_list_get_count(nested_move_list) <= 1) {
    move_copy(move_out, move_list_get_move(nested_move_list, 0));
    return;
  }
  SimResults *nested_sim_results = config_get_sim_results(nested_config);
  const error_code_t status = config_simulate_and_return_status(
      nested_config, NULL, NULL, nested_sim_results);
  if (status != ERROR_STATUS_SUCCESS) {
    move_copy(move_out, move_list_get_move(nested_move_list, 0));
    return;
  }
  move_copy(move_out, sim_results_get_best_move(nested_sim_results));
}

static double leave_nested_sim_reference_value(Config *nested_config,
                                               const Game *game,
                                               const Move *move,
                                               int mover_index,
                                               uint64_t world_seed) {
  Game *rollout_game = game_duplicate(game);
  game_seed(rollout_game, world_seed);
  const int opponent_index = 1 - mover_index;
  Rack last_leave[2];
  bool has_leave[2] = {false, false};
  for (int p = 0; p < 2; p++) {
    rack_set_dist_size(&last_leave[p], ld_get_size(game_get_ld(game)));
    rack_reset(&last_leave[p]);
  }
  play_move(move, rollout_game, &last_leave[mover_index]);
  has_leave[mover_index] = true;
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
    set_random_rack(rollout_game, opponent_index, NULL);
  }
  Move reply;
  for (int ply = 0; ply < LEAVE_NESTED_SIM_REFERENCE_PLIES; ply++) {
    if (game_get_game_end_reason(rollout_game) != GAME_END_REASON_NONE) {
      break;
    }
    const int on_turn = game_get_player_on_turn_index(rollout_game);
    const uint64_t nested_seed =
        world_seed + LEAVE_NESTED_SIM_SEED_OFFSET + (uint64_t)ply * 7919ULL;
    leave_nested_sim_pick_move(nested_config, rollout_game, nested_seed,
                               &reply);
    play_move(&reply, rollout_game, &last_leave[on_turn]);
    has_leave[on_turn] = true;
  }
  const Player *mover = game_get_player(rollout_game, mover_index);
  const Player *opponent = game_get_player(rollout_game, opponent_index);
  double value =
      equity_to_double(player_get_score(mover) - player_get_score(opponent));
  if (game_get_game_end_reason(rollout_game) == GAME_END_REASON_NONE) {
    if (has_leave[mover_index]) {
      value += equity_to_double(
          klv_get_leave_value(player_get_klv(mover), &last_leave[mover_index]));
    }
    if (has_leave[opponent_index]) {
      value -= equity_to_double(klv_get_leave_value(
          player_get_klv(opponent), &last_leave[opponent_index]));
    }
  }
  game_destroy(rollout_game);
  return value;
}

static void leave_nested_sim_reference_values(
    Config *nested_config, const Game *game, const Move *move, int mover_index,
    uint64_t base_seed, int num_worlds, double *values_out) {
  for (int world = 0; world < num_worlds; world++) {
    const uint64_t world_seed = base_seed + (uint64_t)world * 1000003ULL;
    values_out[world] = leave_nested_sim_reference_value(
        nested_config, game, move, mover_index, world_seed);
  }
}

static void
log_leave_scored_disagreement(uint64_t position_seed, const char *cgp,
                              const Move *move_a, const Move *move_b,
                              const Board *board, const LetterDistribution *ld,
                              double nested_sim, double nested_sim_var) {
  FILE *log_file = fopen(LEAVE_ROLLOUT_VALUE_SCORED_LOG_PATH, "a");
  if (!log_file) {
    return;
  }
  StringBuilder *a_sb = string_builder_create();
  string_builder_add_move(a_sb, board, move_a, ld, true);
  char *a_str = string_builder_dump(a_sb, NULL);
  string_builder_destroy(a_sb);
  StringBuilder *b_sb = string_builder_create();
  string_builder_add_move(b_sb, board, move_b, ld, true);
  char *b_str = string_builder_dump(b_sb, NULL);
  string_builder_destroy(b_sb);
  fprintf(log_file,
          "seed=%llu\tcgp=%s\tmove_a_real_leave=%s\tmove_b_zeroed_leave=%s\t"
          "nested_sim=%.6f\tnested_sim_var=%.6f\n",
          (unsigned long long)position_seed, cgp, a_str, b_str, nested_sim,
          nested_sim_var);
  fclose(log_file);
  free(a_str);
  free(b_str);
}

static double elapsed_seconds_since2(const struct timespec *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)(now.tv_sec - start->tv_sec) +
         (double)(now.tv_nsec - start->tv_nsec) / 1.0e9;
}

void test_leave_rollout_value_accumulate(void) {
  Config *config = pat_move_choice_config_create();

  char *wmp_path =
      get_formatted_string("./data/lexica/%s.wmp", LEAVE_ROLLOUT_VALUE_LEXICON);
  char *rit_path =
      get_formatted_string("./data/lexica/%s.rit", LEAVE_ROLLOUT_VALUE_LEXICON);
  char *wit_path =
      get_formatted_string("./data/lexica/%s.wit", LEAVE_ROLLOUT_VALUE_LEXICON);
  const bool have_wmp = access(wmp_path, R_OK) == 0;
  const bool have_rit = access(rit_path, R_OK) == 0;
  const bool have_wit = access(wit_path, R_OK) == 0;
  free(wmp_path);
  free(rit_path);
  free(wit_path);
  char *set_cmd = get_formatted_string(
      "set -wmp %s %s %s -numplays %d -plies %d -threads %d -iter %d -sr rr "
      "-scond none -threshold none -uwin %f -uspread %f -uspreadscale %f",
      have_wmp ? "true" : "false", have_rit ? "-rit true -ritmmap true" : "",
      have_wit ? "-wit true" : "", LEAVE_ROLLOUT_VALUE_NUM_CANDIDATES,
      LEAVE_ROLLOUT_VALUE_PLIES, LEAVE_ROLLOUT_VALUE_THREADS,
      LEAVE_ROLLOUT_VALUE_NUM_CANDIDATES *
          LEAVE_ROLLOUT_VALUE_ITERATIONS_PER_CANDIDATE,
      LEAVE_UTIL_W_WINPCT, LEAVE_UTIL_W_SPREAD, LEAVE_UTIL_SPREAD_SCALE);
  load_and_exec_config_or_die(config, set_cmd);
  free(set_cmd);
  {
    const double actual_uwin = config_get_utility_w_winpct(config);
    const double actual_uspread = config_get_utility_w_spread(config);
    const double actual_uscale = config_get_utility_spread_scale(config);
    fprintf(stderr,
            "[leave-utility-verify] outer config: uwin=%.4f uspread=%.4f "
            "uspreadscale=%.4f\n",
            actual_uwin, actual_uspread, actual_uscale);
    if (actual_uwin != LEAVE_UTIL_W_WINPCT ||
        actual_uspread != LEAVE_UTIL_W_SPREAD ||
        actual_uscale != LEAVE_UTIL_SPREAD_SCALE) {
      fprintf(stderr, "[leave-utility-verify] FATAL: outer config utility "
                      "weights did not take effect as requested\n");
      abort();
    }
  }

  Game *game = config_get_game(config);
  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  SimResults *sim_results = config_get_sim_results(config);
  MoveList *setup_move_list = move_list_create(1);

  // The all-zero KLV player_set_rollout_zero_leave's flag swaps in: built
  // once from each player's own real KLV (same kwg, same leave count, all
  // values zeroed via klv_set_all_leave_values_to_zero), then attached via
  // player_set_rollout_zero_klv. Since both players share the same
  // lexicon/KLV here, one zero KLV suffices for both.
  const KLV *real_klv0 = player_get_klv(player0);
  assert(real_klv0);
  KLV *zero_klv = klv_create_zeroed_from_kwg(
      (KWG *)klv_get_kwg(real_klv0), (int)klv_get_number_of_leaves(real_klv0),
      "leave_rollout_value_zero_klv");
  player_set_rollout_zero_klv(player0, zero_klv);
  player_set_rollout_zero_klv(player1, zero_klv);

  Config *nested_config = leave_nested_sim_config_create();

  double values_a[LEAVE_ROLLOUT_VALUE_NUM_WORLDS];
  double values_b[LEAVE_ROLLOUT_VALUE_NUM_WORLDS];

  long num_positions_considered = 0;
  long num_disagreements = 0;
  double sum_means = 0.0;
  double sum_means_sq = 0.0;
  double sum_within = 0.0;
  double nested_sim_elapsed_seconds = 0.0;

  struct timespec run_start;
  clock_gettime(CLOCK_MONOTONIC, &run_start);
  struct timespec last_checkpoint = run_start;

  printf("[leave-accumulate] no time budget (runs until stopped "
         "externally), checkpoint every %.0f minutes, seeds starting at "
         "%llu, scored results -> %s\n",
         LEAVE_ROLLOUT_VALUE_CHECKPOINT_INTERVAL_SECONDS / 60.0,
         (unsigned long long)LEAVE_ROLLOUT_VALUE_POSITION_SEED_BASE,
         LEAVE_ROLLOUT_VALUE_SCORED_LOG_PATH);
  fflush(stdout);

  for (uint64_t attempt = 0;; attempt++) {
    if (elapsed_seconds_since2(&last_checkpoint) >=
        LEAVE_ROLLOUT_VALUE_CHECKPOINT_INTERVAL_SECONDS) {
      clock_gettime(CLOCK_MONOTONIC, &last_checkpoint);
      const double total_elapsed = elapsed_seconds_since2(&run_start);
      printf("\n[leave-accumulate checkpoint] elapsed %.2fh: %ld positions "
             "considered, %ld disagreements (%.2f%%)\n",
             total_elapsed / 3600.0, num_positions_considered,
             num_disagreements,
             num_positions_considered > 0 ? 100.0 * (double)num_disagreements /
                                                (double)num_positions_considered
                                          : 0.0);
      if (num_disagreements > 1) {
        const long n = num_disagreements;
        const double mean = sum_means / (double)n;
        const double var_means = (sum_means_sq / (double)n - mean * mean) *
                                 ((double)n / (double)(n - 1));
        const double se = sqrt(var_means / (double)n);
        printf("[leave-accumulate checkpoint] pooled nested-sim-scored "
               "paired effect (zeroed-leave-in-rollout minus real): n=%ld "
               "mean=%.4f SE=%.4f 95%%CI=[%.4f, %.4f]\n",
               n, mean, se, mean - 1.96 * se, mean + 1.96 * se);
        const double within = sum_within / (double)n;
        const double between =
            var_means - within / LEAVE_ROLLOUT_VALUE_NUM_WORLDS;
        printf("[leave-accumulate checkpoint] variance decomposition: "
               "within-position %.2f, between-position %.2f (R=%d worlds); "
               "shares of Var(mean): between %.1f%%, within %.1f%%\n",
               within, between, LEAVE_ROLLOUT_VALUE_NUM_WORLDS,
               100.0 * between / var_means,
               100.0 * (within / LEAVE_ROLLOUT_VALUE_NUM_WORLDS) / var_means);
      }
      printf("[leave-accumulate checkpoint] nested-sim scoring: %.1fs "
             "total, %.2fs/disagreement so far\n\n",
             nested_sim_elapsed_seconds,
             num_disagreements > 0
                 ? nested_sim_elapsed_seconds / (double)num_disagreements
                 : 0.0);
      fflush(stdout);
    }

    const uint64_t position_seed =
        LEAVE_ROLLOUT_VALUE_POSITION_SEED_BASE + attempt;
    game_reset(game);
    game_seed(game, position_seed);
    draw_starting_racks(game);
    player_set_rollout_zero_leave(player0, false);
    player_set_rollout_zero_leave(player1, false);
    const int target_bag =
        PAT_MOVE_CHOICE_DEFAULT_BAG_LO +
        (int)(position_seed % (uint64_t)(PAT_MOVE_CHOICE_DEFAULT_BAG_HI -
                                         PAT_MOVE_CHOICE_DEFAULT_BAG_LO + 1));
    bool position_ok = true;
    while (bag_get_letters(game_get_bag(game)) > target_bag) {
      const Move *setup_move = get_top_equity_move(game, setup_move_list);
      play_move(setup_move, game, NULL);
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        position_ok = false;
        break;
      }
    }
    if (!position_ok || bag_get_letters(game_get_bag(game)) == 0) {
      continue;
    }
    const Board *board = game_get_board(game);
    if (board_get_transposed(board) || !board_get_cross_sets_valid(board)) {
      continue;
    }
    num_positions_considered++;

    // Step: freeze the top-K static-equity candidates once (real leave,
    // whatever PAT config production uses). Both conditions below
    // round-robin simulate this exact list; neither regenerates it.
    load_and_exec_config_or_die(config, "gen");
    if (move_list_get_count(config_get_move_list(config)) < 2) {
      continue;
    }

    char *seed_cmd = get_formatted_string(
        "set -seed %llu",
        (unsigned long long)(position_seed +
                             LEAVE_ROLLOUT_VALUE_SIM_SEED_OFFSET));
    load_and_exec_config_or_die(config, seed_cmd);
    free(seed_cmd);

    // Condition A: standard production rollout, real leave value
    // throughout (rollout_zero_leave is false, its default -- no toggle
    // touched at all).
    const error_code_t status_a =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    if (status_a != ERROR_STATUS_SUCCESS) {
      continue;
    }
    Move move_a;
    move_copy(&move_a, sim_results_get_best_move(sim_results));

    // Condition B: leave value forced to zero for the rollout's own
    // forward-play plies only; the frozen candidate list above is
    // untouched by this flag.
    player_set_rollout_zero_leave(player0, true);
    player_set_rollout_zero_leave(player1, true);
    const error_code_t status_b =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    player_set_rollout_zero_leave(player0, false);
    player_set_rollout_zero_leave(player1, false);
    if (status_b != ERROR_STATUS_SUCCESS) {
      continue;
    }
    Move move_b;
    move_copy(&move_b, sim_results_get_best_move(sim_results));

    if (move_get_type(&move_a) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        move_get_type(&move_b) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    if (compare_moves_without_equity(&move_a, &move_b, true) == -1) {
      continue;
    }
    num_disagreements++;

    // Inline oracle scoring, right here (interleaved/incremental): `game`
    // is still exactly at the disagreement position (nothing below
    // mutates it -- leave_nested_sim_reference_value(s) duplicates it
    // internally), so no reload is needed.
    const int mover_index = game_get_player_on_turn_index(game);
    const uint64_t world_seed =
        position_seed + LEAVE_ROLLOUT_VALUE_WORLD_SEED_OFFSET;

    struct timespec oracle_start;
    struct timespec oracle_end;
    clock_gettime(CLOCK_MONOTONIC, &oracle_start);
    leave_nested_sim_reference_values(nested_config, game, &move_a, mover_index,
                                      world_seed,
                                      LEAVE_ROLLOUT_VALUE_NUM_WORLDS, values_a);
    leave_nested_sim_reference_values(nested_config, game, &move_b, mover_index,
                                      world_seed,
                                      LEAVE_ROLLOUT_VALUE_NUM_WORLDS, values_b);
    clock_gettime(CLOCK_MONOTONIC, &oracle_end);
    nested_sim_elapsed_seconds +=
        (double)(oracle_end.tv_sec - oracle_start.tv_sec) +
        (double)(oracle_end.tv_nsec - oracle_start.tv_nsec) / 1.0e9;

    double sum_d = 0.0;
    double sum_d_sq = 0.0;
    for (int world = 0; world < LEAVE_ROLLOUT_VALUE_NUM_WORLDS; world++) {
      const double d = values_b[world] - values_a[world];
      sum_d += d;
      sum_d_sq += d * d;
    }
    const double position_mean = sum_d / LEAVE_ROLLOUT_VALUE_NUM_WORLDS;
    const double position_var = (sum_d_sq / LEAVE_ROLLOUT_VALUE_NUM_WORLDS -
                                 position_mean * position_mean) *
                                ((double)LEAVE_ROLLOUT_VALUE_NUM_WORLDS /
                                 (LEAVE_ROLLOUT_VALUE_NUM_WORLDS - 1));
    sum_means += position_mean;
    sum_means_sq += position_mean * position_mean;
    sum_within += position_var;

    char *cgp = game_get_cgp(game, /*write_player_on_turn_first=*/true);
    log_leave_scored_disagreement(position_seed, cgp, &move_a, &move_b, board,
                                  game_get_ld(game), position_mean,
                                  position_var);
    free(cgp);

    printf("  disagreement %ld (seed=%llu, elapsed=%.0fs): nested-sim "
           "(zeroed-minus-real) %.2f\n",
           num_disagreements, (unsigned long long)position_seed,
           elapsed_seconds_since2(&run_start), position_mean);
    fflush(stdout);
  }
}
