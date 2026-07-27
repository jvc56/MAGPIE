#include "klv3_sim_oracle_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/bai_defs.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/klv_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/rack_info_table.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Position-paired KLV2-vs-KLV3 quality benchmark.
//
// At every midgame position, both models get the same short, multithreaded
// two-ply simulation budget and nominate one move.  When they disagree, both
// root moves are evaluated with identical shuffled-unseen seeds by two
// terminal rollout policies:
//
//   1. KLV2 static play to game end.
//   2. KLV3 static play to game end.
//
// The oracle statistic is the paired terminal-utility difference
// (KLV3 nominee minus KLV2 nominee), not either short sim's self-reported
// value.  Reaching a terminal score removes the horizon leave-value choice;
// running both continuation policies exposes, rather than hides, policy
// dependence.  The paired rollout function is deliberately isolated so a
// nested-sim continuation policy can later replace get_top_equity_move and
// estimate a multilevel correction on the same seeds.

enum {
  KLV3_ORACLE_DEFAULT_GAMES = 4,
  KLV3_ORACLE_DEFAULT_SAMPLES = 1000,
  KLV3_ORACLE_DEFAULT_MAX_POSITIONS = 80,
  KLV3_ORACLE_NUM_PLAYS = 15,
  KLV3_ORACLE_SIM_PLIES = 2,
  KLV3_ORACLE_MIN_PLAY_ITERATIONS = 100000,
  KLV3_ORACLE_MAX_PLAYOUT_TURNS = 200,
  KLV3_ORACLE_KLV2_POLICY = 0,
  KLV3_ORACLE_KLV3_POLICY = 1,
  KLV3_ORACLE_ENSEMBLE_POLICY = 2,
  KLV3_ORACLE_NUM_POLICIES = 3,
  KLV3_ORACLE_MAX_ROOT_CANDIDATES = 3,
  KLV3_ORACLE_OUTER_PLIES = 4,
  KLV3_ORACLE_NESTED_PLIES = 2,
};

static const double KLV3_ORACLE_DEFAULT_SECONDS_PER_TURN = 0.1;
static const double KLV3_ORACLE_NESTED_TIE_EPSILON = 1e-12;
static const uint64_t KLV3_ORACLE_BASE_SEED = UINT64_C(0x4b4c56334f524143);
static const uint64_t KLV3_ORACLE_SEED_STRIDE = UINT64_C(0x9e3779b97f4a7c15);

typedef struct NominationContext {
  SimCtx *sim_ctx;
  SimResults *sim_results;
  Rack known_opponent_rack;
} NominationContext;

typedef struct NominationResult {
  Move move;
  uint64_t iterations;
  uint64_t nodes;
  double elapsed_seconds;
  double best_win_pct;
  double best_win_pct_sem;
  double best_equity;
  double best_equity_sem;
  double best_utility;
} NominationResult;

typedef struct NominationAggregate {
  int positions;
  int repetitions;
  int klv2_stable_positions;
  int klv3_stable_positions;
  int stable_model_disagreements;
  int paired_disagreements;
  uint64_t iterations[2];
  double elapsed_seconds[2];
} NominationAggregate;

typedef struct TerminalRolloutWorker {
  const Game *base_game;
  Move klv2_move;
  Move klv3_move;
  int worker_index;
  int num_workers;
  int num_samples;
  uint64_t seed;
  Stat *spread_delta;
  Stat *win_delta;
  Stat *utility_delta;
} TerminalRolloutWorker;

typedef struct TerminalRolloutResult {
  double spread_delta;
  double spread_sem;
  double win_delta;
  double win_sem;
  double utility_delta;
  double utility_sem;
  double elapsed_seconds;
} TerminalRolloutResult;

typedef struct OracleAggregate {
  int positions;
  int disagreements;
  int policy_sign_agreements;
  int policy_sign_disagreements;
  int unresolved_by_policy;
  double klv2_regret[2];
  double klv3_regret[2];
  Stat *spread_delta[KLV3_ORACLE_NUM_POLICIES];
  Stat *win_delta[KLV3_ORACLE_NUM_POLICIES];
  Stat *utility_delta[KLV3_ORACLE_NUM_POLICIES];
} OracleAggregate;

typedef struct NestedPolicyContext {
  MoveList *candidate_moves;
  SimResults *sim_results;
  SimCtx *sim_ctx;
  ThreadControl *thread_control;
  ErrorStack *error_stack;
  Rack known_opponent_rack;
  WinPct *win_pcts;
  int num_threads;
} NestedPolicyContext;

typedef struct NestedOracleCounters {
  uint64_t static_agreements;
  uint64_t nested_disagreements;
  uint64_t nested_ties;
  uint64_t nested_policy_samples;
  uint64_t nested_nodes;
  uint64_t outer_candidate_rollouts;
  uint64_t outer_continuation_nodes;
} NestedOracleCounters;

typedef struct NestedRootStats {
  Move move;
  Stat *utility;
  Stat *win_pct;
  Stat *spread;
} NestedRootStats;

typedef struct NestedCorpusAggregate {
  int positions;
  int model_root_wins[3];
  Stat *klv3_minus_klv2;
  Stat *hybrid_minus_klv2;
  Stat *klv3_minus_hybrid;
  NestedOracleCounters counters;
} NestedCorpusAggregate;

static int env_positive_int(const char *name, int default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  const long parsed = strtol(value, NULL, 10);
  if (parsed <= 0 || parsed > INT32_MAX) {
    log_fatal("%s must be a positive integer", name);
  }
  return (int)parsed;
}

static int env_nonnegative_int(const char *name, int default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  const long parsed = strtol(value, NULL, 10);
  if (parsed < 0 || parsed > INT32_MAX) {
    log_fatal("%s must be a nonnegative integer", name);
  }
  return (int)parsed;
}

static double env_positive_double(const char *name, double default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  const double parsed = strtod(value, NULL);
  if (!(parsed > 0.0) || !isfinite(parsed)) {
    log_fatal("%s must be a positive finite number", name);
  }
  return parsed;
}

static bool env_optional_nonnegative_double(const char *name, double *value) {
  const char *string_value = getenv(name);
  if (string_value == NULL) {
    return false;
  }
  const double parsed = strtod(string_value, NULL);
  if (!(parsed >= 0.0) || !isfinite(parsed)) {
    log_fatal("%s must be a nonnegative finite number", name);
  }
  *value = parsed;
  return true;
}

static bool env_bool(const char *name, bool default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
    return true;
  }
  if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
    return false;
  }
  log_fatal("%s must be true, false, 1, or 0", name);
  return default_value;
}

static const char *env_string(const char *name, const char *default_value) {
  const char *value = getenv(name);
  return value == NULL || value[0] == '\0' ? default_value : value;
}

static Config *create_oracle_config(const char *klv_name, int num_threads,
                                    double seconds_per_turn,
                                    int min_play_iterations,
                                    uint64_t max_iterations,
                                    double fallback_threshold) {
  char command[1024];
  (void)snprintf(
      command, sizeof(command),
      "set -lex CSW24 -k1 %s -k2 %s -wmp true -rit true -ritmmap true "
      "-wit true -winpct winpct -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays %d "
      "-plies %d -tlim %.17g -it %" PRIu64
      " -minplayiterations %d -sinfer false "
      "-threads %d -pfrequency 0 -hr false -savesettings false "
      "-autosavegcg false -fgrequired false -klv3fallback %.17g "
      "-seed %" PRIu64,
      klv_name, klv_name, KLV3_ORACLE_NUM_PLAYS, KLV3_ORACLE_SIM_PLIES,
      seconds_per_turn, max_iterations, min_play_iterations, num_threads,
      fallback_threshold, KLV3_ORACLE_BASE_SEED);
  Config *config = config_create_or_die(command);
  // A set-only config has loaded game data but does not allocate its Game
  // or MoveList until game commands are executed.  The oracle subsequently
  // replaces this throwaway starting position with each trajectory CGP and
  // reuses the allocated MoveList.
  load_and_exec_config_or_die(config, "newgame");
  load_and_exec_config_or_die(config, "rack AEINRST");
  load_and_exec_config_or_die(config, "generate");
  return config;
}

static void load_position(Config *config, const char *cgp) {
  ErrorStack *error_stack = error_stack_create();
  game_load_cgp(config_get_game(config), cgp, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to load generated oracle position");
  }
  error_stack_destroy(error_stack);
}

static void nomination_context_init(NominationContext *context,
                                    const Config *config) {
  context->sim_ctx = NULL;
  context->sim_results = sim_results_create(0.0);
  rack_set_dist_size_and_reset(&context->known_opponent_rack,
                               ld_get_size(config_get_ld(config)));
}

static void nomination_context_destroy(NominationContext *context) {
  sim_ctx_destroy(context->sim_ctx);
  sim_results_destroy(context->sim_results);
}

static NominationResult
nominate_move_with_fixed_samples(Config *config, NominationContext *context,
                                 int samples_per_arm, uint64_t fixed_seed) {
  Game *game = config_get_game(config);
  MoveList *move_list = config_get_move_list(config);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
  assert(move_list_get_count(move_list) > 0);

  NominationResult result = {0};
  if (move_list_get_count(move_list) == 1) {
    move_copy(&result.move, move_list_get_move(move_list, 0));
    return result;
  }

  uint64_t expected_iterations = 0;
  if (samples_per_arm > 0) {
    const int arm_count = move_list_get_count(move_list) < KLV3_ORACLE_NUM_PLAYS
                              ? move_list_get_count(move_list)
                              : KLV3_ORACLE_NUM_PLAYS;
    expected_iterations = (uint64_t)samples_per_arm * (uint64_t)arm_count;
    char fixed_sample_command[256];
    (void)snprintf(fixed_sample_command, sizeof(fixed_sample_command),
                   "set -it %" PRIu64
                   " -minplayiterations %d -tlim 0 -seed %" PRIu64,
                   expected_iterations, samples_per_arm, fixed_seed);
    load_and_exec_config_or_die(config, fixed_sample_command);
  }

  Timer timer;
  ctimer_start(&timer);
  ThreadControl *thread_control = config_get_thread_control(config);
  const bool started =
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  assert(started);
  ErrorStack *error_stack = error_stack_create();
  config_simulate(config, &context->sim_ctx, &context->known_opponent_rack,
                  context->sim_results, NULL, 0, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("short nomination sim failed");
  }
  error_stack_destroy(error_stack);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_FINISHED);
  const int best_move_index =
      sim_results_get_best_move_index(context->sim_results);
  assert(best_move_index >= 0);
  const SimmedPlay *best_simmed_play =
      sim_results_get_simmed_play(context->sim_results, best_move_index);
  const Move *best_move = simmed_play_get_move(best_simmed_play);
  assert(best_move != NULL);
  move_copy(&result.move, best_move);
  result.iterations = sim_results_get_iteration_count(context->sim_results);
  if (expected_iterations > 0 && result.iterations != expected_iterations) {
    log_fatal("fixed-sample nomination completed %" PRIu64
              " iterations; expected %" PRIu64,
              result.iterations, expected_iterations);
  }
  result.nodes = sim_results_get_node_count(context->sim_results);
  result.elapsed_seconds = ctimer_elapsed_seconds(&timer);
  result.best_win_pct =
      stat_get_mean(simmed_play_get_win_pct_stat(best_simmed_play));
  result.best_win_pct_sem =
      stat_get_sem(simmed_play_get_win_pct_stat(best_simmed_play));
  result.best_equity =
      stat_get_mean(simmed_play_get_equity_stat(best_simmed_play));
  result.best_equity_sem =
      stat_get_sem(simmed_play_get_equity_stat(best_simmed_play));
  result.best_utility = sim_results_get_best_move_utility(context->sim_results);
  return result;
}

static NominationResult nominate_move(Config *config,
                                      NominationContext *context) {
  return nominate_move_with_fixed_samples(config, context,
                                          /*samples_per_arm=*/0,
                                          /*fixed_seed=*/0);
}

static bool moves_are_equal(const Move *a, const Move *b);

static bool move_list_contains_move(const MoveList *move_list,
                                    const Move *move) {
  for (int move_index = 0; move_index < move_list_get_count(move_list);
       move_index++) {
    if (moves_are_equal(move_list_get_move(move_list, move_index), move)) {
      return true;
    }
  }
  return false;
}

// Generate each evaluator's top-N static set and retain only the symmetric
// difference. Both source lists have the same capacity and legal move
// universe, so their non-overlap counts must match. Shared candidates carry
// no information about which evaluator contributes better marginal sim arms.
static int build_nonoverlap_candidate_lists(Config *klv2_config,
                                            Config *klv3_config,
                                            MoveList *klv2_only,
                                            MoveList *klv3_only) {
  Config *configs[2] = {klv2_config, klv3_config};
  for (int model = 0; model < 2; model++) {
    const MoveGenArgs gen_args = {
        .game = config_get_game(configs[model]),
        .move_list = config_get_move_list(configs[model]),
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }

  const MoveList *klv2_top = config_get_move_list(klv2_config);
  const MoveList *klv3_top = config_get_move_list(klv3_config);
  const int top_count = move_list_get_count(klv2_top);
  if (top_count != move_list_get_count(klv3_top)) {
    log_fatal("KLV2 and KLV3 candidate selectors produced different list "
              "sizes from the same legal move universe");
  }

  move_list_reset(klv2_only);
  move_list_reset(klv3_only);
  move_list_set_rack(klv2_only, move_list_get_rack(klv2_top));
  move_list_set_rack(klv3_only, move_list_get_rack(klv2_top));
  for (int move_index = 0; move_index < top_count; move_index++) {
    const Move *klv2_move = move_list_get_move(klv2_top, move_index);
    if (!move_list_contains_move(klv3_top, klv2_move)) {
      move_list_add_move(klv2_only, klv2_move);
    }
    const Move *klv3_move = move_list_get_move(klv3_top, move_index);
    if (!move_list_contains_move(klv2_top, klv3_move)) {
      move_list_add_move(klv3_only, klv3_move);
    }
  }
  if (move_list_get_count(klv2_only) != move_list_get_count(klv3_only)) {
    log_fatal("equal-sized top candidate sets must have equal-sized "
              "non-overlap sets");
  }
  return top_count - move_list_get_count(klv2_only);
}

// Simulate a caller-supplied candidate set on the KLV2 game. In particular,
// candidates selected by KLV3 do not bring KLV3 into rollout move selection or
// horizon valuation. The exact minimum equals the total cap, which guarantees
// equal samples per retained arm.
static NominationResult nominate_candidate_list_with_klv2_policy(
    Config *klv2_config, NominationContext *context,
    const MoveList *candidate_moves, int samples_per_arm, uint64_t fixed_seed) {
  const int arm_count = move_list_get_count(candidate_moves);
  assert(arm_count > 0);
  NominationResult result = {0};
  if (arm_count == 1) {
    move_copy(&result.move, move_list_get_move(candidate_moves, 0));
    return result;
  }

  const uint64_t expected_iterations =
      (uint64_t)samples_per_arm * (uint64_t)arm_count;
  rack_set_dist_size_and_reset(&context->known_opponent_rack,
                               ld_get_size(config_get_ld(klv2_config)));
  ThreadControl *thread_control = config_get_thread_control(klv2_config);
  const bool started =
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  assert(started);
  ErrorStack *error_stack = error_stack_create();
  SimArgs sim_args = {0};
  sim_args_fill(KLV3_ORACLE_SIM_PLIES, candidate_moves, arm_count,
                &context->known_opponent_rack, config_get_win_pcts(klv2_config),
                /*inference_results=*/NULL, thread_control,
                config_get_game(klv2_config), /*sim_with_inference=*/false,
                /*use_heat_map=*/false, config_get_num_threads(klv2_config),
                /*print_interval=*/0, /*max_num_display_plays=*/arm_count,
                /*max_num_display_plies=*/KLV3_ORACLE_SIM_PLIES, fixed_seed,
                expected_iterations, samples_per_arm, /*scond=*/0.0,
                BAI_THRESHOLD_NONE, /*time_limit_seconds=*/0.0,
                BAI_SAMPLING_RULE_ROUND_ROBIN, /*cutoff=*/0.0,
                config_get_utility_w_winpct(klv2_config),
                config_get_utility_w_spread(klv2_config),
                config_get_utility_spread_scale(klv2_config),
                /*inference_args=*/NULL, &sim_args);

  Timer timer;
  ctimer_start(&timer);
  simulate(&sim_args, &context->sim_ctx, context->sim_results, error_stack);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_FINISHED);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("KLV candidate-selector nomination sim failed");
  }
  error_stack_destroy(error_stack);

  result.iterations = sim_results_get_iteration_count(context->sim_results);
  if (result.iterations != expected_iterations) {
    log_fatal("candidate-selector nomination completed %" PRIu64
              " iterations; expected %" PRIu64,
              result.iterations, expected_iterations);
  }
  result.nodes = sim_results_get_node_count(context->sim_results);
  result.elapsed_seconds = ctimer_elapsed_seconds(&timer);
  const int best_move_index =
      sim_results_get_best_move_index(context->sim_results);
  assert(best_move_index >= 0);
  const SimmedPlay *best_simmed_play =
      sim_results_get_simmed_play(context->sim_results, best_move_index);
  move_copy(&result.move, simmed_play_get_move(best_simmed_play));
  result.best_win_pct =
      stat_get_mean(simmed_play_get_win_pct_stat(best_simmed_play));
  result.best_win_pct_sem =
      stat_get_sem(simmed_play_get_win_pct_stat(best_simmed_play));
  result.best_equity =
      stat_get_mean(simmed_play_get_equity_stat(best_simmed_play));
  result.best_equity_sem =
      stat_get_sem(simmed_play_get_equity_stat(best_simmed_play));
  result.best_utility = sim_results_get_best_move_utility(context->sim_results);
  return result;
}

