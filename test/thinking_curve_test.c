#include "thinking_curve_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/bai_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/analysis_trace.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Observation-only calibration harness for the anytime simulation curve.
//
// Each budget is an independent BAI arm with a proportional uniform-sampling
// floor. Reusing checkpoints from one long run would let early BAI noise lock
// in a poor arm and would not represent the allocation used at that budget.
// The source corpus's oracle values are retained only as diagnostics. A
// stronger online 10-ply judge adjudicates the distinct budget nominees plus,
// when requested, a checkpoint-observable risk set selected only from the
// arm's contemporaneous means, variances, and sample counts. The judge never
// participates in nomination or stopping.

enum {
  THINKING_CURVE_MAX_CANDIDATES = 128,
  THINKING_CURVE_MAX_TARGETS = 32,
  THINKING_CURVE_RAW_MOVE_CAPACITY = 160,
};

static const uint64_t THINKING_CURVE_BASE_SEED = UINT64_C(0x4b4c56334f524143);
static const uint64_t THINKING_CURVE_SEED_STRIDE = UINT64_C(0x9e3779b97f4a7c15);
static const uint64_t THINKING_CURVE_NOMINATION_SEED_XOR =
    UINT64_C(0xa0761d6478bd642f);

static bool thinking_curve_bai_can_finish_before_sample_limit(
    bai_result_status_t status) {
  return status == BAI_RESULT_STATUS_REGRET_LIMIT ||
         status == BAI_RESULT_STATUS_THRESHOLD;
}

typedef struct ThinkingCurveCandidate {
  Move move;
  char raw_move[THINKING_CURVE_RAW_MOVE_CAPACITY];
  double oracle_utility;
  double oracle_win;
  double oracle_spread;
} ThinkingCurveCandidate;

typedef struct ThinkingCurveContext {
  SimCtx *sim_ctx;
  SimResults *sim_results;
  Rack known_opponent_rack;
} ThinkingCurveContext;

typedef struct ThinkingCurveJudgeResult {
  double utility[THINKING_CURVE_MAX_CANDIDATES];
  double utility_sem[THINKING_CURVE_MAX_CANDIDATES];
  double win[THINKING_CURVE_MAX_CANDIDATES];
  double spread[THINKING_CURVE_MAX_CANDIDATES];
  int best_rank;
  int candidate_count;
  uint64_t iterations;
  uint64_t nodes;
  double elapsed_seconds;
  bool forced;
} ThinkingCurveJudgeResult;

typedef struct ThinkingCurveSampleTrace {
  atomic_uint_fast64_t count;
  atomic_uint_fast64_t dropped;
  uint64_t capacity;
  SimSampleEvent *events;
} ThinkingCurveSampleTrace;

typedef struct ThinkingCurveArmResult {
  AnalysisProgressEvent decision;
  bai_result_status_t bai_status;
  double estimated_regret;
  double regret_at_stop;
  uint64_t sample_counts[THINKING_CURVE_MAX_CANDIDATES];
  double utility_means[THINKING_CURVE_MAX_CANDIDATES];
  double utility_variances[THINKING_CURVE_MAX_CANDIDATES];
  double win_means[THINKING_CURVE_MAX_CANDIDATES];
  double win_variances[THINKING_CURVE_MAX_CANDIDATES];
  double spread_means[THINKING_CURVE_MAX_CANDIDATES];
  double spread_variances[THINKING_CURVE_MAX_CANDIDATES];
  int rank_by_arm[THINKING_CURVE_MAX_CANDIDATES];
  ThinkingCurveSampleTrace *sample_trace;
  int probe_count;
  int probe_rank_by_arm[THINKING_CURVE_MAX_CANDIDATES];
  uint64_t probe_iterations;
  uint64_t probe_nodes;
  double probe_seconds;
} ThinkingCurveArmResult;

static ThinkingCurveSampleTrace *
thinking_curve_sample_trace_create(uint64_t capacity) {
  if (capacity > SIZE_MAX / sizeof(SimSampleEvent)) {
    log_fatal("thinking-curve sample trace capacity is too large");
  }
  ThinkingCurveSampleTrace *trace =
      malloc_or_die(sizeof(ThinkingCurveSampleTrace));
  atomic_init(&trace->count, 0);
  atomic_init(&trace->dropped, 0);
  trace->capacity = capacity;
  trace->events =
      capacity > 0 ? malloc_or_die(capacity * sizeof(*trace->events)) : NULL;
  return trace;
}

static void
thinking_curve_sample_trace_destroy(ThinkingCurveSampleTrace *trace) {
  if (trace == NULL) {
    return;
  }
  free(trace->events);
  free(trace);
}

static void thinking_curve_sample_trace_record(const SimSampleEvent *event,
                                               void *user_data) {
  ThinkingCurveSampleTrace *trace = user_data;
  const uint64_t index = atomic_fetch_add(&trace->count, 1);
  if (index < trace->capacity) {
    trace->events[index] = *event;
  } else {
    atomic_fetch_add(&trace->dropped, 1);
  }
}

static int thinking_curve_env_positive_int(const char *name,
                                           int default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  char *end = NULL;
  const long parsed = strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed <= 0 || parsed > INT32_MAX) {
    log_fatal("%s must be a positive integer", name);
  }
  return (int)parsed;
}

static int thinking_curve_env_nonnegative_int(const char *name,
                                              int default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  char *end = NULL;
  const long parsed = strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < 0 || parsed > INT32_MAX) {
    log_fatal("%s must be a nonnegative integer", name);
  }
  return (int)parsed;
}

static uint64_t thinking_curve_env_positive_uint64(const char *name,
                                                   uint64_t default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  char *end = NULL;
  const unsigned long long parsed = strtoull(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0) {
    log_fatal("%s must be a positive integer", name);
  }
  return (uint64_t)parsed;
}

static double thinking_curve_env_nonnegative_double(const char *name,
                                                    double default_value) {
  const char *value = getenv(name);
  if (value == NULL) {
    return default_value;
  }
  char *end = NULL;
  const double parsed = strtod(value, &end);
  if (end == value || *end != '\0' || parsed < 0.0 || !isfinite(parsed)) {
    log_fatal("%s must be a nonnegative finite number", name);
  }
  return parsed;
}

static const char *thinking_curve_env_string(const char *name,
                                             const char *default_value) {
  const char *value = getenv(name);
  return value == NULL || value[0] == '\0' ? default_value : value;
}

static int thinking_curve_parse_targets(const char *value, uint64_t targets[]) {
  char *copy = string_duplicate(value);
  char *saveptr = NULL;
  int count = 0;
  uint64_t previous = 0;
  for (char *token = strtok_r(copy, ",", &saveptr); token != NULL;
       token = strtok_r(NULL, ",", &saveptr)) {
    if (count >= THINKING_CURVE_MAX_TARGETS) {
      log_fatal("THINKING_CURVE_TARGET_NODES exceeds %d entries",
                THINKING_CURVE_MAX_TARGETS);
    }
    char *end = NULL;
    const unsigned long long parsed = strtoull(token, &end, 10);
    if (end == token || *end != '\0' || parsed == 0 ||
        (count > 0 && (uint64_t)parsed <= previous)) {
      log_fatal("THINKING_CURVE_TARGET_NODES must be strictly increasing "
                "positive integers");
    }
    targets[count++] = (uint64_t)parsed;
    previous = (uint64_t)parsed;
  }
  free(copy);
  if (count == 0) {
    log_fatal("THINKING_CURVE_TARGET_NODES must not be empty");
  }
  return count;
}