static bool moves_are_equal(const Move *a, const Move *b) {
  return compare_moves_without_equity(a, b, true) == -1;
}

// Stable, notation-independent encoding sufficient to reconstruct a Move.
// Equity is retained for provenance even though later oracle evaluation
// should recompute it under the chosen oracle policy.
static void move_to_record_encoding(const Move *move, char *buffer,
                                    size_t buffer_size) {
  int written = snprintf(
      buffer, buffer_size, "%d,%d,%d,%d,%d,%d,%d,%d", (int)move_get_type(move),
      move_get_row_start(move), move_get_col_start(move), move_get_dir(move),
      move_get_tiles_played(move), move_get_tiles_length(move),
      (int)move_get_score(move), (int)move_get_equity(move));
  if (written < 0 || (size_t)written >= buffer_size) {
    log_fatal("could not encode oracle move metadata");
  }
  size_t used = (size_t)written;
  for (int tile_index = 0; tile_index < move_get_tiles_length(move);
       tile_index++) {
    written = snprintf(buffer + used, buffer_size - used, ",%u",
                       (unsigned int)move_get_tile(move, tile_index));
    if (written < 0 || (size_t)written >= buffer_size - used) {
      log_fatal("could not encode oracle move tiles");
    }
    used += (size_t)written;
  }
}

static int count_unique_nomination_moves(const NominationResult *results,
                                         int count) {
  int unique = 0;
  for (int i = 0; i < count; i++) {
    bool seen = false;
    for (int j = 0; j < i; j++) {
      if (moves_are_equal(&results[i].move, &results[j].move)) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      unique++;
    }
  }
  return unique;
}

static void get_context_adjustment_metrics(const Game *game,
                                           Equity *sampled_magnitude,
                                           Equity *exact_range) {
  *sampled_magnitude = 0;
  *exact_range = 0;
  const int player_index = game_get_player_on_turn_index(game);
  const Player *player = game_get_player(game, player_index);
  const Player *opponent = game_get_player(game, 1 - player_index);
  const KLV *klv = player_get_klv(player);
  if (!klv_has_context_model(klv)) {
    return;
  }
  int unseen_counts[MACHINE_LETTER_MAX_VALUE] = {0};
  bag_increment_unseen_count(game_get_bag(game), unseen_counts);
  rack_increment_unseen_count(player_get_rack(opponent), unseen_counts);
  const int tiles_in_bag = bag_get_letters(game_get_bag(game));
  const int unseen_total =
      tiles_in_bag + rack_get_total_letters(player_get_rack(opponent));
  Equity adjustments[KLV3_DRAW_COUNT_HEADS][MACHINE_LETTER_MAX_VALUE];
  *sampled_magnitude = klv3_sample_rack_adjustment_magnitude(
      klv, unseen_counts, unseen_total, player_get_rack(player), tiles_in_bag);
  klv3_compute_rack_tile_adjustments(klv, unseen_counts, unseen_total,
                                     player_get_rack(player), adjustments);
  *exact_range = klv3_get_rack_adjustment_range(player_get_rack(player),
                                                tiles_in_bag, adjustments);
}

static void print_nomination_summary(const NominationAggregate *aggregate) {
  const double baseline_rate =
      aggregate->elapsed_seconds[0] > 0.0
          ? (double)aggregate->iterations[0] / aggregate->elapsed_seconds[0]
          : 0.0;
  const double candidate_rate =
      aggregate->elapsed_seconds[1] > 0.0
          ? (double)aggregate->iterations[1] / aggregate->elapsed_seconds[1]
          : 0.0;
  const int paired_trials = aggregate->positions * aggregate->repetitions;
  printf("KLV3_NOMINATION_SUMMARY positions=%d repetitions=%d "
         "baseline_stable_positions=%d candidate_stable_positions=%d "
         "stable_model_disagreements=%d "
         "paired_disagreements=%d paired_trials=%d "
         "baseline_iterations=%" PRIu64 " baseline_seconds=%.6f "
         "baseline_iters_per_second=%.3f "
         "candidate_iterations=%" PRIu64 " candidate_seconds=%.6f "
         "candidate_iters_per_second=%.3f candidate_speed_ratio=%.6f\n",
         aggregate->positions, aggregate->repetitions,
         aggregate->klv2_stable_positions, aggregate->klv3_stable_positions,
         aggregate->stable_model_disagreements, aggregate->paired_disagreements,
         paired_trials, aggregate->iterations[0], aggregate->elapsed_seconds[0],
         baseline_rate, aggregate->iterations[1], aggregate->elapsed_seconds[1],
         candidate_rate,
         baseline_rate > 0.0 ? candidate_rate / baseline_rate : 0.0);
}

static double terminal_win_value(int spread) {
  if (spread > 0) {
    return 1.0;
  }
  if (spread < 0) {
    return 0.0;
  }
  return 0.5;
}

static int play_terminal_rollout(Game *scratch_game, const Game *base_game,
                                 MoveList *move_list, const Move *root_move,
                                 uint64_t seed) {
  game_copy(scratch_game, base_game);
  game_seed(scratch_game, seed);
  const int initial_player = game_get_player_on_turn_index(scratch_game);
  Rack known_opponent_rack;
  rack_set_dist_size_and_reset(&known_opponent_rack,
                               ld_get_size(game_get_ld(scratch_game)));
  set_random_rack(scratch_game, 1 - initial_player, &known_opponent_rack);
  play_move(root_move, scratch_game, NULL);

  int turns = 0;
  while (!game_over(scratch_game)) {
    if (turns++ >= KLV3_ORACLE_MAX_PLAYOUT_TURNS) {
      log_fatal("terminal oracle exceeded %d continuation turns",
                KLV3_ORACLE_MAX_PLAYOUT_TURNS);
    }
    const Move *move = get_top_equity_move(scratch_game, move_list);
    assert(move != NULL);
    play_move(move, scratch_game, NULL);
  }
  const int initial_score = equity_to_int(
      player_get_score(game_get_player(scratch_game, initial_player)));
  const int opponent_score = equity_to_int(
      player_get_score(game_get_player(scratch_game, 1 - initial_player)));
  return initial_score - opponent_score;
}

static void *terminal_rollout_worker(void *uncasted_worker) {
  TerminalRolloutWorker *worker = (TerminalRolloutWorker *)uncasted_worker;
  Game *scratch_game = game_duplicate(worker->base_game);
  MoveList *move_list = move_list_create(1);
  for (int sample_index = worker->worker_index;
       sample_index < worker->num_samples;
       sample_index += worker->num_workers) {
    const uint64_t seed =
        worker->seed + (uint64_t)sample_index * KLV3_ORACLE_SEED_STRIDE;
    const int klv2_spread = play_terminal_rollout(
        scratch_game, worker->base_game, move_list, &worker->klv2_move, seed);
    const int klv3_spread = play_terminal_rollout(
        scratch_game, worker->base_game, move_list, &worker->klv3_move, seed);
    const double klv2_win = terminal_win_value(klv2_spread);
    const double klv3_win = terminal_win_value(klv3_spread);
    const double klv2_utility = sim_utility_blend(
        klv2_win, int_to_equity(klv2_spread), 1.0, 0.5, 100.0);
    const double klv3_utility = sim_utility_blend(
        klv3_win, int_to_equity(klv3_spread), 1.0, 0.5, 100.0);
    stat_push(worker->spread_delta, (double)(klv3_spread - klv2_spread), 1);
    stat_push(worker->win_delta, klv3_win - klv2_win, 1);
    stat_push(worker->utility_delta, klv3_utility - klv2_utility, 1);
  }
  move_list_destroy(move_list);
  game_destroy(scratch_game);
  return NULL;
}

static TerminalRolloutResult
run_terminal_rollouts(const Game *base_game, const Move *klv2_move,
                      const Move *klv3_move, int num_samples,
                      int requested_threads, uint64_t seed) {
  const int num_threads =
      requested_threads < num_samples ? requested_threads : num_samples;
  TerminalRolloutWorker *workers =
      malloc_or_die(sizeof(*workers) * (size_t)num_threads);
  cpthread_t *worker_ids =
      malloc_or_die(sizeof(*worker_ids) * (size_t)num_threads);
  Stat **spread_stats = malloc_or_die(sizeof(Stat *) * (size_t)num_threads);
  Stat **win_stats = malloc_or_die(sizeof(Stat *) * (size_t)num_threads);
  Stat **utility_stats = malloc_or_die(sizeof(Stat *) * (size_t)num_threads);
  Timer timer;
  ctimer_start(&timer);
  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    spread_stats[thread_index] = stat_create(true);
    win_stats[thread_index] = stat_create(true);
    utility_stats[thread_index] = stat_create(true);
    workers[thread_index] = (TerminalRolloutWorker){
        .base_game = base_game,
        .klv2_move = *klv2_move,
        .klv3_move = *klv3_move,
        .worker_index = thread_index,
        .num_workers = num_threads,
        .num_samples = num_samples,
        .seed = seed,
        .spread_delta = spread_stats[thread_index],
        .win_delta = win_stats[thread_index],
        .utility_delta = utility_stats[thread_index],
    };
    cpthread_create(&worker_ids[thread_index], terminal_rollout_worker,
                    &workers[thread_index]);
  }
  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    cpthread_join(worker_ids[thread_index]);
  }

  Stat *combined_spread = stat_create(true);
  Stat *combined_win = stat_create(true);
  Stat *combined_utility = stat_create(true);
  stats_combine(spread_stats, num_threads, combined_spread);
  stats_combine(win_stats, num_threads, combined_win);
  stats_combine(utility_stats, num_threads, combined_utility);
  const TerminalRolloutResult result = {
      .spread_delta = stat_get_mean(combined_spread),
      .spread_sem = stat_get_sem(combined_spread),
      .win_delta = stat_get_mean(combined_win),
      .win_sem = stat_get_sem(combined_win),
      .utility_delta = stat_get_mean(combined_utility),
      .utility_sem = stat_get_sem(combined_utility),
      .elapsed_seconds = ctimer_elapsed_seconds(&timer),
  };

  for (int thread_index = 0; thread_index < num_threads; thread_index++) {
    stat_destroy(spread_stats[thread_index]);
    stat_destroy(win_stats[thread_index]);
    stat_destroy(utility_stats[thread_index]);
  }
  stat_destroy(combined_spread);
  stat_destroy(combined_win);
  stat_destroy(combined_utility);
  free(spread_stats);
  free(win_stats);
  free(utility_stats);
  free(worker_ids);
  free(workers);
  return result;
}

static void move_to_string(const Game *game, const Move *move, char *buffer,
                           size_t buffer_size) {
  StringBuilder *string_builder = string_builder_create();
  string_builder_add_move(string_builder, game_get_board(game), move,
                          game_get_ld(game), false);
  (void)snprintf(buffer, buffer_size, "%s",
                 string_builder_peek(string_builder));
  string_builder_destroy(string_builder);
}

static OracleAggregate oracle_aggregate_create(void) {
  OracleAggregate aggregate = {0};
  for (int policy = 0; policy < KLV3_ORACLE_NUM_POLICIES; policy++) {
    aggregate.spread_delta[policy] = stat_create(true);
    aggregate.win_delta[policy] = stat_create(true);
    aggregate.utility_delta[policy] = stat_create(true);
  }
  return aggregate;
}

static void oracle_aggregate_destroy(OracleAggregate *aggregate) {
  for (int policy = 0; policy < KLV3_ORACLE_NUM_POLICIES; policy++) {
    stat_destroy(aggregate->spread_delta[policy]);
    stat_destroy(aggregate->win_delta[policy]);
    stat_destroy(aggregate->utility_delta[policy]);
  }
}

static void add_regret(double delta, double *klv2_regret, double *klv3_regret) {
  if (delta > 0.0) {
    *klv2_regret += delta;
  } else {
    *klv3_regret -= delta;
  }
}

static void oracle_aggregate_add(OracleAggregate *aggregate, bool same_move,
                                 const TerminalRolloutResult *klv2_policy,
                                 const TerminalRolloutResult *klv3_policy) {
  aggregate->positions++;
  if (same_move) {
    for (int policy = 0; policy < KLV3_ORACLE_NUM_POLICIES; policy++) {
      stat_push(aggregate->spread_delta[policy], 0.0, 1);
      stat_push(aggregate->win_delta[policy], 0.0, 1);
      stat_push(aggregate->utility_delta[policy], 0.0, 1);
    }
    return;
  }
  aggregate->disagreements++;
  const double spread_deltas[KLV3_ORACLE_NUM_POLICIES] = {
      klv2_policy->spread_delta,
      klv3_policy->spread_delta,
      (klv2_policy->spread_delta + klv3_policy->spread_delta) / 2.0,
  };
  const double win_deltas[KLV3_ORACLE_NUM_POLICIES] = {
      klv2_policy->win_delta,
      klv3_policy->win_delta,
      (klv2_policy->win_delta + klv3_policy->win_delta) / 2.0,
  };
  const double utility_deltas[KLV3_ORACLE_NUM_POLICIES] = {
      klv2_policy->utility_delta,
      klv3_policy->utility_delta,
      (klv2_policy->utility_delta + klv3_policy->utility_delta) / 2.0,
  };
  for (int policy = 0; policy < KLV3_ORACLE_NUM_POLICIES; policy++) {
    stat_push(aggregate->spread_delta[policy], spread_deltas[policy], 1);
    stat_push(aggregate->win_delta[policy], win_deltas[policy], 1);
    stat_push(aggregate->utility_delta[policy], utility_deltas[policy], 1);
  }
  add_regret(utility_deltas[KLV3_ORACLE_KLV2_POLICY],
             &aggregate->klv2_regret[0], &aggregate->klv3_regret[0]);
  add_regret(utility_deltas[KLV3_ORACLE_KLV3_POLICY],
             &aggregate->klv2_regret[1], &aggregate->klv3_regret[1]);
  if (utility_deltas[KLV3_ORACLE_KLV2_POLICY] == 0.0 ||
      utility_deltas[KLV3_ORACLE_KLV3_POLICY] == 0.0) {
    aggregate->unresolved_by_policy++;
  } else if ((utility_deltas[KLV3_ORACLE_KLV2_POLICY] > 0.0) ==
             (utility_deltas[KLV3_ORACLE_KLV3_POLICY] > 0.0)) {
    aggregate->policy_sign_agreements++;
  } else {
    aggregate->policy_sign_disagreements++;
  }
}