static bool thinking_curve_parse_record_move(const char *encoding, Move *move) {
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
    char *end = NULL;
    const long parsed = strtol(token, &end, 10);
    if (end == token || *end != '\0' || parsed < INT32_MIN ||
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

static char *thinking_curve_copy_field(const char *line,
                                       const char *field_name) {
  const char *start = strstr(line, field_name);
  if (start == NULL) {
    return NULL;
  }
  start += strlen(field_name);
  const char *end = strchr(start, ' ');
  if (end == NULL) {
    end = start + strcspn(start, "\r\n");
  }
  const size_t length = (size_t)(end - start);
  char *field = malloc_or_die(length + 1);
  memcpy(field, start, length);
  field[length] = '\0';
  return field;
}

static void thinking_curve_load_position(Config *config, const char *cgp) {
  ErrorStack *error_stack = error_stack_create();
  game_load_cgp(config_get_game(config), cgp, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to load thinking-curve position");
  }
  error_stack_destroy(error_stack);
}

static void thinking_curve_context_init(ThinkingCurveContext *context,
                                        const Config *config) {
  context->sim_ctx = NULL;
  context->sim_results = sim_results_create(0.0);
  rack_set_dist_size_and_reset(&context->known_opponent_rack,
                               ld_get_size(config_get_ld(config)));
}

static void thinking_curve_context_destroy(ThinkingCurveContext *context) {
  sim_ctx_destroy(context->sim_ctx);
  sim_results_destroy(context->sim_results);
}

static Config *thinking_curve_create_config(int num_threads, int num_plays) {
  char command[768];
  (void)snprintf(
      command, sizeof(command),
      "set -lex CSW24 -k1 CSW24 -k2 CSW24 -wmp true -rit true "
      "-ritmmap true -wit true -winpct winpct -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays %d -threads %d -pfrequency 0 "
      "-hr false -savesettings false -autosavegcg false -fgrequired false "
      "-sinfer false -seed %" PRIu64,
      num_plays, num_threads, THINKING_CURVE_BASE_SEED);
  Config *config = config_create_or_die(command);
  // Allocate the reusable Game and MoveList before replacing the position.
  load_and_exec_config_or_die(config, "newgame");
  load_and_exec_config_or_die(config, "rack AEINRST");
  load_and_exec_config_or_die(config, "generate");
  return config;
}

static const AnalysisProgressEvent *
thinking_curve_find_finish(const AnalysisProgressEvent events[], size_t count) {
  for (size_t index = count; index > 0; index--) {
    const AnalysisProgressEvent *event = &events[index - 1];
    if (event->mode == ANALYSIS_MODE_SIM &&
        event->event == ANALYSIS_EVENT_FINISH && event->best_index >= 0) {
      return event;
    }
  }
  return NULL;
}

static int
thinking_curve_find_candidate_rank(const ThinkingCurveCandidate candidates[],
                                   int candidate_count, const Move *move) {
  for (int rank = 0; rank < candidate_count; rank++) {
    if (compare_moves_without_equity(move, &candidates[rank].move, true) ==
        -1) {
      return rank;
    }
  }
  log_fatal("thinking-curve simulation returned a move outside its panel");
  return -1;
}

static ThinkingCurveJudgeResult thinking_curve_run_judge(
    Config *config, ThinkingCurveContext *context, MoveList *candidate_moves,
    const ThinkingCurveCandidate candidates[], const bool selected_ranks[],
    int candidate_count, int judge_plies, uint64_t samples_per_candidate,
    int position) {
  ThinkingCurveJudgeResult result = {
      .best_rank = -1,
  };
  for (int rank = 0; rank < THINKING_CURVE_MAX_CANDIDATES; rank++) {
    result.utility[rank] = NAN;
    result.utility_sem[rank] = NAN;
    result.win[rank] = NAN;
    result.spread[rank] = NAN;
  }
  int judge_ranks[THINKING_CURVE_MAX_CANDIDATES];
  for (int rank = 0; rank < candidate_count; rank++) {
    if (selected_ranks[rank]) {
      judge_ranks[result.candidate_count++] = rank;
    }
  }
  if (result.candidate_count == 0) {
    log_fatal("thinking-curve judge has no checkpoint candidates");
  }
  if (result.candidate_count == 1) {
    const int rank = judge_ranks[0];
    result.utility[rank] = 0.0;
    result.utility_sem[rank] = 0.0;
    result.win[rank] = 0.0;
    result.spread[rank] = 0.0;
    result.best_rank = rank;
    result.forced = true;
    return result;
  }

  const Game *game = config_get_game(config);
  const int player_index = game_get_player_on_turn_index(game);
  move_list_reset(candidate_moves);
  move_list_set_rack(candidate_moves,
                     player_get_rack(game_get_player(game, player_index)));
  for (int index = 0; index < result.candidate_count; index++) {
    move_list_add_move(candidate_moves, &candidates[judge_ranks[index]].move);
  }

  ThreadControl *thread_control = config_get_thread_control(config);
  assert(
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED));
  rack_set_dist_size_and_reset(&context->known_opponent_rack,
                               ld_get_size(config_get_ld(config)));
  const uint64_t total_iterations =
      samples_per_candidate * (uint64_t)result.candidate_count;
  SimArgs judge_args = {0};
  sim_args_fill(judge_plies, candidate_moves, result.candidate_count,
                &context->known_opponent_rack, config_get_win_pcts(config),
                /*inference_results=*/NULL, thread_control, game,
                /*sim_with_inference=*/false, /*use_heat_map=*/false,
                config_get_num_threads(config), /*print_interval=*/0,
                /*max_num_display_plays=*/result.candidate_count,
                /*max_num_display_plies=*/judge_plies,
                (THINKING_CURVE_BASE_SEED +
                 (uint64_t)(position + 1) * THINKING_CURVE_SEED_STRIDE) ^
                    UINT64_C(0xd1b54a32d192ed03),
                total_iterations, samples_per_candidate,
                /*scond=*/0.0, BAI_THRESHOLD_NONE,
                /*time_limit_seconds=*/0.0, BAI_SAMPLING_RULE_ROUND_ROBIN,
                /*cutoff=*/-1.0, config_get_utility_w_winpct(config),
                config_get_utility_w_spread(config),
                config_get_utility_spread_scale(config),
                /*inference_args=*/NULL, &judge_args);

  ErrorStack *error_stack = error_stack_create();
  Timer timer;
  ctimer_start(&timer);
  simulate(&judge_args, &context->sim_ctx, context->sim_results, error_stack);
  result.elapsed_seconds = ctimer_elapsed_seconds(&timer);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_FINISHED);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("thinking-curve judge failed");
  }
  error_stack_destroy(error_stack);
  result.iterations = sim_results_get_iteration_count(context->sim_results);
  result.nodes = sim_results_get_node_count(context->sim_results);
  if (result.iterations != total_iterations) {
    log_fatal("thinking-curve judge completed %" PRIu64
              " iterations; expected %" PRIu64,
              result.iterations, total_iterations);
  }
  const bool use_blended_utility = config_get_utility_w_spread(config) > 0.0;
  for (int index = 0; index < result.candidate_count; index++) {
    const SimmedPlay *play =
        sim_results_get_simmed_play(context->sim_results, index);
    // MoveList is a heap, so its internal index is not insertion/source rank.
    const int rank = thinking_curve_find_candidate_rank(
        candidates, candidate_count, simmed_play_get_move(play));
    if (!selected_ranks[rank]) {
      log_fatal("thinking-curve judge returned an unselected panel move");
    }
    const Stat *utility_stat = use_blended_utility
                                   ? simmed_play_get_utility_stat(play)
                                   : simmed_play_get_win_pct_stat(play);
    result.utility[rank] = stat_get_mean(utility_stat);
    result.utility_sem[rank] = stat_get_sem(utility_stat);
    result.win[rank] = stat_get_mean(simmed_play_get_win_pct_stat(play));
    result.spread[rank] = stat_get_mean(simmed_play_get_equity_stat(play));
    if (result.best_rank < 0 ||
        result.utility[rank] > result.utility[result.best_rank]) {
      result.best_rank = rank;
    }
  }
  return result;
}