static void print_oracle_summary(const OracleAggregate *aggregate) {
  const double disagreement_count =
      aggregate->disagreements > 0 ? (double)aggregate->disagreements : 1.0;
  const double conditional_scale =
      aggregate->disagreements > 0
          ? (double)aggregate->positions / disagreement_count
          : 0.0;
  printf("KLV3_ORACLE_SUMMARY positions=%d disagreements=%d "
         "policy_sign_agree=%d policy_sign_disagree=%d policy_unresolved=%d "
         "klv2_policy_delta=%+.6f klv2_policy_position_sem=%.6f "
         "klv3_policy_delta=%+.6f klv3_policy_position_sem=%.6f "
         "ensemble_delta=%+.6f ensemble_position_sem=%.6f "
         "ensemble_spread_delta=%+.6f "
         "ensemble_spread_disagreement_delta=%+.6f "
         "ensemble_win_delta=%+.6f "
         "ensemble_win_disagreement_delta=%+.6f "
         "klv2_regret_by_policy=%.6f,%.6f "
         "klv3_regret_by_policy=%.6f,%.6f\n",
         aggregate->positions, aggregate->disagreements,
         aggregate->policy_sign_agreements,
         aggregate->policy_sign_disagreements, aggregate->unresolved_by_policy,
         stat_get_mean(aggregate->utility_delta[KLV3_ORACLE_KLV2_POLICY]),
         stat_get_sem(aggregate->utility_delta[KLV3_ORACLE_KLV2_POLICY]),
         stat_get_mean(aggregate->utility_delta[KLV3_ORACLE_KLV3_POLICY]),
         stat_get_sem(aggregate->utility_delta[KLV3_ORACLE_KLV3_POLICY]),
         stat_get_mean(aggregate->utility_delta[KLV3_ORACLE_ENSEMBLE_POLICY]),
         stat_get_sem(aggregate->utility_delta[KLV3_ORACLE_ENSEMBLE_POLICY]),
         stat_get_mean(aggregate->spread_delta[KLV3_ORACLE_ENSEMBLE_POLICY]),
         stat_get_mean(aggregate->spread_delta[KLV3_ORACLE_ENSEMBLE_POLICY]) *
             conditional_scale,
         stat_get_mean(aggregate->win_delta[KLV3_ORACLE_ENSEMBLE_POLICY]),
         stat_get_mean(aggregate->win_delta[KLV3_ORACLE_ENSEMBLE_POLICY]) *
             conditional_scale,
         aggregate->klv2_regret[0] / disagreement_count,
         aggregate->klv2_regret[1] / disagreement_count,
         aggregate->klv3_regret[0] / disagreement_count,
         aggregate->klv3_regret[1] / disagreement_count);
}

static bool parse_record_move(const char *encoding, Move *move) {
  char *copy = string_duplicate(encoding);
  char *saveptr = NULL;
  int values[8 + MOVE_MAX_TILES];
  int count = 0;
  for (char *token = strtok_r(copy, ",", &saveptr); token != NULL;
       token = strtok_r(NULL, ",", &saveptr)) {
    if (count >= (int)(sizeof(values) / sizeof(values[0]))) {
      free(copy);
      return false;
    }
    char *endptr = NULL;
    const long parsed = strtol(token, &endptr, 10);
    if (endptr == token || *endptr != '\0' || parsed < INT32_MIN ||
        parsed > INT32_MAX) {
      free(copy);
      return false;
    }
    values[count++] = (int)parsed;
  }
  free(copy);
  if (count < 8 || values[5] < 0 || values[5] > MOVE_MAX_TILES ||
      count != 8 + values[5]) {
    return false;
  }
  move_set_type(move, (game_event_t)values[0]);
  move_set_row_start(move, values[1]);
  move_set_col_start(move, values[2]);
  move_set_dir(move, values[3]);
  move_set_tiles_played(move, values[4]);
  move_set_tiles_length(move, values[5]);
  move_set_score(move, (Equity)values[6]);
  move_set_equity(move, (Equity)values[7]);
  for (int tile_index = 0; tile_index < values[5]; tile_index++) {
    move_set_tile(move, (MachineLetter)values[8 + tile_index], tile_index);
  }
  return true;
}

static int split_tab_fields(char *line, char **fields, int capacity) {
  int count = 0;
  char *cursor = line;
  while (cursor != NULL && count < capacity) {
    fields[count++] = strsep(&cursor, "\t");
  }
  return count;
}

static void nested_policy_context_init(NestedPolicyContext *context,
                                       const Config *config, int num_threads) {
  context->candidate_moves = move_list_create(2);
  context->sim_results = sim_results_create(0.0);
  context->sim_ctx = NULL;
  context->thread_control = thread_control_create();
  context->error_stack = error_stack_create();
  rack_set_dist_size_and_reset(&context->known_opponent_rack,
                               ld_get_size(config_get_ld(config)));
  context->win_pcts = config_get_win_pcts(config);
  context->num_threads = num_threads;
}

static void nested_policy_context_destroy(NestedPolicyContext *context) {
  error_stack_destroy(context->error_stack);
  thread_control_destroy(context->thread_control);
  sim_ctx_destroy(context->sim_ctx);
  sim_results_destroy(context->sim_results);
  move_list_destroy(context->candidate_moves);
}

static int find_simmed_play_index(const SimResults *sim_results,
                                  const Move *move) {
  for (int play_index = 0;
       play_index < sim_results_get_number_of_plays(sim_results);
       play_index++) {
    if (moves_are_equal(simmed_play_get_move(sim_results_get_simmed_play(
                            sim_results, play_index)),
                        move)) {
      return play_index;
    }
  }
  return -1;
}

// Run one half of a neutral nested adjudication. Each policy gives both
// candidates the same number of round-robin samples and the same seed stream.
// The caller averages this KLV2-policy judgment with an equally sized
// KLV3-policy judgment, so KLV3_ORACLE_NESTED_SAMPLES is the total number of
// samples per candidate across both policies, not per policy.
static void run_nested_policy(NestedPolicyContext *context, const Game *game,
                              const Move *candidate_a, const Move *candidate_b,
                              int samples_per_candidate, uint64_t seed,
                              double utilities[2],
                              NestedOracleCounters *counters) {
  move_list_reset(context->candidate_moves);
  move_list_add_move(context->candidate_moves, candidate_a);
  move_list_add_move(context->candidate_moves, candidate_b);
  rack_set_dist_size_and_reset(&context->known_opponent_rack,
                               ld_get_size(game_get_ld(game)));
  error_stack_reset(context->error_stack);
  (void)thread_control_set_status(context->thread_control,
                                  THREAD_CONTROL_STATUS_STARTED);

  SimArgs sim_args = {0};
  sim_args_fill(KLV3_ORACLE_NESTED_PLIES, context->candidate_moves,
                /*num_plays=*/2, &context->known_opponent_rack,
                context->win_pcts,
                /*inference_results=*/NULL, context->thread_control, game,
                /*sim_with_inference=*/false, /*use_heat_map=*/false,
                context->num_threads, /*print_interval=*/0,
                /*max_num_display_plays=*/2,
                /*max_num_display_plies=*/KLV3_ORACLE_NESTED_PLIES, seed,
                /*max_iterations=*/(uint64_t)samples_per_candidate * 2,
                /*min_play_iterations=*/(uint64_t)samples_per_candidate,
                /*scond=*/0.0, BAI_THRESHOLD_NONE,
                /*time_limit_seconds=*/0.0, BAI_SAMPLING_RULE_ROUND_ROBIN,
                /*cutoff=*/0.0,
                /*utility_w_winpct=*/1.0,
                /*utility_w_spread=*/0.5,
                /*utility_spread_scale=*/100.0,
                /*inference_args=*/NULL, &sim_args);
  simulate(&sim_args, &context->sim_ctx, context->sim_results,
           context->error_stack);
  (void)thread_control_set_status(context->thread_control,
                                  THREAD_CONTROL_STATUS_FINISHED);
  if (!error_stack_is_empty(context->error_stack)) {
    error_stack_print_and_reset(context->error_stack);
    log_fatal("nested KLV oracle simulation failed");
  }
  const int candidate_indices[2] = {
      find_simmed_play_index(context->sim_results, candidate_a),
      find_simmed_play_index(context->sim_results, candidate_b),
  };
  for (int candidate = 0; candidate < 2; candidate++) {
    if (candidate_indices[candidate] < 0) {
      log_fatal("nested KLV oracle lost a fixed candidate");
    }
    const Stat *utility_stat =
        simmed_play_get_utility_stat(sim_results_get_simmed_play(
            context->sim_results, candidate_indices[candidate]));
    if (stat_get_num_samples(utility_stat) != (uint64_t)samples_per_candidate) {
      log_fatal("nested KLV oracle expected %d policy samples per candidate, "
                "got %" PRIu64,
                samples_per_candidate, stat_get_num_samples(utility_stat));
    }
    utilities[candidate] = stat_get_mean(utility_stat);
  }
  counters->nested_policy_samples +=
      sim_results_get_iteration_count(context->sim_results);
  counters->nested_nodes += sim_results_get_node_count(context->sim_results);
}

static Move choose_nested_move(NestedPolicyContext policy_contexts[2],
                               const Game *klv2_game, const Game *klv3_game,
                               const Move *klv2_move, const Move *klv3_move,
                               int nested_samples_per_candidate, uint64_t seed,
                               NestedOracleCounters *counters) {
  if (nested_samples_per_candidate % 2 != 0) {
    log_fatal("KLV3_ORACLE_NESTED_SAMPLES must be even so the two leave "
              "policies receive equal samples");
  }
  if (bag_is_empty(game_get_bag(klv2_game)) ||
      bag_is_empty(game_get_bag(klv3_game))) {
    log_fatal("nested KLV oracle reached an empty bag; use positions with "
              "enough tiles for a midgame four-ply smoke test");
  }
  const int policy_samples_per_candidate = nested_samples_per_candidate / 2;
  double policy_utilities[2][2];
  run_nested_policy(&policy_contexts[KLV3_ORACLE_KLV2_POLICY], klv2_game,
                    klv2_move, klv3_move, policy_samples_per_candidate, seed,
                    policy_utilities[KLV3_ORACLE_KLV2_POLICY], counters);
  run_nested_policy(&policy_contexts[KLV3_ORACLE_KLV3_POLICY], klv3_game,
                    klv2_move, klv3_move, policy_samples_per_candidate, seed,
                    policy_utilities[KLV3_ORACLE_KLV3_POLICY], counters);
  const double utility_a = (policy_utilities[KLV3_ORACLE_KLV2_POLICY][0] +
                            policy_utilities[KLV3_ORACLE_KLV3_POLICY][0]) /
                           2.0;
  const double utility_b = (policy_utilities[KLV3_ORACLE_KLV2_POLICY][1] +
                            policy_utilities[KLV3_ORACLE_KLV3_POLICY][1]) /
                           2.0;
  counters->nested_disagreements++;
  // Parallel workers merge floating-point samples in scheduling order. Treat
  // differences at roundoff scale as a deterministic tie so the outer policy
  // cannot change across otherwise identical multithreaded runs.
  if (fabs(utility_a - utility_b) <= KLV3_ORACLE_NESTED_TIE_EPSILON) {
    counters->nested_ties++;
    return (seed & 1) == 0 ? *klv2_move : *klv3_move;
  }
  return utility_a > utility_b ? *klv2_move : *klv3_move;
}

typedef struct HorizonValue {
  double utility;
  double win_pct;
  double spread;
} HorizonValue;

static HorizonValue get_horizon_value(const Config *config, const Game *game,
                                      int initial_player, Equity leftover,
                                      int num_plies) {
  const Equity spread =
      player_get_score(game_get_player(game, initial_player)) -
      player_get_score(game_get_player(game, 1 - initial_player));
  double win_pct;
  if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
    if (spread > 0) {
      win_pct = 1.0;
    } else if (spread < 0) {
      win_pct = 0.0;
    } else {
      win_pct = 0.5;
    }
  } else {
    int spread_plus_leftover = (int)(equity_to_double(spread + leftover) + 0.5 -
                                     (spread + leftover < 0));
    if (num_plies % 2 == 0) {
      spread_plus_leftover = -spread_plus_leftover;
    }
    const int unseen_tiles = bag_get_letters(game_get_bag(game)) +
                             rack_get_total_letters(player_get_rack(
                                 game_get_player(game, 1 - initial_player)));
    win_pct = win_pct_get(config_get_win_pcts(config), spread_plus_leftover,
                          (unsigned int)unseen_tiles);
    if (num_plies % 2 == 0) {
      win_pct = 1.0 - win_pct;
    }
  }
  return (HorizonValue){
      .utility = sim_utility_blend(win_pct, spread, 1.0, 0.5, 100.0),
      .win_pct = win_pct,
      .spread = equity_to_double(spread),
  };
}

static HorizonValue run_nested_outer_candidate(
    const Config *klv2_config, const Config *klv3_config,
    Game *scratch_games[2], MoveList *static_move_lists[2],
    NestedPolicyContext policy_contexts[2], const Move *root_move,
    int outer_plies, int nested_samples_per_candidate, uint64_t outer_seed,
    uint64_t nested_seed, NestedOracleCounters *counters) {
  const Config *configs[2] = {klv2_config, klv3_config};
  for (int policy = 0; policy < 2; policy++) {
    game_copy(scratch_games[policy], config_get_game(configs[policy]));
    game_seed(scratch_games[policy], outer_seed);
    const int off_turn =
        1 - game_get_player_on_turn_index(scratch_games[policy]);
    Rack empty_known_rack;
    rack_set_dist_size_and_reset(
        &empty_known_rack, ld_get_size(game_get_ld(scratch_games[policy])));
    set_random_rack(scratch_games[policy], off_turn, &empty_known_rack);
  }
  const int initial_player =
      game_get_player_on_turn_index(scratch_games[KLV3_ORACLE_KLV2_POLICY]);
  play_move(root_move, scratch_games[KLV3_ORACLE_KLV2_POLICY], NULL);
  play_move(root_move, scratch_games[KLV3_ORACLE_KLV3_POLICY], NULL);
  counters->outer_candidate_rollouts++;

  Equity leftovers[2] = {0, 0};
  for (int ply = 0; ply < outer_plies; ply++) {
    if (game_over(scratch_games[KLV3_ORACLE_KLV2_POLICY])) {
      break;
    }
    Move policy_moves[2];
    for (int policy = 0; policy < 2; policy++) {
      const Move *top_move =
          get_top_equity_move(scratch_games[policy], static_move_lists[policy]);
      assert(top_move != NULL);
      move_copy(&policy_moves[policy], top_move);
    }
    Move chosen_move;
    if (moves_are_equal(&policy_moves[KLV3_ORACLE_KLV2_POLICY],
                        &policy_moves[KLV3_ORACLE_KLV3_POLICY])) {
      counters->static_agreements++;
      chosen_move = policy_moves[KLV3_ORACLE_KLV2_POLICY];
    } else {
      chosen_move = choose_nested_move(
          policy_contexts, scratch_games[KLV3_ORACLE_KLV2_POLICY],
          scratch_games[KLV3_ORACLE_KLV3_POLICY],
          &policy_moves[KLV3_ORACLE_KLV2_POLICY],
          &policy_moves[KLV3_ORACLE_KLV3_POLICY], nested_samples_per_candidate,
          nested_seed + (uint64_t)(ply + 1) * KLV3_ORACLE_SEED_STRIDE,
          counters);
    }
    for (int policy = 0; policy < 2; policy++) {
      if (ply == outer_plies - 2 || ply == outer_plies - 1) {
        const int player_on_turn =
            game_get_player_on_turn_index(scratch_games[policy]);
        Rack rack;
        rack_copy(&rack, player_get_rack(game_get_player(scratch_games[policy],
                                                         player_on_turn)));
        const Equity leave = get_leave_value_for_move_with_context(
            scratch_games[policy], &chosen_move, &rack);
        leftovers[policy] += player_on_turn == initial_player ? leave : -leave;
      }
      play_move(&chosen_move, scratch_games[policy], NULL);
    }
    counters->outer_continuation_nodes++;
  }
  const HorizonValue policy_values[2] = {
      get_horizon_value(klv2_config, scratch_games[KLV3_ORACLE_KLV2_POLICY],
                        initial_player, leftovers[KLV3_ORACLE_KLV2_POLICY],
                        outer_plies),
      get_horizon_value(klv3_config, scratch_games[KLV3_ORACLE_KLV3_POLICY],
                        initial_player, leftovers[KLV3_ORACLE_KLV3_POLICY],
                        outer_plies),
  };
  return (HorizonValue){
      .utility = (policy_values[0].utility + policy_values[1].utility) / 2.0,
      .win_pct = (policy_values[0].win_pct + policy_values[1].win_pct) / 2.0,
      .spread = (policy_values[0].spread + policy_values[1].spread) / 2.0,
  };
}

static int add_unique_root_move(NestedRootStats roots[], int root_count,
                                const Move *move) {
  for (int root_index = 0; root_index < root_count; root_index++) {
    if (moves_are_equal(&roots[root_index].move, move)) {
      return root_index;
    }
  }
  if (root_count >= KLV3_ORACLE_MAX_ROOT_CANDIDATES) {
    log_fatal("too many unique KLV oracle root candidates");
  }
  roots[root_count] = (NestedRootStats){
      .move = *move,
      .utility = stat_create(true),
      .win_pct = stat_create(true),
      .spread = stat_create(true),
  };
  return root_count;
}

static void nested_root_stats_destroy(NestedRootStats roots[], int root_count) {
  for (int root_index = 0; root_index < root_count; root_index++) {
    stat_destroy(roots[root_index].utility);
    stat_destroy(roots[root_index].win_pct);
    stat_destroy(roots[root_index].spread);
  }
}

typedef struct NestedOuterWorker {
  const Config *klv2_config;
  const Config *klv3_config;
  NestedRootStats roots[KLV3_ORACLE_MAX_ROOT_CANDIDATES];
  Stat *position_deltas[3];
  int root_count;
  int model_root_indices[3];
  int worker_index;
  int num_workers;
  int outer_samples;
  int outer_plies;
  int nested_samples_per_candidate;
  uint64_t position_seed;
  NestedOracleCounters counters;
} NestedOuterWorker;

static void *nested_outer_worker(void *uncasted_worker) {
  NestedOuterWorker *worker = (NestedOuterWorker *)uncasted_worker;
  NestedPolicyContext policy_contexts[2];
  // Parallelize independent outer scenarios. Each nested simulation stays
  // single-threaded, giving it a fixed sample order; this preserves exact
  // reproducibility while still using all requested cores for the position.
  nested_policy_context_init(&policy_contexts[KLV3_ORACLE_KLV2_POLICY],
                             worker->klv2_config, 1);
  nested_policy_context_init(&policy_contexts[KLV3_ORACLE_KLV3_POLICY],
                             worker->klv3_config, 1);
  Game *scratch_games[2] = {
      game_duplicate(config_get_game(worker->klv2_config)),
      game_duplicate(config_get_game(worker->klv3_config)),
  };
  MoveList *static_move_lists[2] = {
      move_list_create(1),
      move_list_create(1),
  };

  for (int sample = worker->worker_index; sample < worker->outer_samples;
       sample += worker->num_workers) {
    HorizonValue root_values[KLV3_ORACLE_MAX_ROOT_CANDIDATES] = {0};
    const uint64_t outer_seed =
        worker->position_seed + (uint64_t)sample * KLV3_ORACLE_SEED_STRIDE;
    const uint64_t nested_seed =
        worker->position_seed ^
        ((uint64_t)(sample + 1) * UINT64_C(0xd1b54a32d192ed03));
    for (int root_index = 0; root_index < worker->root_count; root_index++) {
      root_values[root_index] = run_nested_outer_candidate(
          worker->klv2_config, worker->klv3_config, scratch_games,
          static_move_lists, policy_contexts, &worker->roots[root_index].move,
          worker->outer_plies, worker->nested_samples_per_candidate, outer_seed,
          nested_seed, &worker->counters);
      stat_push(worker->roots[root_index].utility,
                root_values[root_index].utility, 1);
      stat_push(worker->roots[root_index].win_pct,
                root_values[root_index].win_pct, 1);
      stat_push(worker->roots[root_index].spread,
                root_values[root_index].spread, 1);
    }
    const double klv2_utility =
        root_values[worker->model_root_indices[0]].utility;
    const double hybrid_utility =
        root_values[worker->model_root_indices[1]].utility;
    const double klv3_utility =
        root_values[worker->model_root_indices[2]].utility;
    stat_push(worker->position_deltas[0], klv3_utility - klv2_utility, 1);
    stat_push(worker->position_deltas[1], hybrid_utility - klv2_utility, 1);
    stat_push(worker->position_deltas[2], klv3_utility - hybrid_utility, 1);
  }

  for (int policy = 0; policy < 2; policy++) {
    move_list_destroy(static_move_lists[policy]);
    game_destroy(scratch_games[policy]);
    nested_policy_context_destroy(&policy_contexts[policy]);
  }
  return NULL;
}

static void combine_worker_stat(NestedOuterWorker workers[], int num_workers,
                                Stat *(*get_stat)(NestedOuterWorker *, int),
                                int stat_index, Stat *combined) {
  Stat **worker_stats = malloc_or_die(sizeof(Stat *) * (size_t)num_workers);
  for (int worker_index = 0; worker_index < num_workers; worker_index++) {
    worker_stats[worker_index] = get_stat(&workers[worker_index], stat_index);
  }
  stats_combine(worker_stats, num_workers, combined);
  free(worker_stats);
}

static Stat *get_worker_utility(NestedOuterWorker *worker, int root_index) {
  return worker->roots[root_index].utility;
}

static Stat *get_worker_win_pct(NestedOuterWorker *worker, int root_index) {
  return worker->roots[root_index].win_pct;
}

static Stat *get_worker_spread(NestedOuterWorker *worker, int root_index) {
  return worker->roots[root_index].spread;
}

static Stat *get_worker_delta(NestedOuterWorker *worker, int delta_index) {
  return worker->position_deltas[delta_index];
}

static void run_nested_position_samples(
    const Config *klv2_config, const Config *klv3_config,
    NestedRootStats roots[], int root_count, const int model_root_indices[3],
    int outer_samples, int outer_plies, int nested_samples_per_candidate,
    int requested_threads, uint64_t position_seed, Stat *position_deltas[3],
    NestedOracleCounters *counters) {
  const int num_workers =
      requested_threads < outer_samples ? requested_threads : outer_samples;
  NestedOuterWorker *workers =
      malloc_or_die(sizeof(*workers) * (size_t)num_workers);
  cpthread_t *worker_ids =
      malloc_or_die(sizeof(*worker_ids) * (size_t)num_workers);
  for (int worker_index = 0; worker_index < num_workers; worker_index++) {
    workers[worker_index] = (NestedOuterWorker){
        .klv2_config = klv2_config,
        .klv3_config = klv3_config,
        .root_count = root_count,
        .worker_index = worker_index,
        .num_workers = num_workers,
        .outer_samples = outer_samples,
        .outer_plies = outer_plies,
        .nested_samples_per_candidate = nested_samples_per_candidate,
        .position_seed = position_seed,
    };
    for (int model = 0; model < 3; model++) {
      workers[worker_index].model_root_indices[model] =
          model_root_indices[model];
      workers[worker_index].position_deltas[model] = stat_create(true);
    }
    for (int root_index = 0; root_index < root_count; root_index++) {
      workers[worker_index].roots[root_index] = (NestedRootStats){
          .move = roots[root_index].move,
          .utility = stat_create(true),
          .win_pct = stat_create(true),
          .spread = stat_create(true),
      };
    }
    cpthread_create(&worker_ids[worker_index], nested_outer_worker,
                    &workers[worker_index]);
  }
  for (int worker_index = 0; worker_index < num_workers; worker_index++) {
    cpthread_join(worker_ids[worker_index]);
  }
  for (int root_index = 0; root_index < root_count; root_index++) {
    combine_worker_stat(workers, num_workers, get_worker_utility, root_index,
                        roots[root_index].utility);
    combine_worker_stat(workers, num_workers, get_worker_win_pct, root_index,
                        roots[root_index].win_pct);
    combine_worker_stat(workers, num_workers, get_worker_spread, root_index,
                        roots[root_index].spread);
  }
  for (int delta = 0; delta < 3; delta++) {
    combine_worker_stat(workers, num_workers, get_worker_delta, delta,
                        position_deltas[delta]);
  }
  for (int worker_index = 0; worker_index < num_workers; worker_index++) {
    counters->static_agreements +=
        workers[worker_index].counters.static_agreements;
    counters->nested_disagreements +=
        workers[worker_index].counters.nested_disagreements;
    counters->nested_ties += workers[worker_index].counters.nested_ties;
    counters->nested_policy_samples +=
        workers[worker_index].counters.nested_policy_samples;
    counters->nested_nodes += workers[worker_index].counters.nested_nodes;
    counters->outer_candidate_rollouts +=
        workers[worker_index].counters.outer_candidate_rollouts;
    counters->outer_continuation_nodes +=
        workers[worker_index].counters.outer_continuation_nodes;
    for (int delta = 0; delta < 3; delta++) {
      stat_destroy(workers[worker_index].position_deltas[delta]);
    }
    nested_root_stats_destroy(workers[worker_index].roots, root_count);
  }
  free(worker_ids);
  free(workers);
}

static NestedCorpusAggregate nested_corpus_aggregate_create(void) {
  return (NestedCorpusAggregate){
      .klv3_minus_klv2 = stat_create(true),
      .hybrid_minus_klv2 = stat_create(true),
      .klv3_minus_hybrid = stat_create(true),
  };
}

static void nested_corpus_aggregate_destroy(NestedCorpusAggregate *aggregate) {
  stat_destroy(aggregate->klv3_minus_klv2);
  stat_destroy(aggregate->hybrid_minus_klv2);
  stat_destroy(aggregate->klv3_minus_hybrid);
}

static void print_nested_corpus_summary(const NestedCorpusAggregate *aggregate,
                                        double elapsed_seconds) {
  const uint64_t continuation_decisions =
      aggregate->counters.static_agreements +
      aggregate->counters.nested_disagreements;
  const double nested_rate =
      continuation_decisions > 0
          ? (double)aggregate->counters.nested_disagreements /
                (double)continuation_decisions
          : 0.0;
  printf("KLV3_NESTED_SUMMARY positions=%d "
         "klv2_root_wins=%d hybrid_root_wins=%d klv3_root_wins=%d "
         "klv3_minus_klv2=%+.9f klv3_minus_klv2_sem=%.9f "
         "hybrid_minus_klv2=%+.9f hybrid_minus_klv2_sem=%.9f "
         "klv3_minus_hybrid=%+.9f klv3_minus_hybrid_sem=%.9f "
         "outer_candidate_rollouts=%" PRIu64 " outer_nodes=%" PRIu64 " "
         "static_agreements=%" PRIu64 " nested_disagreements=%" PRIu64 " "
         "nested_rate=%.6f nested_ties=%" PRIu64 " "
         "nested_policy_samples=%" PRIu64 " nested_nodes=%" PRIu64 " "
         "elapsed_seconds=%.6f\n",
         aggregate->positions, aggregate->model_root_wins[0],
         aggregate->model_root_wins[1], aggregate->model_root_wins[2],
         stat_get_mean(aggregate->klv3_minus_klv2),
         stat_get_sem(aggregate->klv3_minus_klv2),
         stat_get_mean(aggregate->hybrid_minus_klv2),
         stat_get_sem(aggregate->hybrid_minus_klv2),
         stat_get_mean(aggregate->klv3_minus_hybrid),
         stat_get_sem(aggregate->klv3_minus_hybrid),
         aggregate->counters.outer_candidate_rollouts,
         aggregate->counters.outer_continuation_nodes,
         aggregate->counters.static_agreements,
         aggregate->counters.nested_disagreements, nested_rate,
         aggregate->counters.nested_ties,
         aggregate->counters.nested_policy_samples,
         aggregate->counters.nested_nodes, elapsed_seconds);
}

static void run_nested_corpus_oracle(Config *klv2_config, Config *klv3_config,
                                     const char *corpus_files,
                                     int outer_samples, int outer_plies,
                                     int nested_samples_per_candidate,
                                     int num_threads, int start_disagreement,
                                     int max_positions, int min_bag_tiles,
                                     double wall_seconds) {
  enum {
    CORPUS_FIELD_CAPACITY = 40,
    CORPUS_MIN_FIELDS = 39,
    CORPUS_DISAGREEMENT_FIELD = 1,
    CORPUS_POSITION_FIELD = 2,
    CORPUS_GAME_FIELD = 3,
    CORPUS_TURN_FIELD = 4,
    CORPUS_BAG_FIELD = 5,
    CORPUS_KLV2_MOVE_RAW_FIELD = 11,
    CORPUS_HYBRID_MOVE_RAW_FIELD = 12,
    CORPUS_KLV3_MOVE_RAW_FIELD = 13,
    CORPUS_CGP_FIELD = 38,
  };
  NestedCorpusAggregate aggregate = nested_corpus_aggregate_create();
  Timer timer;
  ctimer_start(&timer);
  int rows_read = 0;
  int rows_below_min_bag = 0;
  int all_same_roots = 0;
  int evaluated = 0;
  int last_disagreement = start_disagreement;
  bool wall_limit_reached = false;
  char *files_copy = string_duplicate(corpus_files);
  char *files_saveptr = NULL;

  printf("KLV3_NESTED_CONFIG files=%s outer_samples_per_candidate=%d "
         "outer_plies=%d nested_plies=%d nested_samples_per_candidate=%d "
         "nested_policy_split=%d+%d outer_threads=%d nested_threads=1 "
         "start_disagreement=%d "
         "max_positions=%d min_bag_tiles=%d wall_seconds=%.3f\n",
         corpus_files, outer_samples, outer_plies, KLV3_ORACLE_NESTED_PLIES,
         nested_samples_per_candidate, nested_samples_per_candidate / 2,
         nested_samples_per_candidate / 2, num_threads, start_disagreement,
         max_positions, min_bag_tiles, wall_seconds);
  fflush_or_die(stdout);

  for (char *filename = strtok_r(files_copy, ",", &files_saveptr);
       filename != NULL && !wall_limit_reached;
       filename = strtok_r(NULL, ",", &files_saveptr)) {
    FILE *stream = fopen(filename, "re");
    if (stream == NULL) {
      log_fatal("could not open KLV3 oracle corpus file: %s", filename);
    }
    assert(stream != NULL);
    char *line = NULL;
    size_t line_capacity = 0;
    while (getline_ignore_carriage_return(&line, &line_capacity, stream) !=
           -1) {
      if (strncmp(line, "KLV3_THREEWAY_DISAGREEMENT\t", 27) != 0) {
        continue;
      }
      rows_read++;
      char *fields[CORPUS_FIELD_CAPACITY] = {0};
      const int field_count =
          split_tab_fields(line, fields, CORPUS_FIELD_CAPACITY);
      if (field_count < CORPUS_MIN_FIELDS) {
        log_fatal("truncated KLV3 nested-oracle corpus row in %s", filename);
      }
      const int disagreement =
          (int)strtol(fields[CORPUS_DISAGREEMENT_FIELD], NULL, 10);
      if (disagreement <= start_disagreement) {
        continue;
      }
      if (max_positions > 0 && evaluated >= max_positions) {
        break;
      }
      if (wall_seconds > 0.0 &&
          ctimer_elapsed_seconds(&timer) >= wall_seconds) {
        wall_limit_reached = true;
        break;
      }
      const int bag_tiles = (int)strtol(fields[CORPUS_BAG_FIELD], NULL, 10);
      if (bag_tiles < min_bag_tiles) {
        rows_below_min_bag++;
        continue;
      }
      fields[CORPUS_CGP_FIELD][strcspn(fields[CORPUS_CGP_FIELD], "\r\n")] =
          '\0';
      Move model_moves[3];
      if (!parse_record_move(fields[CORPUS_KLV2_MOVE_RAW_FIELD],
                             &model_moves[0]) ||
          !parse_record_move(fields[CORPUS_HYBRID_MOVE_RAW_FIELD],
                             &model_moves[1]) ||
          !parse_record_move(fields[CORPUS_KLV3_MOVE_RAW_FIELD],
                             &model_moves[2])) {
        log_fatal("invalid raw move in KLV3 nested-oracle disagreement %d",
                  disagreement);
      }
      NestedRootStats roots[KLV3_ORACLE_MAX_ROOT_CANDIDATES] = {0};
      int root_count = 0;
      int model_root_indices[3];
      for (int model = 0; model < 3; model++) {
        const int index =
            add_unique_root_move(roots, root_count, &model_moves[model]);
        model_root_indices[model] = index;
        if (index == root_count) {
          root_count++;
        }
      }
      if (root_count == 1) {
        all_same_roots++;
        nested_root_stats_destroy(roots, root_count);
        last_disagreement = disagreement;
        continue;
      }

      load_position(klv2_config, fields[CORPUS_CGP_FIELD]);
      load_position(klv3_config, fields[CORPUS_CGP_FIELD]);
      Stat *position_deltas[3] = {
          stat_create(true),
          stat_create(true),
          stat_create(true),
      };
      const uint64_t position_seed =
          KLV3_ORACLE_BASE_SEED +
          (uint64_t)disagreement * KLV3_ORACLE_SEED_STRIDE;
      run_nested_position_samples(
          klv2_config, klv3_config, roots, root_count, model_root_indices,
          outer_samples, outer_plies, nested_samples_per_candidate, num_threads,
          position_seed, position_deltas, &aggregate.counters);

      int best_root = 0;
      for (int root_index = 1; root_index < root_count; root_index++) {
        if (stat_get_mean(roots[root_index].utility) >
            stat_get_mean(roots[best_root].utility)) {
          best_root = root_index;
        }
      }
      for (int model = 0; model < 3; model++) {
        aggregate.model_root_wins[model] +=
            model_root_indices[model] == best_root;
      }
      stat_push(aggregate.klv3_minus_klv2, stat_get_mean(position_deltas[0]),
                1);
      stat_push(aggregate.hybrid_minus_klv2, stat_get_mean(position_deltas[1]),
                1);
      stat_push(aggregate.klv3_minus_hybrid, stat_get_mean(position_deltas[2]),
                1);
      aggregate.positions++;
      evaluated++;
      last_disagreement = disagreement;
      printf(
          "KLV3_NESTED_POSITION disagreement=%d position=%s game=%s turn=%s "
          "bag=%d unique_roots=%d outer_samples=%d "
          "klv2_root=%d hybrid_root=%d klv3_root=%d best_root=%d "
          "klv3_minus_klv2=%+.9f klv3_minus_klv2_sem=%.9f "
          "hybrid_minus_klv2=%+.9f hybrid_minus_klv2_sem=%.9f "
          "klv3_minus_hybrid=%+.9f klv3_minus_hybrid_sem=%.9f",
          disagreement, fields[CORPUS_POSITION_FIELD],
          fields[CORPUS_GAME_FIELD], fields[CORPUS_TURN_FIELD], bag_tiles,
          root_count, outer_samples, model_root_indices[0],
          model_root_indices[1], model_root_indices[2], best_root,
          stat_get_mean(position_deltas[0]), stat_get_sem(position_deltas[0]),
          stat_get_mean(position_deltas[1]), stat_get_sem(position_deltas[1]),
          stat_get_mean(position_deltas[2]), stat_get_sem(position_deltas[2]));
      for (int root_index = 0; root_index < root_count; root_index++) {
        printf(" root%d_utility=%.9f root%d_utility_sem=%.9f "
               "root%d_win=%.9f root%d_spread=%.6f",
               root_index, stat_get_mean(roots[root_index].utility), root_index,
               stat_get_sem(roots[root_index].utility), root_index,
               stat_get_mean(roots[root_index].win_pct), root_index,
               stat_get_mean(roots[root_index].spread));
      }
      printf("\n");
      for (int delta = 0; delta < 3; delta++) {
        stat_destroy(position_deltas[delta]);
      }
      nested_root_stats_destroy(roots, root_count);
      if (evaluated % 5 == 0) {
        print_nested_corpus_summary(&aggregate, ctimer_elapsed_seconds(&timer));
      }
      fflush_or_die(stdout);
    }
    free(line);
    fclose_or_die(stream);
  }
  free(files_copy);
  print_nested_corpus_summary(&aggregate, ctimer_elapsed_seconds(&timer));
  printf("KLV3_NESTED_DONE rows_read=%d rows_below_min_bag=%d "
         "all_same_roots=%d evaluated=%d last_disagreement=%d "
         "elapsed_seconds=%.6f wall_limit_reached=%d\n",
         rows_read, rows_below_min_bag, all_same_roots, evaluated,
         last_disagreement, ctimer_elapsed_seconds(&timer), wall_limit_reached);
  fflush_or_die(stdout);

  nested_corpus_aggregate_destroy(&aggregate);
}