static void thinking_curve_print_point(
    int source_index, int position, int game_index, int bag_tiles, int plies,
    uint64_t target_nodes, bool final, bool control,
    const AnalysisProgressEvent *event,
    const ThinkingCurveCandidate candidates[], int candidate_count,
    int num_plays, int panel_best_index, int candidate_best_index,
    uint64_t min_play_iterations, const ThinkingCurveJudgeResult *judge,
    bai_result_status_t bai_status, double estimated_regret,
    double regret_at_stop, double regret_stop_target) {
  const int selected = event->best_index;
  if (selected < 0 || selected >= num_plays || selected >= candidate_count) {
    log_fatal("thinking-curve checkpoint selected invalid arm %d", selected);
  }
  const double candidate_regret =
      candidates[panel_best_index].oracle_utility -
      candidates[candidate_best_index].oracle_utility;
  const double sampling_regret =
      candidates[candidate_best_index].oracle_utility -
      candidates[selected].oracle_utility;
  const double total_regret = candidate_regret + sampling_regret;
  printf(
      "THINKING_CURVE_POINT source_index=%d position=%d game=%d bag=%d "
      "plies=%d candidates=%d target_nodes=%" PRIu64
      " final=%d control=%d "
      "elapsed_ns=%" PRId64 " cpu_ns=%" PRId64
      " iterations=%" PRIu64 " nodes=%" PRIu64
      " min_play_iterations=%" PRIu64 " selected_rank=%d selected_id=%" PRIu64
      " selected_raw=%s panel_best_rank=%d candidate_best_rank=%d "
      "candidate_regret=%+.9f sampling_regret=%+.9f "
      "provisional_total_regret=%+.9f provisional_win_delta=%+.9f "
      "provisional_spread_delta=%+.9f judge_best_rank=%d "
      "judge_utility=%.12f judge_utility_sem=%.12f judge_win=%.12f "
      "judge_regret=%.12f judge_candidates=%d judge_forced=%d "
      "estimated_best=%.12f estimated_challenger=%.12f "
      "estimated_regret=%.12f regret_at_stop=%.12f "
      "regret_stop_target=%.12f bai_status=%d\n",
      source_index, position, game_index, bag_tiles, plies, candidate_count,
      target_nodes, final, control, event->elapsed_ns, event->cpu_ns,
      event->iterations,
      event->nodes, min_play_iterations, selected,
      move_get_fingerprint(&candidates[selected].move),
      candidates[selected].raw_move, panel_best_index, candidate_best_index,
      candidate_regret, sampling_regret, total_regret,
      candidates[selected].oracle_win - candidates[panel_best_index].oracle_win,
      candidates[selected].oracle_spread -
          candidates[panel_best_index].oracle_spread,
      judge->best_rank, judge->utility[selected], judge->utility_sem[selected],
      judge->win[selected],
      judge->utility[judge->best_rank] - judge->utility[selected],
      judge->candidate_count, judge->forced, event->best_value,
      event->challenger_value, estimated_regret, regret_at_stop,
      regret_stop_target, bai_status);
}

static ThinkingCurveArmResult thinking_curve_run_arm(
    Config *config, ThinkingCurveContext *context,
    const MoveList *candidate_moves, int num_plays,
    const ThinkingCurveCandidate candidates[], int candidate_count, int plies,
    uint64_t target_nodes, uint64_t min_play_iterations, uint64_t seed,
    uint64_t run_id, double regret_stop_target,
    double regret_cross_arm_correlation, double regret_calibration,
    uint64_t regret_check_interval, uint64_t regret_min_samples_per_arm,
    size_t *event_count_out) {
  ThinkingCurveArmResult result = {0};
  const uint64_t max_iterations = target_nodes / ((uint64_t)plies + 1);
  if (max_iterations == 0 ||
      (uint64_t)num_plays * min_play_iterations > max_iterations) {
    log_fatal("thinking-curve arm budget is too small");
  }
  AnalysisTrace *trace = analysis_trace_create(16);
  const AnalysisProgressListener listener = {
      .callback = analysis_trace_record,
      .user_data = trace,
      .run_id = run_id,
      // Structural START/FINISH events are sufficient because every arm is
      // itself one point on the budget-matched curve.
      .checkpoint_interval = 0,
  };

  ThreadControl *thread_control = config_get_thread_control(config);
  assert(
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED));
  rack_set_dist_size_and_reset(&context->known_opponent_rack,
                               ld_get_size(config_get_ld(config)));
  SimArgs sim_args = {0};
  sim_args_fill(plies, candidate_moves, num_plays,
                &context->known_opponent_rack, config_get_win_pcts(config),
                /*inference_results=*/NULL, thread_control,
                config_get_game(config), /*sim_with_inference=*/false,
                /*use_heat_map=*/false, config_get_num_threads(config),
                /*print_interval=*/0,
                /*max_num_display_plays=*/num_plays,
                /*max_num_display_plies=*/plies, seed, max_iterations,
                min_play_iterations,
                /*scond=*/0.0, BAI_THRESHOLD_NONE,
                /*time_limit_seconds=*/0.0, BAI_SAMPLING_RULE_TOP_TWO_IDS,
                /*cutoff=*/-1.0, config_get_utility_w_winpct(config),
                config_get_utility_w_spread(config),
                config_get_utility_spread_scale(config),
                /*inference_args=*/NULL, &sim_args);
  sim_args.bai_options.regret_stop_target = regret_stop_target;
  sim_args.bai_options.regret_cross_arm_correlation =
      regret_cross_arm_correlation;
  sim_args.bai_options.regret_calibration = regret_calibration;
  sim_args.bai_options.regret_check_interval = regret_check_interval;
  sim_args.bai_options.regret_min_samples_per_arm = regret_min_samples_per_arm;
  sim_args.progress_listener = listener;

  ErrorStack *error_stack = error_stack_create();
  simulate(&sim_args, &context->sim_ctx, context->sim_results, error_stack);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_FINISHED);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("thinking-curve simulation arm failed");
  }
  error_stack_destroy(error_stack);

  const size_t event_count = analysis_trace_get_count(trace);
  AnalysisProgressEvent events[16];
  if (event_count > sizeof(events) / sizeof(events[0]) ||
      analysis_trace_get_dropped(trace) != 0) {
    log_fatal("thinking-curve arm trace overflowed");
  }
  for (size_t index = 0; index < event_count; index++) {
    if (!analysis_trace_get_event(trace, index, &events[index])) {
      log_fatal("thinking-curve arm trace changed after solve");
    }
  }
  const AnalysisProgressEvent *finish =
      thinking_curve_find_finish(events, event_count);
  BAIResult *bai_result = sim_results_get_bai_result(context->sim_results);
  result.bai_status = bai_result_get_status(bai_result);
  result.estimated_regret = bai_result_get_estimated_regret(bai_result);
  result.regret_at_stop = bai_result_get_regret_at_stop(bai_result);
  // TOP_TWO_IDS deliberately collapses plays with the same similarity key.
  // If every candidate is in that one equivalence class, BAI reports
  // THRESHOLD immediately after the uniform-sampling floor even when the
  // statistical threshold is disabled.  That is a valid completed arm, not a
  // broken fixed-budget run: no further samples can distinguish candidates
  // under the controller's equivalence model.  Keep the stricter exact sample
  // accounting for ordinary SAMPLE_LIMIT arms.
  const bool stopped_before_sample_limit =
      thinking_curve_bai_can_finish_before_sample_limit(result.bai_status);
  if (finish == NULL || finish->nodes > target_nodes ||
      (!stopped_before_sample_limit &&
       finish->iterations != max_iterations) ||
      (stopped_before_sample_limit && finish->iterations > max_iterations)) {
    log_fatal("thinking-curve arm has invalid final accounting: "
              "finish=%d status=%d iterations=%" PRIu64 "/%" PRIu64
              " nodes=%" PRIu64 "/%" PRIu64
              " estimated_regret=%.12f target=%.12f",
              finish != NULL, result.bai_status,
              finish == NULL ? 0 : finish->iterations, max_iterations,
              finish == NULL ? 0 : finish->nodes, target_nodes,
              result.estimated_regret, regret_stop_target);
  }
  result.decision = *finish;
  const bool use_blended_utility = config_get_utility_w_spread(config) > 0.0;
  for (int arm = 0; arm < num_plays; arm++) {
    const SimmedPlay *play =
        sim_results_get_simmed_play(context->sim_results, arm);
    const int rank = thinking_curve_find_candidate_rank(
        candidates, candidate_count, simmed_play_get_move(play));
    result.rank_by_arm[arm] = rank;
    const Stat *utility_stat = use_blended_utility
                                   ? simmed_play_get_utility_stat(play)
                                   : simmed_play_get_win_pct_stat(play);
    const Stat *win_stat = simmed_play_get_win_pct_stat(play);
    const Stat *spread_stat = simmed_play_get_equity_stat(play);
    result.sample_counts[rank] = stat_get_num_samples(utility_stat);
    result.utility_means[rank] = stat_get_mean(utility_stat);
    result.utility_variances[rank] = stat_get_variance(utility_stat);
    result.win_means[rank] = stat_get_mean(win_stat);
    result.win_variances[rank] = stat_get_variance(win_stat);
    result.spread_means[rank] = stat_get_mean(spread_stat);
    result.spread_variances[rank] = stat_get_variance(spread_stat);
  }
  // BAI's astar index is the sampling controller's current representative.
  // Production ultimately selects with SimResults' comparator, which also
  // applies the normal similarity/tie handling. Record that actual move, as
  // the validated depth sweep does, rather than mistaking astar for the
  // returned play.
  const int best_move_index =
      sim_results_get_best_move_index(context->sim_results);
  if (best_move_index < 0 || best_move_index >= num_plays) {
    log_fatal("thinking-curve arm returned an invalid best move");
  }
  const SimmedPlay *best_play =
      sim_results_get_simmed_play(context->sim_results, best_move_index);
  result.decision.best_index = thinking_curve_find_candidate_rank(
      candidates, candidate_count, simmed_play_get_move(best_play));
  result.decision.item_id =
      move_get_fingerprint(simmed_play_get_move(best_play));
  result.decision.best_value =
      sim_results_get_best_move_utility(context->sim_results);
  *event_count_out += event_count;
  analysis_trace_destroy(trace);
  return result;
}

static void thinking_curve_choose_risk_set(
    const ThinkingCurveArmResult *result, int num_plays, int risk_plays,
    bool risk_set[]) {
  memset(risk_set, 0, THINKING_CURVE_MAX_CANDIDATES * sizeof(*risk_set));
  const int selected = result->decision.best_index;
  if (selected < 0 || selected >= num_plays) {
    log_fatal("thinking-curve regret panel has invalid selected rank");
  }
  risk_set[selected] = true;
  int selected_count = 1;
  while (selected_count < risk_plays && selected_count < num_plays) {
    int best_rank = -1;
    double best_upper_bound = -INFINITY;
    for (int rank = 0; rank < num_plays; rank++) {
      if (risk_set[rank]) {
        continue;
      }
      const uint64_t count = result->sample_counts[rank];
      if (count == 0) {
        log_fatal("thinking-curve regret panel has an unsampled arm");
      }
      const double sem = sqrt(result->utility_variances[rank] / (double)count);
      const double upper_bound =
          result->utility_means[rank] + 2.5758293035489004 * sem;
      if (best_rank < 0 || upper_bound > best_upper_bound) {
        best_rank = rank;
        best_upper_bound = upper_bound;
      }
    }
    if (best_rank < 0) {
      log_fatal("thinking-curve regret panel could not select a risk arm");
    }
    risk_set[best_rank] = true;
    selected_count++;
  }
}

static void
thinking_curve_run_regret_probe(Config *config, ThinkingCurveContext *context,
                                const ThinkingCurveCandidate candidates[],
                                int candidate_count, const bool risk_set[],
                                int num_plays, int plies,
                                uint64_t samples_per_candidate, uint64_t seed,
                                ThinkingCurveArmResult *result) {
  for (int rank = 0; rank < num_plays; rank++) {
    if (risk_set[rank]) {
      result->probe_count++;
    }
  }
  if (result->probe_count < 2) {
    log_fatal("thinking-curve regret probe has too few moves");
  }

  MoveList *probe_moves = move_list_create(result->probe_count);
  const Game *game = config_get_game(config);
  const int player_index = game_get_player_on_turn_index(game);
  move_list_set_rack(probe_moves,
                     player_get_rack(game_get_player(game, player_index)));
  for (int rank = 0; rank < num_plays; rank++) {
    if (risk_set[rank]) {
      move_list_add_move(probe_moves, &candidates[rank].move);
    }
  }

  const uint64_t total_iterations =
      samples_per_candidate * (uint64_t)result->probe_count;
  result->sample_trace = thinking_curve_sample_trace_create(total_iterations);
  ThreadControl *thread_control = config_get_thread_control(config);
  assert(
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED));
  rack_set_dist_size_and_reset(&context->known_opponent_rack,
                               ld_get_size(config_get_ld(config)));
  SimArgs probe_args = {0};
  sim_args_fill(plies, probe_moves, result->probe_count,
                &context->known_opponent_rack, config_get_win_pcts(config),
                /*inference_results=*/NULL, thread_control, game,
                /*sim_with_inference=*/false, /*use_heat_map=*/false,
                config_get_num_threads(config), /*print_interval=*/0,
                /*max_num_display_plays=*/result->probe_count,
                /*max_num_display_plies=*/plies, seed, total_iterations,
                samples_per_candidate, /*scond=*/0.0, BAI_THRESHOLD_NONE,
                /*time_limit_seconds=*/0.0, BAI_SAMPLING_RULE_ROUND_ROBIN,
                /*cutoff=*/-1.0, config_get_utility_w_winpct(config),
                config_get_utility_w_spread(config),
                config_get_utility_spread_scale(config),
                /*inference_args=*/NULL, &probe_args);
  probe_args.sample_listener = (SimSampleListener){
      .callback = thinking_curve_sample_trace_record,
      .user_data = result->sample_trace,
  };

  ErrorStack *error_stack = error_stack_create();
  Timer timer;
  ctimer_start(&timer);
  simulate(&probe_args, &context->sim_ctx, context->sim_results, error_stack);
  result->probe_seconds = ctimer_elapsed_seconds(&timer);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_FINISHED);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("thinking-curve regret probe failed");
  }
  error_stack_destroy(error_stack);
  result->probe_iterations =
      sim_results_get_iteration_count(context->sim_results);
  result->probe_nodes = sim_results_get_node_count(context->sim_results);
  if (result->probe_iterations != total_iterations ||
      atomic_load(&result->sample_trace->count) != total_iterations ||
      atomic_load(&result->sample_trace->dropped) != 0) {
    log_fatal("thinking-curve regret probe has invalid accounting");
  }
  for (int arm = 0; arm < result->probe_count; arm++) {
    const SimmedPlay *play =
        sim_results_get_simmed_play(context->sim_results, arm);
    const int rank = thinking_curve_find_candidate_rank(
        candidates, candidate_count, simmed_play_get_move(play));
    if (!risk_set[rank]) {
      log_fatal("thinking-curve regret probe returned an unselected move");
    }
    result->probe_rank_by_arm[arm] = rank;
  }
  move_list_destroy(probe_moves);
}

static int compare_sim_sample_events(const void *a, const void *b) {
  const SimSampleEvent *ea = a;
  const SimSampleEvent *eb = b;
  if (ea->play_index != eb->play_index) {
    return (ea->play_index > eb->play_index) -
           (ea->play_index < eb->play_index);
  }
  return (ea->scenario_seed > eb->scenario_seed) -
         (ea->scenario_seed < eb->scenario_seed);
}

static const SimSampleEvent *
thinking_curve_find_sample_seed(const SimSampleEvent events[], uint64_t start,
                                uint64_t count, uint64_t seed) {
  uint64_t low = start;
  uint64_t high = start + count;
  while (low < high) {
    const uint64_t middle = low + (high - low) / 2;
    if (events[middle].scenario_seed < seed) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low < start + count && events[low].scenario_seed == seed) {
    return &events[low];
  }
  return NULL;
}