typedef struct EqualSampleAggregate {
  int source_positions;
  int agreements;
  int disagreements;
  int model_root_wins[2];
  uint64_t nomination_iterations[2];
  uint64_t nomination_nodes[2];
  double nomination_seconds[2];
  Stat *conditional_utility_delta;
  Stat *conditional_win_delta;
  Stat *conditional_spread_delta;
  Stat *all_position_utility_delta;
  NestedOracleCounters counters;
} EqualSampleAggregate;

static EqualSampleAggregate equal_sample_aggregate_create(void) {
  return (EqualSampleAggregate){
      .conditional_utility_delta = stat_create(true),
      .conditional_win_delta = stat_create(true),
      .conditional_spread_delta = stat_create(true),
      .all_position_utility_delta = stat_create(true),
  };
}

static void equal_sample_aggregate_destroy(EqualSampleAggregate *aggregate) {
  stat_destroy(aggregate->conditional_utility_delta);
  stat_destroy(aggregate->conditional_win_delta);
  stat_destroy(aggregate->conditional_spread_delta);
  stat_destroy(aggregate->all_position_utility_delta);
}

static void print_equal_sample_summary(const EqualSampleAggregate *aggregate,
                                       int global_disagreements,
                                       double elapsed_seconds) {
  const double disagreement_rate = aggregate->source_positions > 0
                                       ? (double)aggregate->disagreements /
                                             (double)aggregate->source_positions
                                       : 0.0;
  const double klv2_rate = aggregate->nomination_seconds[0] > 0.0
                               ? (double)aggregate->nomination_iterations[0] /
                                     aggregate->nomination_seconds[0]
                               : 0.0;
  const double klv3_rate = aggregate->nomination_seconds[1] > 0.0
                               ? (double)aggregate->nomination_iterations[1] /
                                     aggregate->nomination_seconds[1]
                               : 0.0;
  printf("KLV3_EQUAL_SUMMARY source_positions=%d agreements=%d "
         "run_disagreements=%d global_disagreements=%d disagreement_rate=%.6f "
         "klv2_root_wins=%d klv3_root_wins=%d "
         "conditional_klv3_minus_klv2=%+.9f conditional_sem=%.9f "
         "conditional_win_delta=%+.9f conditional_win_sem=%.9f "
         "conditional_spread_delta=%+.9f conditional_spread_sem=%.9f "
         "all_position_klv3_minus_klv2=%+.9f all_position_sem=%.9f "
         "klv2_iterations=%" PRIu64 " klv3_iterations=%" PRIu64 " "
         "klv2_iters_per_second=%.3f klv3_iters_per_second=%.3f "
         "outer_candidate_rollouts=%" PRIu64 " nested_policy_samples=%" PRIu64
         " elapsed_seconds=%.6f\n",
         aggregate->source_positions, aggregate->agreements,
         aggregate->disagreements, global_disagreements, disagreement_rate,
         aggregate->model_root_wins[0], aggregate->model_root_wins[1],
         stat_get_mean(aggregate->conditional_utility_delta),
         stat_get_sem(aggregate->conditional_utility_delta),
         stat_get_mean(aggregate->conditional_win_delta),
         stat_get_sem(aggregate->conditional_win_delta),
         stat_get_mean(aggregate->conditional_spread_delta),
         stat_get_sem(aggregate->conditional_spread_delta),
         stat_get_mean(aggregate->all_position_utility_delta),
         stat_get_sem(aggregate->all_position_utility_delta),
         aggregate->nomination_iterations[0],
         aggregate->nomination_iterations[1], klv2_rate, klv3_rate,
         aggregate->counters.outer_candidate_rollouts,
         aggregate->counters.nested_policy_samples, elapsed_seconds);
}

static void run_equal_sample_online_oracle(
    Config *klv2_config, Config *klv3_config, int num_games, int start_game,
    int max_positions, int start_position, int collect_from_position,
    int start_disagreements, int target_disagreements, int samples_per_arm,
    int outer_samples, int outer_plies, int nested_samples_per_candidate,
    int num_threads, int min_bag_tiles, double wall_seconds) {
  EqualSampleAggregate aggregate = equal_sample_aggregate_create();
  NominationContext nomination_contexts[2];
  nomination_context_init(&nomination_contexts[0], klv2_config);
  nomination_context_init(&nomination_contexts[1], klv3_config);
  Game *trajectory_game = config_game_create(klv2_config);
  MoveList *trajectory_move_list = move_list_create(1);
  Timer timer;
  ctimer_start(&timer);
  int position_index = start_position;
  int global_disagreements = start_disagreements;
  bool wall_limit_reached = false;

  printf("KLV3_EQUAL_CONFIG games=%d start_game=%d max_positions=%d "
         "start_position=%d collect_from_position=%d "
         "start_disagreements=%d target_disagreements=%d "
         "samples_per_arm=%d max_arms=%d nomination_plies=%d "
         "outer_samples_per_candidate=%d outer_plies=%d nested_plies=%d "
         "nested_samples_per_candidate=%d nested_policy_split=%d+%d "
         "nomination_threads=%d outer_threads=%d nested_threads=1 "
         "min_bag_tiles=%d wall_seconds=%.3f\n",
         num_games, start_game, max_positions, start_position,
         collect_from_position, start_disagreements, target_disagreements,
         samples_per_arm, KLV3_ORACLE_NUM_PLAYS, KLV3_ORACLE_SIM_PLIES,
         outer_samples, outer_plies, KLV3_ORACLE_NESTED_PLIES,
         nested_samples_per_candidate, nested_samples_per_candidate / 2,
         nested_samples_per_candidate / 2, num_threads, num_threads,
         min_bag_tiles, wall_seconds);
  fflush_or_die(stdout);

  for (int game_index = start_game;
       game_index < num_games && position_index < max_positions &&
       global_disagreements < target_disagreements && !wall_limit_reached;
       game_index++) {
    game_reset(trajectory_game);
    game_seed(trajectory_game,
              KLV3_ORACLE_BASE_SEED +
                  (uint64_t)game_index * KLV3_ORACLE_SEED_STRIDE);
    game_set_starting_player_index(trajectory_game, game_index % 2);
    draw_starting_racks(trajectory_game);
    int turn_index = 0;
    while (!game_over(trajectory_game) &&
           bag_get_letters(game_get_bag(trajectory_game)) >= min_bag_tiles &&
           position_index < max_positions &&
           global_disagreements < target_disagreements) {
      if (wall_seconds > 0.0 &&
          ctimer_elapsed_seconds(&timer) >= wall_seconds) {
        wall_limit_reached = true;
        break;
      }
      char *cgp = game_get_cgp(trajectory_game, true);
      load_position(klv2_config, cgp);
      load_position(klv3_config, cgp);

      if (position_index >= collect_from_position) {
        NominationResult nominations[2];
        const uint64_t nomination_seed =
            KLV3_ORACLE_BASE_SEED +
            (uint64_t)(position_index + 1) * KLV3_ORACLE_SEED_STRIDE;
        if (position_index % 2 == 0) {
          nominations[0] = nominate_move_with_fixed_samples(
              klv2_config, &nomination_contexts[0], samples_per_arm,
              nomination_seed);
          nominations[1] = nominate_move_with_fixed_samples(
              klv3_config, &nomination_contexts[1], samples_per_arm,
              nomination_seed);
        } else {
          nominations[1] = nominate_move_with_fixed_samples(
              klv3_config, &nomination_contexts[1], samples_per_arm,
              nomination_seed);
          nominations[0] = nominate_move_with_fixed_samples(
              klv2_config, &nomination_contexts[0], samples_per_arm,
              nomination_seed);
        }
        aggregate.source_positions++;
        for (int model = 0; model < 2; model++) {
          aggregate.nomination_iterations[model] +=
              nominations[model].iterations;
          aggregate.nomination_nodes[model] += nominations[model].nodes;
          aggregate.nomination_seconds[model] +=
              nominations[model].elapsed_seconds;
        }

        if (moves_are_equal(&nominations[0].move, &nominations[1].move)) {
          aggregate.agreements++;
          stat_push(aggregate.all_position_utility_delta, 0.0, 1);
        } else {
          NestedRootStats roots[KLV3_ORACLE_MAX_ROOT_CANDIDATES] = {0};
          const int klv2_root =
              add_unique_root_move(roots, 0, &nominations[0].move);
          int root_count = 1;
          const int klv3_root =
              add_unique_root_move(roots, root_count, &nominations[1].move);
          if (klv3_root == root_count) {
            root_count++;
          }
          assert(root_count == 2);
          const int model_root_indices[3] = {
              klv2_root,
              klv2_root,
              klv3_root,
          };
          Stat *position_deltas[3] = {
              stat_create(true),
              stat_create(true),
              stat_create(true),
          };
          const uint64_t position_seed =
              KLV3_ORACLE_BASE_SEED +
              (uint64_t)(position_index + 1) * KLV3_ORACLE_SEED_STRIDE;
          run_nested_position_samples(
              klv2_config, klv3_config, roots, root_count, model_root_indices,
              outer_samples, outer_plies, nested_samples_per_candidate,
              num_threads, position_seed, position_deltas, &aggregate.counters);

          const double utility_delta = stat_get_mean(position_deltas[0]);
          const double win_delta = stat_get_mean(roots[klv3_root].win_pct) -
                                   stat_get_mean(roots[klv2_root].win_pct);
          const double spread_delta = stat_get_mean(roots[klv3_root].spread) -
                                      stat_get_mean(roots[klv2_root].spread);
          stat_push(aggregate.conditional_utility_delta, utility_delta, 1);
          stat_push(aggregate.conditional_win_delta, win_delta, 1);
          stat_push(aggregate.conditional_spread_delta, spread_delta, 1);
          stat_push(aggregate.all_position_utility_delta, utility_delta, 1);
          aggregate.disagreements++;
          global_disagreements++;

          int best_root = 0;
          for (int root_index = 1; root_index < root_count; root_index++) {
            if (stat_get_mean(roots[root_index].utility) >
                stat_get_mean(roots[best_root].utility)) {
              best_root = root_index;
            }
          }
          aggregate.model_root_wins[0] += klv2_root == best_root;
          aggregate.model_root_wins[1] += klv3_root == best_root;

          char klv2_move_string[64];
          char klv3_move_string[64];
          char klv2_move_raw[256];
          char klv3_move_raw[256];
          move_to_string(config_get_game(klv2_config), &nominations[0].move,
                         klv2_move_string, sizeof(klv2_move_string));
          move_to_string(config_get_game(klv3_config), &nominations[1].move,
                         klv3_move_string, sizeof(klv3_move_string));
          move_to_record_encoding(&nominations[0].move, klv2_move_raw,
                                  sizeof(klv2_move_raw));
          move_to_record_encoding(&nominations[1].move, klv3_move_raw,
                                  sizeof(klv3_move_raw));
          printf(
              "KLV3_EQUAL_POSITION disagreement=%d position=%d game=%d "
              "turn=%d bag=%d samples_per_arm=%d "
              "klv2_iterations=%" PRIu64 " klv3_iterations=%" PRIu64 " "
              "klv2_nodes=%" PRIu64 " klv3_nodes=%" PRIu64 " "
              "klv2_seconds=%.9f klv3_seconds=%.9f "
              "klv2_move=%s klv3_move=%s "
              "klv2_move_raw=%s klv3_move_raw=%s "
              "best_root=%d klv3_minus_klv2=%+.9f sem=%.9f "
              "win_delta=%+.9f spread_delta=%+.9f "
              "root0_utility=%.9f root0_utility_sem=%.9f "
              "root0_win=%.9f root0_spread=%.6f "
              "root1_utility=%.9f root1_utility_sem=%.9f "
              "root1_win=%.9f root1_spread=%.6f cgp=%s\n",
              global_disagreements, position_index, game_index, turn_index,
              bag_get_letters(game_get_bag(trajectory_game)), samples_per_arm,
              nominations[0].iterations, nominations[1].iterations,
              nominations[0].nodes, nominations[1].nodes,
              nominations[0].elapsed_seconds, nominations[1].elapsed_seconds,
              klv2_move_string, klv3_move_string, klv2_move_raw, klv3_move_raw,
              best_root, utility_delta, stat_get_sem(position_deltas[0]),
              win_delta, spread_delta, stat_get_mean(roots[0].utility),
              stat_get_sem(roots[0].utility), stat_get_mean(roots[0].win_pct),
              stat_get_mean(roots[0].spread), stat_get_mean(roots[1].utility),
              stat_get_sem(roots[1].utility), stat_get_mean(roots[1].win_pct),
              stat_get_mean(roots[1].spread), cgp);

          for (int delta = 0; delta < 3; delta++) {
            stat_destroy(position_deltas[delta]);
          }
          nested_root_stats_destroy(roots, root_count);
          if (aggregate.disagreements % 5 == 0) {
            print_equal_sample_summary(&aggregate, global_disagreements,
                                       ctimer_elapsed_seconds(&timer));
          }
          fflush_or_die(stdout);
        }
      }

      Game *trajectory_policy_game = game_index % 2 == 0
                                         ? config_get_game(klv2_config)
                                         : config_get_game(klv3_config);
      const Move *trajectory_move =
          get_top_equity_move(trajectory_policy_game, trajectory_move_list);
      assert(trajectory_move != NULL);
      play_move(trajectory_move, trajectory_game, NULL);
      free(cgp);
      position_index++;
      turn_index++;
    }
  }

  if (aggregate.disagreements > 0) {
    print_equal_sample_summary(&aggregate, global_disagreements,
                               ctimer_elapsed_seconds(&timer));
  }
  printf("KLV3_EQUAL_DONE source_positions=%d agreements=%d "
         "run_disagreements=%d global_disagreements=%d target=%d "
         "last_position=%d elapsed_seconds=%.6f wall_limit_reached=%d\n",
         aggregate.source_positions, aggregate.agreements,
         aggregate.disagreements, global_disagreements, target_disagreements,
         position_index, ctimer_elapsed_seconds(&timer), wall_limit_reached);
  fflush_or_die(stdout);

  move_list_destroy(trajectory_move_list);
  game_destroy(trajectory_game);
  nomination_context_destroy(&nomination_contexts[0]);
  nomination_context_destroy(&nomination_contexts[1]);
  equal_sample_aggregate_destroy(&aggregate);
}