static void thinking_curve_print_regret_panel(
    int source_index, int position, int game_index, int bag_tiles, int plies,
    uint64_t target_nodes, const ThinkingCurveArmResult *result,
    const bool risk_set[], int num_plays, int paired_sample_limit,
    const ThinkingCurveJudgeResult *judge) {
  ThinkingCurveSampleTrace *trace = result->sample_trace;
  if (trace == NULL) {
    return;
  }
  const uint64_t trace_count = atomic_load(&trace->count);
  qsort(trace->events, (size_t)trace_count, sizeof(*trace->events),
        compare_sim_sample_events);

  uint64_t arm_starts[THINKING_CURVE_MAX_CANDIDATES];
  uint64_t arm_counts[THINKING_CURVE_MAX_CANDIDATES] = {0};
  for (int arm = 0; arm < result->probe_count; arm++) {
    arm_starts[arm] = UINT64_MAX;
  }
  for (uint64_t index = 0; index < trace_count; index++) {
    const int arm = trace->events[index].play_index;
    if (arm < 0 || arm >= result->probe_count) {
      log_fatal("thinking-curve sample trace has an invalid arm");
    }
    if (arm_starts[arm] == UINT64_MAX) {
      arm_starts[arm] = index;
    }
    arm_counts[arm]++;
  }

  int arm_by_rank[THINKING_CURVE_MAX_CANDIDATES];
  for (int rank = 0; rank < num_plays; rank++) {
    arm_by_rank[rank] = -1;
  }
  for (int arm = 0; arm < result->probe_count; arm++) {
    const int rank = result->probe_rank_by_arm[arm];
    if (rank < 0 || rank >= num_plays || arm_by_rank[rank] >= 0) {
      log_fatal("thinking-curve sample trace has an invalid arm mapping");
    }
    arm_by_rank[rank] = arm;
    if (arm_counts[arm] != (uint64_t)paired_sample_limit) {
      log_fatal("thinking-curve sample trace arm count mismatch");
    }
  }

  int risk_count = 0;
  int base_arm = -1;
  uint64_t common_count = UINT64_MAX;
  int risk_judge_best = -1;
  for (int rank = 0; rank < num_plays; rank++) {
    if (!risk_set[rank]) {
      continue;
    }
    risk_count++;
    const int arm = arm_by_rank[rank];
    if (arm_counts[arm] < common_count) {
      common_count = arm_counts[arm];
      base_arm = arm;
    }
    if (risk_judge_best < 0 ||
        judge->utility[rank] > judge->utility[risk_judge_best]) {
      risk_judge_best = rank;
    }
  }
  if (risk_count < 2 || base_arm < 0 || common_count == 0 ||
      risk_judge_best < 0) {
    log_fatal("thinking-curve regret panel is incomplete");
  }
  const uint64_t paired_rows = common_count < (uint64_t)paired_sample_limit
                                   ? common_count
                                   : (uint64_t)paired_sample_limit;
  const int selected = result->decision.best_index;
  printf("THINKING_CURVE_REGRET_PANEL source_index=%d position=%d game=%d "
         "bag=%d plies=%d target_nodes=%" PRIu64
         " risk_count=%d paired_rows=%" PRIu64 " probe_iterations=%" PRIu64
         " probe_nodes=%" PRIu64 " probe_seconds=%.6f"
         " probe_mode=independent_fixed_allocation"
         " selected_rank=%d judge_best_rank=%d "
         "actual_utility_regret=%.12f actual_win_regret=%+.12f "
         "actual_spread_regret=%+.12f\n",
         source_index, position, game_index, bag_tiles, plies, target_nodes,
         risk_count, paired_rows, result->probe_iterations, result->probe_nodes,
         result->probe_seconds, selected, risk_judge_best,
         judge->utility[risk_judge_best] - judge->utility[selected],
         judge->win[risk_judge_best] - judge->win[selected],
         judge->spread[risk_judge_best] - judge->spread[selected]);

  for (int rank = 0; rank < num_plays; rank++) {
    if (!risk_set[rank]) {
      continue;
    }
    printf("THINKING_CURVE_REGRET_ARM source_index=%d plies=%d "
           "target_nodes=%" PRIu64 " rank=%d samples=%" PRIu64
           " utility_mean=%.17g utility_variance=%.17g "
           "win_mean=%.17g win_variance=%.17g "
           "spread_mean=%.17g spread_variance=%.17g "
           "judge_utility=%.17g judge_win=%.17g judge_spread=%.17g\n",
           source_index, plies, target_nodes, rank, result->sample_counts[rank],
           result->utility_means[rank], result->utility_variances[rank],
           result->win_means[rank], result->win_variances[rank],
           result->spread_means[rank], result->spread_variances[rank],
           judge->utility[rank], judge->win[rank], judge->spread[rank]);
  }

  const uint64_t base_start = arm_starts[base_arm];
  for (uint64_t row = 0; row < paired_rows; row++) {
    const uint64_t seed = trace->events[base_start + row].scenario_seed;
    for (int rank = 0; rank < num_plays; rank++) {
      if (!risk_set[rank]) {
        continue;
      }
      const int arm = arm_by_rank[rank];
      const SimSampleEvent *event = thinking_curve_find_sample_seed(
          trace->events, arm_starts[arm], arm_counts[arm], seed);
      if (event == NULL) {
        log_fatal("thinking-curve risk arms do not share scenario seeds");
      }
      printf("THINKING_CURVE_REGRET_SAMPLE source_index=%d plies=%d "
             "target_nodes=%" PRIu64 " pair_row=%" PRIu64
             " scenario_seed=%" PRIu64 " rank=%d utility=%.17g "
             "win=%.17g spread=%.17g\n",
             source_index, plies, target_nodes, row, seed, rank, event->utility,
             event->win_pct, event->spread);
    }
  }
}