static void print_candidate_selector_summary(
    const EqualSampleAggregate *aggregate, int global_comparisons,
    uint64_t total_top_candidates, uint64_t total_overlap,
    uint64_t total_unique_per_selector, double elapsed_seconds) {
  const double comparison_rate = aggregate->source_positions > 0
                                     ? (double)aggregate->disagreements /
                                           (double)aggregate->source_positions
                                     : 0.0;
  const double avg_top =
      aggregate->source_positions > 0
          ? (double)total_top_candidates / aggregate->source_positions
          : 0.0;
  const double avg_overlap =
      aggregate->source_positions > 0
          ? (double)total_overlap / aggregate->source_positions
          : 0.0;
  const double avg_unique =
      aggregate->source_positions > 0
          ? (double)total_unique_per_selector / aggregate->source_positions
          : 0.0;
  const double klv2_rate = aggregate->nomination_seconds[0] > 0.0
                               ? (double)aggregate->nomination_iterations[0] /
                                     aggregate->nomination_seconds[0]
                               : 0.0;
  const double klv3_rate = aggregate->nomination_seconds[1] > 0.0
                               ? (double)aggregate->nomination_iterations[1] /
                                     aggregate->nomination_seconds[1]
                               : 0.0;
  printf("KLV3_CANDIDATE_SUMMARY source_positions=%d full_overlap=%d "
         "run_comparisons=%d global_comparisons=%d comparison_rate=%.6f "
         "avg_top_candidates=%.6f avg_overlap=%.6f "
         "avg_unique_per_selector=%.6f "
         "klv2_unique_root_wins=%d klv3_unique_root_wins=%d "
         "conditional_klv3_minus_klv2=%+.9f conditional_sem=%.9f "
         "conditional_win_delta=%+.9f conditional_win_sem=%.9f "
         "conditional_spread_delta=%+.9f conditional_spread_sem=%.9f "
         "all_source_marginal_delta=%+.9f all_source_sem=%.9f "
         "klv2_unique_iterations=%" PRIu64 " klv3_unique_iterations=%" PRIu64
         " klv2_unique_iters_per_second=%.3f "
         "klv3_unique_iters_per_second=%.3f "
         "outer_candidate_rollouts=%" PRIu64 " nested_policy_samples=%" PRIu64
         " elapsed_seconds=%.6f\n",
         aggregate->source_positions, aggregate->agreements,
         aggregate->disagreements, global_comparisons, comparison_rate, avg_top,
         avg_overlap, avg_unique, aggregate->model_root_wins[0],
         aggregate->model_root_wins[1],
         stat_get_mean(aggregate->conditional_utility_delta),
         stat_get_sem(aggregate->conditional_utility_delta),
         stat_get_mean(aggregate->conditional_win_delta),
         stat_get_sem(aggregate->conditional_win_delta),
         stat_get_mean(aggregate->conditional_spread_delta),
         stat_get_sem(aggregate->conditional_spread_delta),
         stat_get_mean(aggregate->all_position_utility_delta),
         stat_get_sem(aggregate->all_position_utility_delta),
         aggregate->nomination_iterations[0],
         aggregate->nomination_iterations[1], klv2_rate, klv3_rate,
         aggregate->counters.outer_candidate_rollouts,
         aggregate->counters.nested_policy_samples, elapsed_seconds);
}

static void run_candidate_selector_online_oracle(
    Config *klv2_config, Config *klv3_config, int num_games, int start_game,
    int max_positions, int start_position, int collect_from_position,
    int start_comparisons, int target_comparisons, int samples_per_arm,
    int outer_samples, int outer_plies, int nested_samples_per_candidate,
    int num_threads, int min_bag_tiles, double wall_seconds) {
  EqualSampleAggregate aggregate = equal_sample_aggregate_create();
  NominationContext nomination_contexts[2];
  nomination_context_init(&nomination_contexts[0], klv2_config);
  nomination_context_init(&nomination_contexts[1], klv2_config);
  MoveList *klv2_only = move_list_create(KLV3_ORACLE_NUM_PLAYS);
  MoveList *klv3_only = move_list_create(KLV3_ORACLE_NUM_PLAYS);
  Game *trajectory_game = config_game_create(klv2_config);
  MoveList *trajectory_move_list = move_list_create(1);
  Timer timer;
  ctimer_start(&timer);
  int position_index = start_position;
  int global_comparisons = start_comparisons;
  uint64_t total_top_candidates = 0;
  uint64_t total_overlap = 0;
  uint64_t total_unique_per_selector = 0;
  bool wall_limit_reached = false;

  printf("KLV3_CANDIDATE_CONFIG games=%d start_game=%d max_positions=%d "
         "start_position=%d collect_from_position=%d "
         "start_comparisons=%d target_comparisons=%d "
         "top_candidates=%d compare_nonoverlap_only=1 "
         "nomination_policy=klv2 nomination_plies=%d samples_per_unique_arm=%d "
         "sampling_rule=round_robin "
         "outer_samples_per_candidate=%d outer_plies=%d nested_plies=%d "
         "nested_samples_per_candidate=%d nested_policy_split=%d+%d "
         "nomination_threads=%d outer_threads=%d nested_threads=1 "
         "min_bag_tiles=%d wall_seconds=%.3f\n",
         num_games, start_game, max_positions, start_position,
         collect_from_position, start_comparisons, target_comparisons,
         KLV3_ORACLE_NUM_PLAYS, KLV3_ORACLE_SIM_PLIES, samples_per_arm,
         outer_samples, outer_plies, KLV3_ORACLE_NESTED_PLIES,
         nested_samples_per_candidate, nested_samples_per_candidate / 2,
         nested_samples_per_candidate / 2, num_threads, num_threads,
         min_bag_tiles, wall_seconds);
  fflush_or_die(stdout);

  for (int game_index = start_game;
       game_index < num_games && position_index < max_positions &&
       global_comparisons < target_comparisons && !wall_limit_reached;
       game_index++) {
    game_reset(trajectory_game);
    game_seed(trajectory_game,
              KLV3_ORACLE_BASE_SEED +
                  (uint64_t)game_index * KLV3_ORACLE_SEED_STRIDE);
    game_set_starting_player_index(trajectory_game, game_index % 2);
    draw_starting_racks(trajectory_game);
    int turn_index = 0;
    while (!game_over(trajectory_game) &&
           bag_get_letters(game_get_bag(trajectory_game)) >= min_bag_tiles &&
           position_index < max_positions &&
           global_comparisons < target_comparisons) {
      if (wall_seconds > 0.0 &&
          ctimer_elapsed_seconds(&timer) >= wall_seconds) {
        wall_limit_reached = true;
        break;
      }
      char *cgp = game_get_cgp(trajectory_game, true);
      load_position(klv2_config, cgp);
      load_position(klv3_config, cgp);

      if (position_index >= collect_from_position) {
        const int overlap = build_nonoverlap_candidate_lists(
            klv2_config, klv3_config, klv2_only, klv3_only);
        const int unique_count = move_list_get_count(klv2_only);
        const int top_count = overlap + unique_count;
        aggregate.source_positions++;
        total_top_candidates += (uint64_t)top_count;
        total_overlap += (uint64_t)overlap;
        total_unique_per_selector += (uint64_t)unique_count;

        if (unique_count == 0) {
          aggregate.agreements++;
          stat_push(aggregate.all_position_utility_delta, 0.0, 1);
        } else {
          NominationResult nominations[2];
          const uint64_t nomination_seed =
              KLV3_ORACLE_BASE_SEED +
              (uint64_t)(position_index + 1) * KLV3_ORACLE_SEED_STRIDE;
          if (position_index % 2 == 0) {
            nominations[0] = nominate_candidate_list_with_klv2_policy(
                klv2_config, &nomination_contexts[0], klv2_only,
                samples_per_arm, nomination_seed);
            nominations[1] = nominate_candidate_list_with_klv2_policy(
                klv2_config, &nomination_contexts[1], klv3_only,
                samples_per_arm, nomination_seed);
          } else {
            nominations[1] = nominate_candidate_list_with_klv2_policy(
                klv2_config, &nomination_contexts[1], klv3_only,
                samples_per_arm, nomination_seed);
            nominations[0] = nominate_candidate_list_with_klv2_policy(
                klv2_config, &nomination_contexts[0], klv2_only,
                samples_per_arm, nomination_seed);
          }
          assert(!moves_are_equal(&nominations[0].move, &nominations[1].move));
          for (int selector = 0; selector < 2; selector++) {
            aggregate.nomination_iterations[selector] +=
                nominations[selector].iterations;
            aggregate.nomination_nodes[selector] += nominations[selector].nodes;
            aggregate.nomination_seconds[selector] +=
                nominations[selector].elapsed_seconds;
          }

          NestedRootStats roots[KLV3_ORACLE_MAX_ROOT_CANDIDATES] = {0};
          const int klv2_root =
              add_unique_root_move(roots, 0, &nominations[0].move);
          int root_count = 1;
          const int klv3_root =
              add_unique_root_move(roots, root_count, &nominations[1].move);
          if (klv3_root == root_count) {
            root_count++;
          }
          assert(root_count == 2);
          const int selector_root_indices[3] = {
              klv2_root,
              klv2_root,
              klv3_root,
          };
          Stat *position_deltas[3] = {
              stat_create(true),
              stat_create(true),
              stat_create(true),
          };
          const uint64_t position_seed =
              KLV3_ORACLE_BASE_SEED +
              (uint64_t)(position_index + 1) * KLV3_ORACLE_SEED_STRIDE;
          run_nested_position_samples(
              klv2_config, klv3_config, roots, root_count,
              selector_root_indices, outer_samples, outer_plies,
              nested_samples_per_candidate, num_threads, position_seed,
              position_deltas, &aggregate.counters);

          const double utility_delta = stat_get_mean(position_deltas[0]);
          const double win_delta = stat_get_mean(roots[klv3_root].win_pct) -
                                   stat_get_mean(roots[klv2_root].win_pct);
          const double spread_delta = stat_get_mean(roots[klv3_root].spread) -
                                      stat_get_mean(roots[klv2_root].spread);
          stat_push(aggregate.conditional_utility_delta, utility_delta, 1);
          stat_push(aggregate.conditional_win_delta, win_delta, 1);
          stat_push(aggregate.conditional_spread_delta, spread_delta, 1);
          stat_push(aggregate.all_position_utility_delta, utility_delta, 1);
          aggregate.disagreements++;
          global_comparisons++;

          int best_root = 0;
          for (int root_index = 1; root_index < root_count; root_index++) {
            if (stat_get_mean(roots[root_index].utility) >
                stat_get_mean(roots[best_root].utility)) {
              best_root = root_index;
            }
          }
          aggregate.model_root_wins[0] += klv2_root == best_root;
          aggregate.model_root_wins[1] += klv3_root == best_root;

          char klv2_move_string[64];
          char klv3_move_string[64];
          char klv2_move_raw[256];
          char klv3_move_raw[256];
          move_to_string(config_get_game(klv2_config), &nominations[0].move,
                         klv2_move_string, sizeof(klv2_move_string));
          move_to_string(config_get_game(klv2_config), &nominations[1].move,
                         klv3_move_string, sizeof(klv3_move_string));
          move_to_record_encoding(&nominations[0].move, klv2_move_raw,
                                  sizeof(klv2_move_raw));
          move_to_record_encoding(&nominations[1].move, klv3_move_raw,
                                  sizeof(klv3_move_raw));
          printf(
              "KLV3_CANDIDATE_POSITION comparison=%d position=%d game=%d "
              "turn=%d bag=%d top_count=%d overlap=%d "
              "unique_per_selector=%d samples_per_unique_arm=%d "
              "klv2_unique_iterations=%" PRIu64
              " klv3_unique_iterations=%" PRIu64 " "
              "klv2_unique_nodes=%" PRIu64 " klv3_unique_nodes=%" PRIu64 " "
              "klv2_unique_seconds=%.9f klv3_unique_seconds=%.9f "
              "klv2_unique_move=%s klv3_unique_move=%s "
              "klv2_move_raw=%s klv3_move_raw=%s "
              "best_root=%d klv3_minus_klv2=%+.9f sem=%.9f "
              "win_delta=%+.9f spread_delta=%+.9f "
              "root0_utility=%.9f root0_utility_sem=%.9f "
              "root0_win=%.9f root0_spread=%.6f "
              "root1_utility=%.9f root1_utility_sem=%.9f "
              "root1_win=%.9f root1_spread=%.6f cgp=%s\n",
              global_comparisons, position_index, game_index, turn_index,
              bag_get_letters(game_get_bag(trajectory_game)), top_count,
              overlap, unique_count, samples_per_arm, nominations[0].iterations,
              nominations[1].iterations, nominations[0].nodes,
              nominations[1].nodes, nominations[0].elapsed_seconds,
              nominations[1].elapsed_seconds, klv2_move_string,
              klv3_move_string, klv2_move_raw, klv3_move_raw, best_root,
              utility_delta, stat_get_sem(position_deltas[0]), win_delta,
              spread_delta, stat_get_mean(roots[0].utility),
              stat_get_sem(roots[0].utility), stat_get_mean(roots[0].win_pct),
              stat_get_mean(roots[0].spread), stat_get_mean(roots[1].utility),
              stat_get_sem(roots[1].utility), stat_get_mean(roots[1].win_pct),
              stat_get_mean(roots[1].spread), cgp);

          for (int delta = 0; delta < 3; delta++) {
            stat_destroy(position_deltas[delta]);
          }
          nested_root_stats_destroy(roots, root_count);
          if (aggregate.disagreements % 5 == 0) {
            print_candidate_selector_summary(
                &aggregate, global_comparisons, total_top_candidates,
                total_overlap, total_unique_per_selector,
                ctimer_elapsed_seconds(&timer));
          }
          fflush_or_die(stdout);
        }
      }

      Game *trajectory_policy_game = game_index % 2 == 0
                                         ? config_get_game(klv2_config)
                                         : config_get_game(klv3_config);
      const Move *trajectory_move =
          get_top_equity_move(trajectory_policy_game, trajectory_move_list);
      assert(trajectory_move != NULL);
      play_move(trajectory_move, trajectory_game, NULL);
      free(cgp);
      position_index++;
      turn_index++;
    }
  }

  if (aggregate.disagreements > 0) {
    print_candidate_selector_summary(
        &aggregate, global_comparisons, total_top_candidates, total_overlap,
        total_unique_per_selector, ctimer_elapsed_seconds(&timer));
  }
  printf("KLV3_CANDIDATE_DONE source_positions=%d full_overlap=%d "
         "run_comparisons=%d global_comparisons=%d target=%d last_position=%d "
         "elapsed_seconds=%.6f wall_limit_reached=%d\n",
         aggregate.source_positions, aggregate.agreements,
         aggregate.disagreements, global_comparisons, target_comparisons,
         position_index, ctimer_elapsed_seconds(&timer), wall_limit_reached);
  fflush_or_die(stdout);

  move_list_destroy(trajectory_move_list);
  game_destroy(trajectory_game);
  move_list_destroy(klv3_only);
  move_list_destroy(klv2_only);
  nomination_context_destroy(&nomination_contexts[1]);
  nomination_context_destroy(&nomination_contexts[0]);
  equal_sample_aggregate_destroy(&aggregate);
}

static void run_corpus_oracle(Config *klv2_config, Config *klv3_config,
                              const char *corpus_files, int num_samples,
                              int num_threads, int start_disagreement,
                              int max_disagreements, double wall_seconds) {
  enum {
    CORPUS_FIELD_CAPACITY = 40,
    CORPUS_MIN_FIELDS = 39,
    CORPUS_DISAGREEMENT_FIELD = 1,
    CORPUS_POSITION_FIELD = 2,
    CORPUS_GAME_FIELD = 3,
    CORPUS_TURN_FIELD = 4,
    CORPUS_BAG_FIELD = 5,
    CORPUS_KLV2_MOVE_RAW_FIELD = 11,
    CORPUS_KLV3_MOVE_RAW_FIELD = 13,
    CORPUS_CGP_FIELD = 38,
  };
  OracleAggregate aggregate = oracle_aggregate_create();
  Timer timer;
  ctimer_start(&timer);
  int rows_read = 0;
  int same_root_moves = 0;
  int evaluated = 0;
  int last_disagreement = start_disagreement;
  bool wall_limit_reached = false;
  char *files_copy = string_duplicate(corpus_files);
  char *files_saveptr = NULL;

  printf("KLV3_CORPUS_ORACLE_CONFIG files=%s samples_per_candidate=%d "
         "policies=klv2,klv3 threads=%d start_disagreement=%d "
         "max_disagreements=%d wall_seconds=%.3f\n",
         corpus_files, num_samples, num_threads, start_disagreement,
         max_disagreements, wall_seconds);
  fflush_or_die(stdout);

  for (char *filename = strtok_r(files_copy, ",", &files_saveptr);
       filename != NULL && !wall_limit_reached;
       filename = strtok_r(NULL, ",", &files_saveptr)) {
    FILE *stream = fopen(filename, "re");
    if (stream == NULL) {
      log_fatal("could not open KLV3 oracle corpus file: %s", filename);
    }
    assert(stream != NULL);
    char *line = NULL;
    size_t line_capacity = 0;
    while (getline_ignore_carriage_return(&line, &line_capacity, stream) !=
           -1) {
      if (strncmp(line, "KLV3_THREEWAY_DISAGREEMENT\t", 27) != 0) {
        continue;
      }
      rows_read++;
      char *fields[CORPUS_FIELD_CAPACITY] = {0};
      const int field_count =
          split_tab_fields(line, fields, CORPUS_FIELD_CAPACITY);
      if (field_count < CORPUS_MIN_FIELDS) {
        log_fatal("truncated KLV3 oracle corpus row in %s", filename);
      }
      assert(fields[CORPUS_DISAGREEMENT_FIELD] != NULL);
      assert(fields[CORPUS_CGP_FIELD] != NULL);
      const int disagreement =
          (int)strtol(fields[CORPUS_DISAGREEMENT_FIELD], NULL, 10);
      if (disagreement <= start_disagreement) {
        continue;
      }
      if (max_disagreements > 0 && evaluated >= max_disagreements) {
        break;
      }
      if (wall_seconds > 0.0 &&
          ctimer_elapsed_seconds(&timer) >= wall_seconds) {
        wall_limit_reached = true;
        break;
      }
      fields[CORPUS_CGP_FIELD][strcspn(fields[CORPUS_CGP_FIELD], "\r\n")] =
          '\0';
      Move klv2_move;
      Move klv3_move;
      if (!parse_record_move(fields[CORPUS_KLV2_MOVE_RAW_FIELD], &klv2_move) ||
          !parse_record_move(fields[CORPUS_KLV3_MOVE_RAW_FIELD], &klv3_move)) {
        log_fatal("invalid raw move in KLV3 oracle corpus disagreement %d",
                  disagreement);
      }
      if (moves_are_equal(&klv2_move, &klv3_move)) {
        same_root_moves++;
        last_disagreement = disagreement;
        continue;
      }

      load_position(klv2_config, fields[CORPUS_CGP_FIELD]);
      load_position(klv3_config, fields[CORPUS_CGP_FIELD]);
      const uint64_t position_seed =
          KLV3_ORACLE_BASE_SEED +
          (uint64_t)disagreement * KLV3_ORACLE_SEED_STRIDE;
      const TerminalRolloutResult klv2_policy = run_terminal_rollouts(
          config_get_game(klv2_config), &klv2_move, &klv3_move, num_samples,
          num_threads, position_seed);
      const TerminalRolloutResult klv3_policy = run_terminal_rollouts(
          config_get_game(klv3_config), &klv2_move, &klv3_move, num_samples,
          num_threads, position_seed);
      oracle_aggregate_add(&aggregate, false, &klv2_policy, &klv3_policy);
      evaluated++;
      last_disagreement = disagreement;
      const double ensemble_utility =
          (klv2_policy.utility_delta + klv3_policy.utility_delta) / 2.0;
      const double ensemble_spread =
          (klv2_policy.spread_delta + klv3_policy.spread_delta) / 2.0;
      const double ensemble_win =
          (klv2_policy.win_delta + klv3_policy.win_delta) / 2.0;
      printf("KLV3_CORPUS_ORACLE_POSITION disagreement=%d position=%s game=%s "
             "turn=%s bag=%s klv2_policy_utility=%+.9f "
             "klv2_policy_sem=%.9f klv2_policy_spread=%+.6f "
             "klv2_policy_win=%+.9f klv2_policy_seconds=%.6f "
             "klv3_policy_utility=%+.9f klv3_policy_sem=%.9f "
             "klv3_policy_spread=%+.6f klv3_policy_win=%+.9f "
             "klv3_policy_seconds=%.6f ensemble_utility=%+.9f "
             "ensemble_spread=%+.6f ensemble_win=%+.9f\n",
             disagreement, fields[CORPUS_POSITION_FIELD],
             fields[CORPUS_GAME_FIELD], fields[CORPUS_TURN_FIELD],
             fields[CORPUS_BAG_FIELD], klv2_policy.utility_delta,
             klv2_policy.utility_sem, klv2_policy.spread_delta,
             klv2_policy.win_delta, klv2_policy.elapsed_seconds,
             klv3_policy.utility_delta, klv3_policy.utility_sem,
             klv3_policy.spread_delta, klv3_policy.win_delta,
             klv3_policy.elapsed_seconds, ensemble_utility, ensemble_spread,
             ensemble_win);
      if (evaluated % 25 == 0) {
        print_oracle_summary(&aggregate);
      }
      fflush_or_die(stdout);
    }
    free(line);
    fclose_or_die(stream);
  }
  free(files_copy);
  print_oracle_summary(&aggregate);
  printf("KLV3_CORPUS_ORACLE_DONE rows_read=%d same_root_moves=%d "
         "evaluated=%d last_disagreement=%d elapsed_seconds=%.6f "
         "wall_limit_reached=%d\n",
         rows_read, same_root_moves, evaluated, last_disagreement,
         ctimer_elapsed_seconds(&timer), wall_limit_reached);
  fflush_or_die(stdout);
  oracle_aggregate_destroy(&aggregate);
}