static void thinking_curve_run_position(
    Config *config, ThinkingCurveContext *context, MoveList *candidate_moves,
    const ThinkingCurveCandidate candidates[], int candidate_count,
    const char *cgp, int source_index, int position, int game_index,
    int bag_tiles, int num_plays, int plies, uint64_t max_nodes,
    int uniform_floor_per_mille, const uint64_t targets[], int target_count,
    int judge_plies, uint64_t judge_samples, int judge_risk_plays,
    bool capture_regret_samples, int regret_risk_plays,
    int regret_paired_samples, double regret_stop_target,
    double regret_cross_arm_correlation, double regret_calibration,
    uint64_t regret_check_interval, uint64_t regret_min_samples_per_arm,
    bool fixed_control) {
  if (candidate_count < num_plays) {
    log_fatal("thinking-curve position has %d candidates; requested %d",
              candidate_count, num_plays);
  }
  thinking_curve_load_position(config, cgp);
  const Game *game = config_get_game(config);
  const int player_index = game_get_player_on_turn_index(game);
  move_list_reset(candidate_moves);
  move_list_set_rack(candidate_moves,
                     player_get_rack(game_get_player(game, player_index)));
  for (int rank = 0; rank < num_plays; rank++) {
    move_list_add_move(candidate_moves, &candidates[rank].move);
  }

  int panel_best_index = 0;
  for (int rank = 1; rank < candidate_count; rank++) {
    if (candidates[rank].oracle_utility >
        candidates[panel_best_index].oracle_utility) {
      panel_best_index = rank;
    }
  }
  int candidate_best_index = 0;
  for (int rank = 1; rank < num_plays; rank++) {
    if (candidates[rank].oracle_utility >
        candidates[candidate_best_index].oracle_utility) {
      candidate_best_index = rank;
    }
  }

  const uint64_t seed =
      (THINKING_CURVE_BASE_SEED +
       (uint64_t)(position + 1) * THINKING_CURVE_SEED_STRIDE) ^
      THINKING_CURVE_NOMINATION_SEED_XOR;
  ThinkingCurveArmResult decisions[THINKING_CURVE_MAX_TARGETS];
  bool judge_risk_sets[THINKING_CURVE_MAX_TARGETS]
                      [THINKING_CURVE_MAX_CANDIDATES] = {{false}};
  bool regret_risk_sets[THINKING_CURVE_MAX_TARGETS]
                       [THINKING_CURVE_MAX_CANDIDATES] = {{false}};
  uint64_t minimums[THINKING_CURVE_MAX_TARGETS];
  int decision_count = 0;
  size_t event_count = 0;
  bool selected_ranks[THINKING_CURVE_MAX_CANDIDATES] = {false};
  for (int target_index = 0; target_index < target_count; target_index++) {
    if (targets[target_index] > max_nodes) {
      continue;
    }
    const uint64_t total_iterations =
        targets[target_index] / ((uint64_t)plies + 1);
    const uint64_t denominator = (uint64_t)num_plays * 1000;
    if (total_iterations > UINT64_MAX / (uint64_t)uniform_floor_per_mille) {
      log_fatal("thinking-curve minimum-iteration calculation overflow");
    }
    uint64_t minimum = (total_iterations * (uint64_t)uniform_floor_per_mille +
                        denominator / 2) /
                       denominator;
    if (minimum == 0) {
      minimum = 1;
    }
    minimums[decision_count] = minimum;
    decisions[decision_count] = thinking_curve_run_arm(
        config, context, candidate_moves, num_plays, candidates,
        candidate_count, plies, targets[target_index], minimum, seed,
        ((uint64_t)(source_index + 1) << 16) | ((uint64_t)plies << 8) |
            (uint64_t)(decision_count + 1),
        regret_stop_target, regret_cross_arm_correlation, regret_calibration,
        regret_check_interval, regret_min_samples_per_arm, &event_count);
    selected_ranks[decisions[decision_count].decision.best_index] = true;
    if (judge_risk_plays >= 2) {
      thinking_curve_choose_risk_set(
          &decisions[decision_count], num_plays, judge_risk_plays,
          judge_risk_sets[decision_count]);
      for (int rank = 0; rank < num_plays; rank++) {
        selected_ranks[rank] =
            selected_ranks[rank] || judge_risk_sets[decision_count][rank];
      }
    }
    // Without a fixed-budget control, a stopped arm still needs plausible
    // alternatives in the online judge. With a control, only the two returned
    // moves matter: agreeing decisions require no oracle work and a
    // disagreement can be adjudicated directly.
    if (capture_regret_samples ||
        (regret_stop_target > 0.0 && !fixed_control)) {
      thinking_curve_choose_risk_set(&decisions[decision_count], num_plays,
                                     regret_risk_plays,
                                     regret_risk_sets[decision_count]);
      for (int rank = 0; rank < num_plays; rank++) {
        selected_ranks[rank] =
            selected_ranks[rank] || regret_risk_sets[decision_count][rank];
      }
    }
    if (capture_regret_samples) {
      thinking_curve_run_regret_probe(
          config, context, candidates, candidate_count,
          regret_risk_sets[decision_count], num_plays, plies,
          (uint64_t)regret_paired_samples,
          seed ^ targets[target_index] ^ UINT64_C(0xe7037ed1a0b428db),
          &decisions[decision_count]);
    }
    decision_count++;
  }
  if (decision_count == 0) {
    log_fatal("thinking-curve position has no in-range target");
  }
  ThinkingCurveArmResult control_result = {0};
  bool control_judge_risk_set[THINKING_CURVE_MAX_CANDIDATES] = {false};
  bool control_risk_set[THINKING_CURVE_MAX_CANDIDATES] = {false};
  uint64_t control_target_nodes = 0;
  const bool has_control = fixed_control && regret_stop_target > 0.0;
  if (has_control) {
    for (int target_index = 0; target_index < target_count; target_index++) {
      if (targets[target_index] <= max_nodes) {
        control_target_nodes = targets[target_index];
      }
    }
    control_result = thinking_curve_run_arm(
        config, context, candidate_moves, num_plays, candidates,
        candidate_count, plies, control_target_nodes,
        minimums[decision_count - 1], seed,
        ((uint64_t)(source_index + 1) << 32) | ((uint64_t)plies << 24) |
            UINT64_C(0x434f4e),
        /*regret_stop_target=*/0.0, regret_cross_arm_correlation,
        regret_calibration, regret_check_interval, regret_min_samples_per_arm,
        &event_count);
    selected_ranks[control_result.decision.best_index] = true;
    if (judge_risk_plays >= 2) {
      thinking_curve_choose_risk_set(
          &control_result, num_plays, judge_risk_plays,
          control_judge_risk_set);
      for (int rank = 0; rank < num_plays; rank++) {
        selected_ranks[rank] =
            selected_ranks[rank] || control_judge_risk_set[rank];
      }
    }
    if (capture_regret_samples) {
      thinking_curve_choose_risk_set(
          &control_result, num_plays, regret_risk_plays, control_risk_set);
      for (int rank = 0; rank < num_plays; rank++) {
        selected_ranks[rank] = selected_ranks[rank] || control_risk_set[rank];
      }
    }
  }
  const ThinkingCurveJudgeResult judge = thinking_curve_run_judge(
      config, context, candidate_moves, candidates, selected_ranks,
      candidate_count, judge_plies, judge_samples, position);

  printf(
      "THINKING_CURVE_POSITION source_index=%d position=%d game=%d bag=%d "
      "plies=%d candidates=%d max_nodes=%" PRIu64 " uniform_floor_per_mille=%d"
      " judge_plies=%d judge_samples=%" PRIu64
      " judge_risk_plays=%d panel_best_rank=%d cgp=%s\n",
      source_index, position, game_index, bag_tiles, plies, num_plays,
      max_nodes, uniform_floor_per_mille, judge_plies, judge_samples,
      judge_risk_plays, panel_best_index, cgp);
  int decision_index = 0;
  for (int target_index = 0; target_index < target_count; target_index++) {
    if (targets[target_index] <= max_nodes) {
      thinking_curve_print_point(
          source_index, position, game_index, bag_tiles, plies,
          targets[target_index], /*final=*/false, /*control=*/false,
          &decisions[decision_index].decision, candidates, candidate_count,
          num_plays, panel_best_index, candidate_best_index,
          minimums[decision_index], &judge,
          decisions[decision_index].bai_status,
          decisions[decision_index].estimated_regret,
          decisions[decision_index].regret_at_stop, regret_stop_target);
      if (capture_regret_samples) {
        thinking_curve_print_regret_panel(
            source_index, position, game_index, bag_tiles, plies,
            targets[target_index], &decisions[decision_index],
            regret_risk_sets[decision_index], num_plays, regret_paired_samples,
            &judge);
      }
      decision_index++;
    }
  }
  if (has_control) {
    thinking_curve_print_point(
        source_index, position, game_index, bag_tiles, plies,
        control_target_nodes, /*final=*/false, /*control=*/true,
        &control_result.decision, candidates, candidate_count, num_plays,
        panel_best_index, candidate_best_index, minimums[decision_count - 1],
        &judge, control_result.bai_status, control_result.estimated_regret,
        control_result.regret_at_stop, /*regret_stop_target=*/0.0);
  }
  thinking_curve_print_point(
      source_index, position, game_index, bag_tiles, plies,
      /*target_nodes=*/0, /*final=*/true, /*control=*/false,
      &decisions[decision_count - 1].decision, candidates, candidate_count,
      num_plays, panel_best_index, candidate_best_index,
      minimums[decision_count - 1], &judge,
      decisions[decision_count - 1].bai_status,
      decisions[decision_count - 1].estimated_regret,
      decisions[decision_count - 1].regret_at_stop, regret_stop_target);
  printf("THINKING_CURVE_POSITION_DONE source_index=%d position=%d plies=%d "
         "events=%zu dropped=0 final_iterations=%" PRIu64
         " final_nodes=%" PRIu64 " judge_candidates=%d "
         "judge_iterations=%" PRIu64 " judge_nodes=%" PRIu64
         " judge_seconds=%.6f judge_forced=%d control=%d "
         "control_iterations=%" PRIu64 " control_nodes=%" PRIu64 "\n",
         source_index, position, plies, event_count,
         decisions[decision_count - 1].decision.iterations,
         decisions[decision_count - 1].decision.nodes, judge.candidate_count,
         judge.iterations, judge.nodes, judge.elapsed_seconds, judge.forced,
         has_control, control_result.decision.iterations,
         control_result.decision.nodes);
  for (int decision_index_to_destroy = 0;
       decision_index_to_destroy < decision_count;
       decision_index_to_destroy++) {
    thinking_curve_sample_trace_destroy(
        decisions[decision_index_to_destroy].sample_trace);
  }
  thinking_curve_sample_trace_destroy(control_result.sample_trace);
  fflush_or_die(stdout);
}

// Full-game value-to-go calibration starts from raw positions rather than a
// pre-oracled candidate corpus. Generate candidates with the same settings as
// production PlayChooser so candidate topology cannot silently differ from
// the live policy. The online common-seed judge remains the only strength
// label; the legacy oracle fields are diagnostics and therefore zero here.
static int thinking_curve_generate_candidates(
    Config *config, const char *cgp, MoveList *generated_moves, int num_plays,
    ThinkingCurveCandidate candidates[]) {
  thinking_curve_load_position(config, cgp);
  move_list_reset(generated_moves);
  const MoveGenArgs gen_args = {
      .game = config_get_game(config),
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .move_list = generated_moves,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&gen_args);
  int count = move_list_get_count(generated_moves);
  if (count > num_plays) {
    count = num_plays;
  }
  for (int rank = 0; rank < count; rank++) {
    ThinkingCurveCandidate *candidate = &candidates[rank];
    move_copy(&candidate->move, move_list_get_move(generated_moves, rank));
    (void)snprintf(candidate->raw_move, sizeof(candidate->raw_move),
                   "generated-rank-%d", rank);
    candidate->oracle_utility = 0.0;
    candidate->oracle_win = 0.0;
    candidate->oracle_spread = 0.0;
  }
  return count;
}