void test_klv3_sim_oracle(void) {
  const int num_games =
      env_positive_int("KLV3_ORACLE_GAMES", KLV3_ORACLE_DEFAULT_GAMES);
  const int start_game = env_nonnegative_int("KLV3_ORACLE_START_GAME", 0);
  const int num_samples =
      env_positive_int("KLV3_ORACLE_SAMPLES", KLV3_ORACLE_DEFAULT_SAMPLES);
  const int max_positions = env_positive_int("KLV3_ORACLE_MAX_POSITIONS",
                                             KLV3_ORACLE_DEFAULT_MAX_POSITIONS);
  const int start_position =
      env_nonnegative_int("KLV3_ORACLE_START_POSITION", 0);
  const int collect_from_position =
      env_nonnegative_int("KLV3_ORACLE_COLLECT_FROM_POSITION", start_position);
  const int num_threads =
      env_positive_int("KLV3_ORACLE_THREADS", get_num_cores());
  const int nomination_repetitions =
      env_positive_int("KLV3_ORACLE_NOMINATIONS", 1);
  const int min_play_iterations = env_positive_int(
      "KLV3_ORACLE_MIN_PLAY_ITERATIONS", KLV3_ORACLE_MIN_PLAY_ITERATIONS);
  const uint64_t max_iterations =
      (uint64_t)env_positive_int("KLV3_ORACLE_MAX_ITERATIONS", INT32_MAX);
  const bool calibration_only = env_bool("KLV3_ORACLE_CALIBRATION_ONLY", false);
  const bool three_way = env_bool("KLV3_ORACLE_THREE_WAY", false);
  const bool equal_sample_online =
      env_bool("KLV3_ORACLE_EQUAL_SAMPLE_ONLINE", false);
  const bool candidate_selector_online =
      env_bool("KLV3_ORACLE_CANDIDATE_SELECTOR_ONLINE", false);
  if (equal_sample_online && candidate_selector_online) {
    log_fatal("equal-sample rollout-policy and candidate-selector modes are "
              "mutually exclusive");
  }
  const int target_disagreements = env_positive_int(
      "KLV3_ORACLE_TARGET_DISAGREEMENTS",
      equal_sample_online || candidate_selector_online ? 500 : 10000);
  const int start_disagreements =
      env_nonnegative_int("KLV3_ORACLE_START_DISAGREEMENTS", 0);
  if (start_game >= num_games) {
    log_fatal("KLV3_ORACLE_START_GAME must be less than KLV3_ORACLE_GAMES");
  }
  if (start_position > collect_from_position ||
      collect_from_position >= max_positions) {
    log_fatal("oracle positions must satisfy start <= collect-from < max");
  }
  if (start_disagreements >= target_disagreements) {
    log_fatal("KLV3_ORACLE_START_DISAGREEMENTS must be less than the target");
  }
  const bool compare_pruning = env_bool("KLV3_ORACLE_COMPARE_PRUNING", false);
  const char *corpus_files = getenv("KLV3_ORACLE_CORPUS_FILES");
  const bool nested_corpus_oracle = env_bool("KLV3_ORACLE_NESTED", false);
  const int corpus_max_disagreements =
      env_nonnegative_int("KLV3_ORACLE_CORPUS_MAX_DISAGREEMENTS", 0);
  const int nested_outer_samples =
      env_positive_int("KLV3_ORACLE_OUTER_SAMPLES", 10000);
  const int nested_outer_plies =
      env_positive_int("KLV3_ORACLE_OUTER_PLIES", KLV3_ORACLE_OUTER_PLIES);
  const int nested_samples_per_candidate =
      env_positive_int("KLV3_ORACLE_NESTED_SAMPLES", 100);
  const int nested_min_bag_tiles =
      env_nonnegative_int("KLV3_ORACLE_NESTED_MIN_BAG", 29);
  const int fixed_samples_per_arm =
      env_positive_int("KLV3_ORACLE_SAMPLES_PER_ARM", 1500);
  double corpus_wall_seconds = 0.0;
  (void)env_optional_nonnegative_double("KLV3_ORACLE_WALL_SECONDS",
                                        &corpus_wall_seconds);
  const double seconds_per_turn = env_positive_double(
      "KLV3_ORACLE_TLIM", KLV3_ORACLE_DEFAULT_SECONDS_PER_TURN);
  double pruning_cap = 0.0;
  const bool pruning_cap_enabled =
      env_optional_nonnegative_double("KLV3_ORACLE_PRUNE_CAP", &pruning_cap);
  double fallback_threshold = -1.0;
  const bool fallback_threshold_enabled = env_optional_nonnegative_double(
      "KLV3_ORACLE_FALLBACK_THRESHOLD", &fallback_threshold);
  const char *baseline_klv_name =
      env_string("KLV3_ORACLE_BASELINE_KLV",
                 compare_pruning ? "CSW24_klv3_ctx400" : "CSW24");
  const char *candidate_klv_name = env_string(
      "KLV3_ORACLE_CANDIDATE_KLV", compare_pruning && !pruning_cap_enabled
                                       ? "CSW24_klv3_ctx400_p99100"
                                       : "CSW24_klv3_ctx400");
  Config *klv2_config =
      create_oracle_config(baseline_klv_name, num_threads, seconds_per_turn,
                           min_play_iterations, max_iterations,
                           /*fallback_threshold=*/-1.0);
  Config *klv3_config = create_oracle_config(
      candidate_klv_name, num_threads, seconds_per_turn, min_play_iterations,
      max_iterations, three_way ? -1.0 : fallback_threshold);
  Config *hybrid_config =
      three_way ? create_oracle_config(
                      candidate_klv_name, num_threads, seconds_per_turn,
                      min_play_iterations, max_iterations,
                      fallback_threshold_enabled ? fallback_threshold : 1.5)
                : NULL;
  if (candidate_selector_online) {
    if (three_way || (corpus_files != NULL && corpus_files[0] != '\0')) {
      log_fatal("candidate-selector online oracle cannot be combined with "
                "three-way or corpus-file modes");
    }
    if (nested_samples_per_candidate % 2 != 0) {
      log_fatal("KLV3_ORACLE_NESTED_SAMPLES must be even");
    }
    run_candidate_selector_online_oracle(
        klv2_config, klv3_config, num_games, start_game, max_positions,
        start_position, collect_from_position, start_disagreements,
        target_disagreements, fixed_samples_per_arm, nested_outer_samples,
        nested_outer_plies, nested_samples_per_candidate, num_threads,
        nested_min_bag_tiles, corpus_wall_seconds);
    config_destroy(hybrid_config);
    config_destroy(klv3_config);
    config_destroy(klv2_config);
    return;
  }
  if (equal_sample_online) {
    if (three_way || (corpus_files != NULL && corpus_files[0] != '\0')) {
      log_fatal("equal-sample online oracle cannot be combined with "
                "three-way or corpus-file modes");
    }
    if (nested_samples_per_candidate % 2 != 0) {
      log_fatal("KLV3_ORACLE_NESTED_SAMPLES must be even");
    }
    run_equal_sample_online_oracle(
        klv2_config, klv3_config, num_games, start_game, max_positions,
        start_position, collect_from_position, start_disagreements,
        target_disagreements, fixed_samples_per_arm, nested_outer_samples,
        nested_outer_plies, nested_samples_per_candidate, num_threads,
        nested_min_bag_tiles, corpus_wall_seconds);
    config_destroy(hybrid_config);
    config_destroy(klv3_config);
    config_destroy(klv2_config);
    return;
  }
  if (corpus_files != NULL && corpus_files[0] != '\0') {
    if (three_way) {
      log_fatal("KLV3_ORACLE_CORPUS_FILES cannot be combined with three-way "
                "nomination collection");
    }
    if (nested_corpus_oracle) {
      if (nested_samples_per_candidate % 2 != 0) {
        log_fatal("KLV3_ORACLE_NESTED_SAMPLES must be even");
      }
      run_nested_corpus_oracle(klv2_config, klv3_config, corpus_files,
                               nested_outer_samples, nested_outer_plies,
                               nested_samples_per_candidate, num_threads,
                               start_disagreements, corpus_max_disagreements,
                               nested_min_bag_tiles, corpus_wall_seconds);
    } else {
      run_corpus_oracle(klv2_config, klv3_config, corpus_files, num_samples,
                        num_threads, start_disagreements,
                        corpus_max_disagreements, corpus_wall_seconds);
    }
    config_destroy(hybrid_config);
    config_destroy(klv3_config);
    config_destroy(klv2_config);
    return;
  }
  if (pruning_cap_enabled) {
    Game *klv3_game = config_get_game(klv3_config);
    for (int player_index = 0; player_index < 2; player_index++) {
      KLV *klv =
          (KLV *)player_get_klv(game_get_player(klv3_game, player_index));
      klv_set_context_pruning_cap(klv, double_to_equity(pruning_cap));
    }
  }
  // Keep the position trajectory exact and identical across pruning runs.
  // The timed candidate may be lossy, but it never chooses a downstream
  // corpus position. This covers both the old global-cap experiment and the
  // serialized per-leave capped-RIT implementation.
  Config *klv3_trajectory_config =
      compare_pruning
          ? create_oracle_config("CSW24_klv3_ctx400", num_threads,
                                 seconds_per_turn, min_play_iterations,
                                 max_iterations,
                                 /*fallback_threshold=*/-1.0)
          : klv3_config;
  NominationContext klv2_nomination;
  NominationContext klv3_nomination;
  NominationContext hybrid_nomination;
  nomination_context_init(&klv2_nomination, klv2_config);
  nomination_context_init(&klv3_nomination, klv3_config);
  if (three_way) {
    nomination_context_init(&hybrid_nomination, hybrid_config);
  }
  Game *trajectory_game = config_game_create(klv2_config);
  MoveList *trajectory_move_list = move_list_create(1);
  OracleAggregate aggregate = oracle_aggregate_create();
  NominationAggregate nomination_aggregate = {
      .repetitions = nomination_repetitions,
  };
  NominationResult *klv2_nominations =
      malloc_or_die(sizeof(*klv2_nominations) * (size_t)nomination_repetitions);
  NominationResult *klv3_nominations =
      malloc_or_die(sizeof(*klv3_nominations) * (size_t)nomination_repetitions);
  NominationResult *hybrid_nominations =
      three_way ? malloc_or_die(sizeof(*hybrid_nominations) *
                                (size_t)nomination_repetitions)
                : NULL;
  const RackInfoTable *baseline_rit = player_get_rack_info_table(
      game_get_player(config_get_game(klv2_config), 0));
  const RackInfoTable *candidate_rit = player_get_rack_info_table(
      game_get_player(config_get_game(klv3_config), 0));

  printf("KLV3_ORACLE_CONFIG games=%d start_game=%d max_positions=%d "
         "start_position=%d collect_from_position=%d "
         "start_disagreements=%d samples=%d "
         "threads=%d short_tlim=%.3f nominations=%d calibration_only=%d "
         "min_play_iterations=%d max_iterations=%" PRIu64 " compare_pruning=%d "
         "baseline_klv=%s candidate_klv=%s "
         "baseline_rit=%s baseline_rit_word_only=%d "
         "candidate_rit=%s candidate_rit_word_only=%d "
         "pruning_cap=%s%.3f fallback_threshold=%s%.3f "
         "three_way=%d target_disagreements=%d\n",
         num_games, start_game, max_positions, start_position,
         collect_from_position, start_disagreements, num_samples, num_threads,
         seconds_per_turn, nomination_repetitions, calibration_only,
         min_play_iterations, max_iterations, compare_pruning,
         baseline_klv_name, candidate_klv_name,
         rack_info_table_get_name(baseline_rit),
         rack_info_table_is_word_only(baseline_rit),
         rack_info_table_get_name(candidate_rit),
         rack_info_table_is_word_only(candidate_rit),
         pruning_cap_enabled ? "" : "disabled/", pruning_cap,
         fallback_threshold_enabled ? "" : "disabled/", fallback_threshold,
         three_way, target_disagreements);
  if (three_way) {
    printf("KLV3_THREEWAY_COLUMNS\tdisagreement\tposition\tgame\tturn\tbag\t"
           "sampled_adjustment_magnitude\tcontext_adjustment_range\t"
           "klv2_move\thybrid_move\tklv3_move\t"
           "klv2_move_raw\thybrid_move_raw\tklv3_move_raw\t"
           "klv2_iterations\thybrid_iterations\tklv3_iterations\t"
           "klv2_nodes\thybrid_nodes\tklv3_nodes\t"
           "klv2_elapsed_seconds\thybrid_elapsed_seconds\t"
           "klv3_elapsed_seconds\t"
           "klv2_win_pct\thybrid_win_pct\tklv3_win_pct\t"
           "klv2_win_pct_sem\thybrid_win_pct_sem\tklv3_win_pct_sem\t"
           "klv2_equity\thybrid_equity\tklv3_equity\t"
           "klv2_equity_sem\thybrid_equity_sem\tklv3_equity_sem\t"
           "klv2_utility\thybrid_utility\tklv3_utility\tcgp\n");
  }
  int position_index = start_position;
  int three_way_disagreements = start_disagreements;
  uint64_t hybrid_iterations = 0;
  double hybrid_elapsed_seconds = 0.0;
  for (int game_index = start_game;
       game_index < num_games && position_index < max_positions &&
       (!three_way || three_way_disagreements < target_disagreements);
       game_index++) {
    game_reset(trajectory_game);
    game_seed(trajectory_game,
              KLV3_ORACLE_BASE_SEED +
                  (uint64_t)game_index * KLV3_ORACLE_SEED_STRIDE);
    game_set_starting_player_index(trajectory_game, game_index % 2);
    draw_starting_racks(trajectory_game);
    int turn_index = 0;
    while (!game_over(trajectory_game) &&
           bag_get_letters(game_get_bag(trajectory_game)) > PEG_MAX_BAG &&
           position_index < max_positions &&
           (!three_way || three_way_disagreements < target_disagreements)) {
      char *cgp = game_get_cgp(trajectory_game, true);
      load_position(klv2_config, cgp);
      load_position(klv3_config, cgp);
      if (three_way) {
        load_position(hybrid_config, cgp);
      }
      if (klv3_trajectory_config != klv3_config) {
        load_position(klv3_trajectory_config, cgp);
      }

      if (position_index >= collect_from_position) {
        int paired_disagreements = 0;
        for (int repetition = 0; repetition < nomination_repetitions;
             repetition++) {
          // Rotate model order so a consistent order cannot turn thermal drift
          // into a model speed or nomination difference.
          if (three_way) {
            switch ((position_index + repetition) % 3) {
            case 0:
              klv2_nominations[repetition] =
                  nominate_move(klv2_config, &klv2_nomination);
              hybrid_nominations[repetition] =
                  nominate_move(hybrid_config, &hybrid_nomination);
              klv3_nominations[repetition] =
                  nominate_move(klv3_config, &klv3_nomination);
              break;
            case 1:
              hybrid_nominations[repetition] =
                  nominate_move(hybrid_config, &hybrid_nomination);
              klv3_nominations[repetition] =
                  nominate_move(klv3_config, &klv3_nomination);
              klv2_nominations[repetition] =
                  nominate_move(klv2_config, &klv2_nomination);
              break;
            default:
              klv3_nominations[repetition] =
                  nominate_move(klv3_config, &klv3_nomination);
              klv2_nominations[repetition] =
                  nominate_move(klv2_config, &klv2_nomination);
              hybrid_nominations[repetition] =
                  nominate_move(hybrid_config, &hybrid_nomination);
              break;
            }
            hybrid_iterations += hybrid_nominations[repetition].iterations;
            hybrid_elapsed_seconds +=
                hybrid_nominations[repetition].elapsed_seconds;
          } else if ((position_index + repetition) % 2 == 0) {
            klv2_nominations[repetition] =
                nominate_move(klv2_config, &klv2_nomination);
            klv3_nominations[repetition] =
                nominate_move(klv3_config, &klv3_nomination);
          } else {
            klv3_nominations[repetition] =
                nominate_move(klv3_config, &klv3_nomination);
            klv2_nominations[repetition] =
                nominate_move(klv2_config, &klv2_nomination);
          }
          nomination_aggregate.iterations[0] +=
              klv2_nominations[repetition].iterations;
          nomination_aggregate.iterations[1] +=
              klv3_nominations[repetition].iterations;
          nomination_aggregate.elapsed_seconds[0] +=
              klv2_nominations[repetition].elapsed_seconds;
          nomination_aggregate.elapsed_seconds[1] +=
              klv3_nominations[repetition].elapsed_seconds;
          if (!moves_are_equal(&klv2_nominations[repetition].move,
                               &klv3_nominations[repetition].move)) {
            paired_disagreements++;
          }
        }
        const int klv2_unique = count_unique_nomination_moves(
            klv2_nominations, nomination_repetitions);
        const int klv3_unique = count_unique_nomination_moves(
            klv3_nominations, nomination_repetitions);
        const int hybrid_unique =
            three_way ? count_unique_nomination_moves(hybrid_nominations,
                                                      nomination_repetitions)
                      : 0;
        nomination_aggregate.positions++;
        nomination_aggregate.paired_disagreements += paired_disagreements;
        nomination_aggregate.klv2_stable_positions += klv2_unique == 1;
        nomination_aggregate.klv3_stable_positions += klv3_unique == 1;
        const Move klv2_move = klv2_nominations[0].move;
        const Move klv3_move = klv3_nominations[0].move;
        const Move hybrid_move =
            three_way ? hybrid_nominations[0].move : klv2_move;
        const bool oracle_eligible = klv2_unique == 1 && klv3_unique == 1;
        const bool same_move = moves_are_equal(&klv2_move, &klv3_move);
        const bool three_way_eligible =
            three_way && oracle_eligible && hybrid_unique == 1;
        const bool three_way_same =
            same_move && moves_are_equal(&klv2_move, &hybrid_move);
        Equity sampled_adjustment_magnitude;
        Equity context_adjustment_range;
        get_context_adjustment_metrics(config_get_game(klv3_config),
                                       &sampled_adjustment_magnitude,
                                       &context_adjustment_range);
        nomination_aggregate.stable_model_disagreements +=
            oracle_eligible && !same_move;
        char klv2_move_string[64];
        char klv3_move_string[64];
        move_to_string(config_get_game(klv2_config), &klv2_move,
                       klv2_move_string, sizeof(klv2_move_string));
        move_to_string(config_get_game(klv3_config), &klv3_move,
                       klv3_move_string, sizeof(klv3_move_string));
        char hybrid_move_string[64] = "";
        if (three_way) {
          move_to_string(config_get_game(hybrid_config), &hybrid_move,
                         hybrid_move_string, sizeof(hybrid_move_string));
        }

        if (three_way_eligible && !three_way_same) {
          char klv2_move_raw[256];
          char hybrid_move_raw[256];
          char klv3_move_raw[256];
          move_to_record_encoding(&klv2_move, klv2_move_raw,
                                  sizeof(klv2_move_raw));
          move_to_record_encoding(&hybrid_move, hybrid_move_raw,
                                  sizeof(hybrid_move_raw));
          move_to_record_encoding(&klv3_move, klv3_move_raw,
                                  sizeof(klv3_move_raw));
          three_way_disagreements++;
          printf("KLV3_THREEWAY_DISAGREEMENT\t%d\t%d\t%d\t%d\t%d\t%.3f\t"
                 "%.3f\t%s\t%s\t%s\t%s\t%s\t%s\t"
                 "%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t"
                 "%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t"
                 "%.9f\t%.9f\t%.9f\t"
                 "%.9f\t%.9f\t%.9f\t%.9f\t%.9f\t%.9f\t"
                 "%.9f\t%.9f\t%.9f\t%.9f\t%.9f\t%.9f\t"
                 "%.9f\t%.9f\t%.9f\t%s\n",
                 three_way_disagreements, position_index, game_index,
                 turn_index, bag_get_letters(game_get_bag(trajectory_game)),
                 equity_to_double(sampled_adjustment_magnitude),
                 equity_to_double(context_adjustment_range), klv2_move_string,
                 hybrid_move_string, klv3_move_string, klv2_move_raw,
                 hybrid_move_raw, klv3_move_raw, klv2_nominations[0].iterations,
                 hybrid_nominations[0].iterations,
                 klv3_nominations[0].iterations, klv2_nominations[0].nodes,
                 hybrid_nominations[0].nodes, klv3_nominations[0].nodes,
                 klv2_nominations[0].elapsed_seconds,
                 hybrid_nominations[0].elapsed_seconds,
                 klv3_nominations[0].elapsed_seconds,
                 klv2_nominations[0].best_win_pct,
                 hybrid_nominations[0].best_win_pct,
                 klv3_nominations[0].best_win_pct,
                 klv2_nominations[0].best_win_pct_sem,
                 hybrid_nominations[0].best_win_pct_sem,
                 klv3_nominations[0].best_win_pct_sem,
                 klv2_nominations[0].best_equity,
                 hybrid_nominations[0].best_equity,
                 klv3_nominations[0].best_equity,
                 klv2_nominations[0].best_equity_sem,
                 hybrid_nominations[0].best_equity_sem,
                 klv3_nominations[0].best_equity_sem,
                 klv2_nominations[0].best_utility,
                 hybrid_nominations[0].best_utility,
                 klv3_nominations[0].best_utility, cgp);
        }

        TerminalRolloutResult klv2_policy = {0};
        TerminalRolloutResult klv3_policy = {0};
        if (!calibration_only && oracle_eligible && !same_move) {
          const uint64_t position_seed =
              KLV3_ORACLE_BASE_SEED +
              (uint64_t)(position_index + 1) * KLV3_ORACLE_SEED_STRIDE;
          klv2_policy = run_terminal_rollouts(
              config_get_game(klv2_config), &klv2_move, &klv3_move, num_samples,
              num_threads, position_seed);
          klv3_policy = run_terminal_rollouts(
              config_get_game(klv3_config), &klv2_move, &klv3_move, num_samples,
              num_threads, position_seed);
        }
        if (!calibration_only && oracle_eligible) {
          oracle_aggregate_add(&aggregate, same_move, &klv2_policy,
                               &klv3_policy);
        }
        if (!three_way) {
          printf(
              "KLV3_ORACLE_POSITION index=%d game=%d turn=%d bag=%d "
              "sampled_adjustment_magnitude=%.3f "
              "context_adjustment_range=%.3f oracle_eligible=%d same=%d "
              "baseline_move=%s candidate_move=%s "
              "baseline_unique=%d candidate_unique=%d paired_disagreements=%d "
              "klv2_policy_utility_delta=%+.6f sem=%.6f "
              "spread_delta=%+.3f win_delta=%+.6f seconds=%.3f "
              "klv3_policy_utility_delta=%+.6f sem=%.6f "
              "spread_delta=%+.3f win_delta=%+.6f seconds=%.3f\n",
              position_index, game_index, turn_index,
              bag_get_letters(game_get_bag(trajectory_game)),
              equity_to_double(sampled_adjustment_magnitude),
              equity_to_double(context_adjustment_range), oracle_eligible,
              same_move, klv2_move_string, klv3_move_string, klv2_unique,
              klv3_unique, paired_disagreements, klv2_policy.utility_delta,
              klv2_policy.utility_sem, klv2_policy.spread_delta,
              klv2_policy.win_delta, klv2_policy.elapsed_seconds,
              klv3_policy.utility_delta, klv3_policy.utility_sem,
              klv3_policy.spread_delta, klv3_policy.win_delta,
              klv3_policy.elapsed_seconds);
        }
        if (!calibration_only) {
          print_oracle_summary(&aggregate);
        }
        if (!three_way) {
          print_nomination_summary(&nomination_aggregate);
        } else if (position_index % 100 == 99 ||
                   three_way_disagreements >= target_disagreements) {
          const double positions = (double)(position_index + 1);
          const double hybrid_rate =
              hybrid_elapsed_seconds > 0.0
                  ? (double)hybrid_iterations / hybrid_elapsed_seconds
                  : 0.0;
          printf("KLV3_THREEWAY_SUMMARY positions=%d disagreements=%d "
                 "rate=%.6f klv2_iters_per_second=%.3f "
                 "hybrid_iters_per_second=%.3f klv3_iters_per_second=%.3f\n",
                 position_index + 1, three_way_disagreements,
                 (double)three_way_disagreements / positions,
                 nomination_aggregate.elapsed_seconds[0] > 0.0
                     ? (double)nomination_aggregate.iterations[0] /
                           nomination_aggregate.elapsed_seconds[0]
                     : 0.0,
                 hybrid_rate,
                 nomination_aggregate.elapsed_seconds[1] > 0.0
                     ? (double)nomination_aggregate.iterations[1] /
                           nomination_aggregate.elapsed_seconds[1]
                     : 0.0);
          fflush_or_die(stdout);
        }
      }

      // Do not let the wall-clock-limited nominations choose the next corpus
      // position: small timing differences could then change every downstream
      // position.  Alternate deterministic KLV2- and KLV3-static games so the
      // benchmark corpus represents both policies and is repeatable.
      Game *trajectory_policy_game =
          game_index % 2 == 0 ? config_get_game(klv2_config)
                              : config_get_game(klv3_trajectory_config);
      const Move *trajectory_move =
          get_top_equity_move(trajectory_policy_game, trajectory_move_list);
      assert(trajectory_move != NULL);
      play_move(trajectory_move, trajectory_game, NULL);
      free(cgp);
      position_index++;
      turn_index++;
    }
  }

  if (!calibration_only) {
    print_oracle_summary(&aggregate);
  }
  if (!three_way) {
    print_nomination_summary(&nomination_aggregate);
  } else {
    printf("KLV3_THREEWAY_DONE positions=%d disagreements=%d target=%d\n",
           position_index, three_way_disagreements, target_disagreements);
    fflush_or_die(stdout);
  }
  free(klv2_nominations);
  free(klv3_nominations);
  free(hybrid_nominations);
  oracle_aggregate_destroy(&aggregate);
  move_list_destroy(trajectory_move_list);
  game_destroy(trajectory_game);
  nomination_context_destroy(&klv2_nomination);
  nomination_context_destroy(&klv3_nomination);
  if (three_way) {
    nomination_context_destroy(&hybrid_nomination);
  }
  config_destroy(klv2_config);
  if (klv3_trajectory_config != klv3_config) {
    config_destroy(klv3_trajectory_config);
  }
  config_destroy(hybrid_config);
  config_destroy(klv3_config);
}