void test_thinking_curve(void) {
  // Regression guard for all-similar candidate panels: TOP_TWO_IDS reports
  // THRESHOLD after the uniform floor even when GK16 thresholding is disabled.
  assert(thinking_curve_bai_can_finish_before_sample_limit(
      BAI_RESULT_STATUS_THRESHOLD));
  assert(thinking_curve_bai_can_finish_before_sample_limit(
      BAI_RESULT_STATUS_REGRET_LIMIT));
  assert(!thinking_curve_bai_can_finish_before_sample_limit(
      BAI_RESULT_STATUS_SAMPLE_LIMIT));
  log_set_level(LOG_FATAL);
  const char *corpus = thinking_curve_env_string(
      "THINKING_CURVE_CORPUS",
      "obj/thinking-curves-candidates-bag50-644x60.log");
  const int skip_positions =
      thinking_curve_env_nonnegative_int("THINKING_CURVE_SKIP_POSITIONS", 0);
  const int max_positions =
      thinking_curve_env_positive_int("THINKING_CURVE_MAX_POSITIONS", 8);
  const int minimum_bag =
      thinking_curve_env_nonnegative_int("THINKING_CURVE_MIN_BAG", 0);
  const int maximum_bag =
      thinking_curve_env_nonnegative_int("THINKING_CURVE_MAX_BAG", 100);
  const int num_plays =
      thinking_curve_env_positive_int("THINKING_CURVE_NUM_PLAYS", 60);
  const int plies = thinking_curve_env_positive_int("THINKING_CURVE_PLIES", 4);
  const int num_threads = thinking_curve_env_positive_int(
      "THINKING_CURVE_THREADS", get_num_cores());
  const uint64_t max_nodes = thinking_curve_env_positive_uint64(
      "THINKING_CURVE_MAX_NODES", UINT64_C(10000000));
  const int uniform_floor_per_mille = thinking_curve_env_positive_int(
      "THINKING_CURVE_UNIFORM_FLOOR_PER_MILLE", 100);
  const int judge_plies =
      thinking_curve_env_positive_int("THINKING_CURVE_JUDGE_PLIES", 10);
  const uint64_t judge_samples = thinking_curve_env_positive_uint64(
      "THINKING_CURVE_JUDGE_SAMPLES", UINT64_C(100000));
  const int judge_risk_plays = thinking_curve_env_nonnegative_int(
      "THINKING_CURVE_JUDGE_RISK_PLAYS", 0);
  const bool capture_regret_samples =
      thinking_curve_env_nonnegative_int("THINKING_CURVE_REGRET_TRACE", 0) != 0;
  const int regret_risk_plays =
      thinking_curve_env_positive_int("THINKING_CURVE_REGRET_RISK_PLAYS", 8);
  const int regret_paired_samples = thinking_curve_env_positive_int(
      "THINKING_CURVE_REGRET_PAIRED_SAMPLES", 512);
  const double regret_stop_target = thinking_curve_env_nonnegative_double(
      "THINKING_CURVE_REGRET_STOP_TARGET", 0.0);
  const double regret_cross_arm_correlation =
      thinking_curve_env_nonnegative_double("THINKING_CURVE_REGRET_CORRELATION",
                                            0.48);
  const double regret_calibration = thinking_curve_env_nonnegative_double(
      "THINKING_CURVE_REGRET_CALIBRATION", 1.0);
  const uint64_t regret_check_interval = thinking_curve_env_positive_uint64(
      "THINKING_CURVE_REGRET_CHECK_INTERVAL", 256);
  const uint64_t regret_min_samples_per_arm =
      thinking_curve_env_positive_uint64(
          "THINKING_CURVE_REGRET_MIN_SAMPLES_PER_ARM", 32);
  const bool fixed_control = thinking_curve_env_nonnegative_int(
                                 "THINKING_CURVE_FIXED_CONTROL", 0) != 0;
  const double wall_seconds =
      thinking_curve_env_nonnegative_double("THINKING_CURVE_WALL_SECONDS", 0.0);
  const char *target_string = thinking_curve_env_string(
      "THINKING_CURVE_TARGET_NODES",
      "50000,100000,200000,300000,500000,1000000,3000000,10000000");
  uint64_t targets[THINKING_CURVE_MAX_TARGETS];
  const int target_count = thinking_curve_parse_targets(target_string, targets);
  if (num_plays < 2 || num_plays > THINKING_CURVE_MAX_CANDIDATES) {
    log_fatal("THINKING_CURVE_NUM_PLAYS must be between 2 and %d",
              THINKING_CURVE_MAX_CANDIDATES);
  }
  if (maximum_bag < minimum_bag) {
    log_fatal("THINKING_CURVE_MAX_BAG must be at least "
              "THINKING_CURVE_MIN_BAG");
  }
  if (plies < 1 || plies > MAX_PLIES) {
    log_fatal("THINKING_CURVE_PLIES must be between 1 and %d", MAX_PLIES);
  }
  if (uniform_floor_per_mille > 1000) {
    log_fatal("THINKING_CURVE_UNIFORM_FLOOR_PER_MILLE must be at most 1000");
  }
  if (judge_plies <= plies || judge_plies > MAX_PLIES) {
    log_fatal("THINKING_CURVE_JUDGE_PLIES must exceed the experiment plies "
              "and be at most %d",
              MAX_PLIES);
  }
  if (judge_risk_plays == 1 || judge_risk_plays > num_plays) {
    log_fatal("THINKING_CURVE_JUDGE_RISK_PLAYS must be zero or between 2 "
              "and THINKING_CURVE_NUM_PLAYS");
  }
  if (regret_risk_plays < 2 || regret_risk_plays > num_plays) {
    log_fatal("THINKING_CURVE_REGRET_RISK_PLAYS must be between 2 and "
              "THINKING_CURVE_NUM_PLAYS");
  }
  if (regret_cross_arm_correlation >= 1.0) {
    log_fatal("THINKING_CURVE_REGRET_CORRELATION must be less than 1");
  }
  if (regret_calibration <= 0.0) {
    log_fatal("THINKING_CURVE_REGRET_CALIBRATION must be positive");
  }

  FILE *stream = fopen(corpus, "re");
  if (stream == NULL) {
    log_fatal("could not open thinking-curve corpus: %s", corpus);
  }
  Config *config = thinking_curve_create_config(num_threads, num_plays);
  ThinkingCurveContext context;
  thinking_curve_context_init(&context, config);
  MoveList *candidate_moves = move_list_create(num_plays);
  MoveList *generated_moves = move_list_create(num_plays);
  Timer timer;
  ctimer_start(&timer);
  printf("THINKING_CURVE_CONFIG corpus=%s skip_positions=%d max_positions=%d "
         "minimum_bag=%d maximum_bag=%d "
         "num_plays=%d plies=%d threads=%d max_nodes=%" PRIu64
         " uniform_floor_per_mille=%d"
         " judge_plies=%d judge_samples=%" PRIu64
         " judge_risk_plays=%d"
         " regret_trace=%d regret_risk_plays=%d regret_paired_samples=%d"
         " regret_stop_target=%.12f regret_correlation=%.6f"
         " regret_calibration=%.6f regret_check_interval=%" PRIu64
         " regret_min_samples_per_arm=%" PRIu64 " fixed_control=%d"
         " wall_seconds=%.3f targets=%s oracle=online_common_seed "
         "sampling_rule=budget_matched_top_two_ids\n",
         corpus, skip_positions, max_positions, minimum_bag, maximum_bag,
         num_plays, plies, num_threads,
         max_nodes, uniform_floor_per_mille, judge_plies, judge_samples,
         judge_risk_plays, capture_regret_samples, regret_risk_plays,
         regret_paired_samples,
         regret_stop_target, regret_cross_arm_correlation, regret_calibration,
         regret_check_interval, regret_min_samples_per_arm, fixed_control,
         wall_seconds, target_string);
  fflush_or_die(stdout);

  ThinkingCurveCandidate candidates[THINKING_CURVE_MAX_CANDIDATES];
  int candidate_count = 0;
  int expected_candidates = -1;
  int current_position = -1;
  int current_game = -1;
  int current_bag = -1;
  char *current_cgp = NULL;
  int groups_seen = 0;
  int evaluated = 0;
  bool wall_limit_reached = false;
  char *line = NULL;
  size_t line_capacity = 0;
  while (getline_ignore_carriage_return(&line, &line_capacity, stream) != -1) {
    if (strncmp(line, "TIME_VALUE_POSITION ", 20) == 0) {
      if (current_position >= 0) {
        log_fatal("thinking-curve corpus mixes generated and candidate rows");
      }
      char *position_value = thinking_curve_copy_field(line, "position=");
      char *game_value = thinking_curve_copy_field(line, "game=");
      char *bag_value = thinking_curve_copy_field(line, "bag=");
      const char *cgp_start = strstr(line, " cgp=");
      if (position_value == NULL || game_value == NULL || bag_value == NULL ||
          cgp_start == NULL) {
        log_fatal("malformed TIME_VALUE_POSITION row");
      }
      cgp_start += strlen(" cgp=");
      char *cgp = string_duplicate(cgp_start);
      cgp[strcspn(cgp, "\r\n")] = '\0';
      const int position = (int)strtol(position_value, NULL, 10);
      const int game_index = (int)strtol(game_value, NULL, 10);
      const int bag_tiles = (int)strtol(bag_value, NULL, 10);

      const bool in_bag_range =
          bag_tiles >= minimum_bag && bag_tiles <= maximum_bag;
      if (in_bag_range && groups_seen >= skip_positions &&
          evaluated < max_positions) {
        const int generated_count = thinking_curve_generate_candidates(
            config, cgp, generated_moves, num_plays, candidates);
        if (generated_count >= 2) {
          const int position_risk_plays =
              regret_risk_plays < generated_count ? regret_risk_plays
                                                  : generated_count;
          const int position_judge_risk_plays =
              judge_risk_plays > 0 && judge_risk_plays < generated_count
                  ? judge_risk_plays
                  : judge_risk_plays > 0 ? generated_count : 0;
          thinking_curve_run_position(
              config, &context, candidate_moves, candidates, generated_count,
              cgp, groups_seen, position, game_index, bag_tiles,
              generated_count, plies, max_nodes, uniform_floor_per_mille,
              targets, target_count, judge_plies, judge_samples,
              position_judge_risk_plays, capture_regret_samples,
              position_risk_plays,
              regret_paired_samples, regret_stop_target,
              regret_cross_arm_correlation, regret_calibration,
              regret_check_interval, regret_min_samples_per_arm,
              fixed_control);
        } else {
          printf("THINKING_CURVE_FORCED_POSITION source_index=%d position=%d "
                 "game=%d bag=%d plies=%d candidates=%d regret=0 cgp=%s\n",
                 groups_seen, position, game_index, bag_tiles, plies,
                 generated_count, cgp);
          fflush_or_die(stdout);
        }
        evaluated++;
      }
      groups_seen++;
      free(cgp);
      free(position_value);
      free(game_value);
      free(bag_value);
      if (evaluated >= max_positions ||
          (wall_seconds > 0.0 &&
           ctimer_elapsed_seconds(&timer) >= wall_seconds)) {
        wall_limit_reached = wall_seconds > 0.0 &&
                             ctimer_elapsed_seconds(&timer) >= wall_seconds;
        break;
      }
      continue;
    }
    if (strncmp(line, "POSITIONAL_CANDIDATE ", 21) != 0) {
      continue;
    }
    char *position_value = thinking_curve_copy_field(line, "position=");
    char *game_value = thinking_curve_copy_field(line, "game=");
    char *bag_value = thinking_curve_copy_field(line, "bag=");
    char *rank_value = thinking_curve_copy_field(line, "rank=");
    char *candidate_count_value =
        thinking_curve_copy_field(line, "candidates=");
    char *oracle_utility_value =
        thinking_curve_copy_field(line, "oracle_utility=");
    char *oracle_win_value = thinking_curve_copy_field(line, "oracle_win=");
    char *oracle_spread_value =
        thinking_curve_copy_field(line, "oracle_spread=");
    char *raw_move = thinking_curve_copy_field(line, "move_raw=");
    const char *cgp_start = strstr(line, " cgp=");
    if (position_value == NULL || game_value == NULL || bag_value == NULL ||
        rank_value == NULL || candidate_count_value == NULL ||
        oracle_utility_value == NULL || oracle_win_value == NULL ||
        oracle_spread_value == NULL || raw_move == NULL || cgp_start == NULL) {
      log_fatal("malformed thinking-curve corpus row");
    }
    const int position = (int)strtol(position_value, NULL, 10);
    const int rank = (int)strtol(rank_value, NULL, 10);

    if (current_position >= 0 && position != current_position) {
      if (candidate_count != expected_candidates) {
        log_fatal("incomplete thinking-curve candidate group");
      }
      if (groups_seen >= skip_positions && evaluated < max_positions) {
        thinking_curve_run_position(
            config, &context, candidate_moves, candidates, candidate_count,
            current_cgp, groups_seen, current_position, current_game,
            current_bag, num_plays, plies, max_nodes, uniform_floor_per_mille,
            targets, target_count, judge_plies, judge_samples,
            judge_risk_plays, capture_regret_samples, regret_risk_plays,
            regret_paired_samples, regret_stop_target,
            regret_cross_arm_correlation,
            regret_calibration, regret_check_interval,
            regret_min_samples_per_arm, fixed_control);
        evaluated++;
      }
      groups_seen++;
      free(current_cgp);
      current_cgp = NULL;
      candidate_count = 0;
      if (evaluated >= max_positions ||
          (wall_seconds > 0.0 &&
           ctimer_elapsed_seconds(&timer) >= wall_seconds)) {
        wall_limit_reached = wall_seconds > 0.0 &&
                             ctimer_elapsed_seconds(&timer) >= wall_seconds;
        free(position_value);
        free(game_value);
        free(bag_value);
        free(rank_value);
        free(candidate_count_value);
        free(oracle_utility_value);
        free(oracle_win_value);
        free(oracle_spread_value);
        free(raw_move);
        break;
      }
    }

    if (current_cgp == NULL) {
      current_position = position;
      current_game = (int)strtol(game_value, NULL, 10);
      current_bag = (int)strtol(bag_value, NULL, 10);
      expected_candidates = (int)strtol(candidate_count_value, NULL, 10);
      cgp_start += strlen(" cgp=");
      current_cgp = string_duplicate(cgp_start);
      current_cgp[strcspn(current_cgp, "\r\n")] = '\0';
    }
    if (position != current_position || rank != candidate_count ||
        expected_candidates > THINKING_CURVE_MAX_CANDIDATES) {
      log_fatal("noncanonical thinking-curve candidate group");
    }
    ThinkingCurveCandidate *candidate = &candidates[candidate_count];
    if (!thinking_curve_parse_record_move(raw_move, &candidate->move)) {
      log_fatal("invalid thinking-curve raw move");
    }
    (void)snprintf(candidate->raw_move, sizeof(candidate->raw_move), "%s",
                   raw_move);
    candidate->oracle_utility = strtod(oracle_utility_value, NULL);
    candidate->oracle_win = strtod(oracle_win_value, NULL);
    candidate->oracle_spread = strtod(oracle_spread_value, NULL);
    candidate_count++;

    free(position_value);
    free(game_value);
    free(bag_value);
    free(rank_value);
    free(candidate_count_value);
    free(oracle_utility_value);
    free(oracle_win_value);
    free(oracle_spread_value);
    free(raw_move);
  }

  if (evaluated < max_positions && current_position >= 0 &&
      !(wall_seconds > 0.0 && ctimer_elapsed_seconds(&timer) >= wall_seconds)) {
    if (candidate_count != expected_candidates) {
      log_fatal("incomplete final thinking-curve candidate group");
    }
    if (groups_seen >= skip_positions) {
      thinking_curve_run_position(
          config, &context, candidate_moves, candidates, candidate_count,
          current_cgp, groups_seen, current_position, current_game, current_bag,
          num_plays, plies, max_nodes, uniform_floor_per_mille, targets,
          target_count, judge_plies, judge_samples, judge_risk_plays,
          capture_regret_samples, regret_risk_plays, regret_paired_samples,
          regret_stop_target, regret_cross_arm_correlation, regret_calibration,
          regret_check_interval, regret_min_samples_per_arm, fixed_control);
      evaluated++;
    }
    groups_seen++;
  }
  free(current_cgp);
  free(line);
  fclose_or_die(stream);

  printf("THINKING_CURVE_DONE plies=%d evaluated=%d skip_positions=%d "
         "next_skip=%d elapsed_seconds=%.6f wall_limit_reached=%d\n",
         plies, evaluated, skip_positions, skip_positions + evaluated,
         ctimer_elapsed_seconds(&timer), wall_limit_reached);
  fflush_or_die(stdout);

  move_list_destroy(candidate_moves);
  move_list_destroy(generated_moves);
  thinking_curve_context_destroy(&context);
  config_destroy(config);
}
