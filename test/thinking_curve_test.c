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
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/simmer.h"
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

// Observation-only calibration harness for the anytime simulation curve.
//
// One long BAI run supplies all checkpoints for a position. This preserves
// the actual adaptive sampling trajectory and avoids independently rerunning
// every budget. The source corpus's oracle values provide an immediate,
// deliberately labeled provisional regret curve. Every selected raw move and
// source CGP is also emitted so the much stronger 10-ply oracle can later
// adjudicate only the distinct checkpoint nominees.

enum {
  THINKING_CURVE_MAX_CANDIDATES = 128,
  THINKING_CURVE_MAX_TARGETS = 32,
  THINKING_CURVE_RAW_MOVE_CAPACITY = 160,
};

static const uint64_t THINKING_CURVE_BASE_SEED = UINT64_C(0x5448494e4b435552);
static const uint64_t THINKING_CURVE_SEED_STRIDE = UINT64_C(0x9e3779b97f4a7c15);

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

static const AnalysisProgressEvent *thinking_curve_find_target(
    const AnalysisProgressEvent events[], size_t count, uint64_t target_nodes,
    const AnalysisProgressEvent *finish, uint64_t requested_max_nodes) {
  for (size_t index = 0; index < count; index++) {
    const AnalysisProgressEvent *event = &events[index];
    if (event->mode == ANALYSIS_MODE_SIM &&
        event->event == ANALYSIS_EVENT_CHECKPOINT && event->best_index >= 0 &&
        event->nodes >= target_nodes) {
      return event;
    }
  }
  // A requested cap need not be divisible by nodes-per-iteration, and terminal
  // rollouts can end a few nodes early. The final decision is the honest
  // observation for the upper endpoint in either case.
  if (finish != NULL && target_nodes == requested_max_nodes) {
    return finish;
  }
  return NULL;
}

static void thinking_curve_print_point(
    int source_index, int position, int game_index, int bag_tiles, int plies,
    uint64_t target_nodes, bool final, const AnalysisProgressEvent *event,
    const ThinkingCurveCandidate candidates[], int candidate_count,
    int num_plays, int panel_best_index, int candidate_best_index) {
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
  printf("THINKING_CURVE_POINT source_index=%d position=%d game=%d bag=%d "
         "plies=%d target_nodes=%" PRIu64 " final=%d elapsed_ns=%" PRId64
         " iterations=%" PRIu64 " nodes=%" PRIu64
         " selected_rank=%d selected_id=%" PRIu64
         " selected_raw=%s panel_best_rank=%d candidate_best_rank=%d "
         "candidate_regret=%+.9f sampling_regret=%+.9f "
         "provisional_total_regret=%+.9f provisional_win_delta=%+.9f "
         "provisional_spread_delta=%+.9f estimated_best=%.12f "
         "estimated_challenger=%.12f\n",
         source_index, position, game_index, bag_tiles, plies, target_nodes,
         final, event->elapsed_ns, event->iterations, event->nodes, selected,
         move_get_fingerprint(&candidates[selected].move),
         candidates[selected].raw_move, panel_best_index, candidate_best_index,
         candidate_regret, sampling_regret, total_regret,
         candidates[selected].oracle_win -
             candidates[panel_best_index].oracle_win,
         candidates[selected].oracle_spread -
             candidates[panel_best_index].oracle_spread,
         event->best_value, event->challenger_value);
}

static void thinking_curve_run_position(
    Config *config, ThinkingCurveContext *context, MoveList *candidate_moves,
    const ThinkingCurveCandidate candidates[], int candidate_count,
    const char *cgp, int source_index, int position, int game_index,
    int bag_tiles, int num_plays, int plies, uint64_t max_nodes,
    uint64_t checkpoint_nodes, uint64_t min_play_iterations,
    const uint64_t targets[], int target_count) {
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

  const uint64_t nodes_per_iteration = (uint64_t)plies + 1;
  const uint64_t max_iterations = max_nodes / nodes_per_iteration;
  const uint64_t checkpoint_iterations =
      checkpoint_nodes / nodes_per_iteration > 0
          ? checkpoint_nodes / nodes_per_iteration
          : 1;
  if (max_iterations == 0 ||
      (uint64_t)num_plays * min_play_iterations > max_iterations) {
    log_fatal("thinking-curve iteration budget is too small");
  }
  const uint64_t expected_events = max_iterations / checkpoint_iterations + 32;
  if (expected_events > SIZE_MAX) {
    log_fatal("thinking-curve trace capacity overflow");
  }
  AnalysisTrace *trace = analysis_trace_create((size_t)expected_events);
  AnalysisProgressListener listener = {
      .callback = analysis_trace_record,
      .user_data = trace,
      .run_id = ((uint64_t)(source_index + 1) << 8) | (uint64_t)plies,
      .checkpoint_interval = checkpoint_iterations,
  };

  ThreadControl *thread_control = config_get_thread_control(config);
  assert(
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED));
  rack_set_dist_size_and_reset(&context->known_opponent_rack,
                               ld_get_size(config_get_ld(config)));
  SimArgs sim_args = {0};
  sim_args_fill(plies, candidate_moves, num_plays,
                &context->known_opponent_rack, config_get_win_pcts(config),
                /*inference_results=*/NULL, thread_control, game,
                /*sim_with_inference=*/false, /*use_heat_map=*/false,
                config_get_num_threads(config), /*print_interval=*/0,
                /*max_num_display_plays=*/num_plays,
                /*max_num_display_plies=*/plies,
                THINKING_CURVE_BASE_SEED +
                    (uint64_t)(position + 1) * THINKING_CURVE_SEED_STRIDE,
                max_iterations, min_play_iterations,
                /*scond=*/0.0, BAI_THRESHOLD_NONE,
                /*time_limit_seconds=*/0.0, BAI_SAMPLING_RULE_TOP_TWO_IDS,
                // Keep every position on the requested fixed work budget.
                /*cutoff=*/-1.0, config_get_utility_w_winpct(config),
                config_get_utility_w_spread(config),
                config_get_utility_spread_scale(config),
                /*inference_args=*/NULL, &sim_args);
  sim_args.progress_listener = listener;

  ErrorStack *error_stack = error_stack_create();
  simulate(&sim_args, &context->sim_ctx, context->sim_results, error_stack);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_FINISHED);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("thinking-curve simulation failed");
  }
  error_stack_destroy(error_stack);

  const size_t event_count = analysis_trace_get_count(trace);
  AnalysisProgressEvent *events = malloc_or_die(event_count * sizeof(*events));
  for (size_t index = 0; index < event_count; index++) {
    if (!analysis_trace_get_event(trace, index, &events[index])) {
      log_fatal("thinking-curve trace changed after solve");
    }
  }
  if (analysis_trace_get_dropped(trace) != 0) {
    log_fatal("thinking-curve trace dropped events");
  }
  const AnalysisProgressEvent *finish =
      thinking_curve_find_finish(events, event_count);
  if (finish == NULL) {
    log_fatal("thinking-curve trace has no final decision");
  }

  printf(
      "THINKING_CURVE_POSITION source_index=%d position=%d game=%d bag=%d "
      "plies=%d candidates=%d max_nodes=%" PRIu64 " checkpoint_nodes=%" PRIu64
      " min_play_iterations=%" PRIu64 " panel_best_rank=%d cgp=%s\n",
      source_index, position, game_index, bag_tiles, plies, num_plays,
      max_nodes, checkpoint_nodes, min_play_iterations, panel_best_index, cgp);
  for (int target_index = 0; target_index < target_count; target_index++) {
    if (targets[target_index] > max_nodes) {
      continue;
    }
    const AnalysisProgressEvent *event = thinking_curve_find_target(
        events, event_count, targets[target_index], finish, max_nodes);
    if (event != NULL) {
      thinking_curve_print_point(source_index, position, game_index, bag_tiles,
                                 plies, targets[target_index], /*final=*/false,
                                 event, candidates, candidate_count, num_plays,
                                 panel_best_index, candidate_best_index);
    }
  }
  thinking_curve_print_point(
      source_index, position, game_index, bag_tiles, plies,
      /*target_nodes=*/0, /*final=*/true, finish, candidates, candidate_count,
      num_plays, panel_best_index, candidate_best_index);
  printf("THINKING_CURVE_POSITION_DONE source_index=%d position=%d plies=%d "
         "events=%zu dropped=0 final_iterations=%" PRIu64
         " final_nodes=%" PRIu64 "\n",
         source_index, position, plies, event_count, finish->iterations,
         finish->nodes);
  fflush_or_die(stdout);

  free(events);
  analysis_trace_destroy(trace);
}

void test_thinking_curve(void) {
  log_set_level(LOG_FATAL);
  const char *corpus = thinking_curve_env_string(
      "THINKING_CURVE_CORPUS",
      "obj/thinking-curves-candidates-bag50-644x60.log");
  const int skip_positions =
      thinking_curve_env_nonnegative_int("THINKING_CURVE_SKIP_POSITIONS", 0);
  const int max_positions =
      thinking_curve_env_positive_int("THINKING_CURVE_MAX_POSITIONS", 8);
  const int num_plays =
      thinking_curve_env_positive_int("THINKING_CURVE_NUM_PLAYS", 60);
  const int plies = thinking_curve_env_positive_int("THINKING_CURVE_PLIES", 4);
  const int num_threads = thinking_curve_env_positive_int(
      "THINKING_CURVE_THREADS", get_num_cores());
  const uint64_t max_nodes = thinking_curve_env_positive_uint64(
      "THINKING_CURVE_MAX_NODES", UINT64_C(10000000));
  const uint64_t checkpoint_nodes = thinking_curve_env_positive_uint64(
      "THINKING_CURVE_CHECKPOINT_NODES", UINT64_C(25000));
  const uint64_t min_play_iterations = thinking_curve_env_positive_uint64(
      "THINKING_CURVE_MIN_PLAY_ITERATIONS", UINT64_C(100));
  const double wall_seconds =
      thinking_curve_env_nonnegative_double("THINKING_CURVE_WALL_SECONDS", 0.0);
  const char *target_string = thinking_curve_env_string(
      "THINKING_CURVE_TARGET_NODES",
      "100000,200000,300000,500000,750000,1000000,1500000,2000000,"
      "3000000,5000000,7500000,10000000");
  uint64_t targets[THINKING_CURVE_MAX_TARGETS];
  const int target_count = thinking_curve_parse_targets(target_string, targets);
  if (num_plays < 2 || num_plays > THINKING_CURVE_MAX_CANDIDATES) {
    log_fatal("THINKING_CURVE_NUM_PLAYS must be between 2 and %d",
              THINKING_CURVE_MAX_CANDIDATES);
  }
  if (plies < 1 || plies > MAX_PLIES) {
    log_fatal("THINKING_CURVE_PLIES must be between 1 and %d", MAX_PLIES);
  }

  FILE *stream = fopen(corpus, "re");
  if (stream == NULL) {
    log_fatal("could not open thinking-curve corpus: %s", corpus);
  }
  Config *config = thinking_curve_create_config(num_threads, num_plays);
  ThinkingCurveContext context;
  thinking_curve_context_init(&context, config);
  MoveList *candidate_moves = move_list_create(num_plays);
  Timer timer;
  ctimer_start(&timer);
  printf("THINKING_CURVE_CONFIG corpus=%s skip_positions=%d max_positions=%d "
         "num_plays=%d plies=%d threads=%d max_nodes=%" PRIu64
         " checkpoint_nodes=%" PRIu64 " min_play_iterations=%" PRIu64
         " wall_seconds=%.3f targets=%s oracle=provisional_panel "
         "sampling_rule=top_two_ids\n",
         corpus, skip_positions, max_positions, num_plays, plies, num_threads,
         max_nodes, checkpoint_nodes, min_play_iterations, wall_seconds,
         target_string);
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
            current_bag, num_plays, plies, max_nodes, checkpoint_nodes,
            min_play_iterations, targets, target_count);
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
      thinking_curve_run_position(config, &context, candidate_moves, candidates,
                                  candidate_count, current_cgp, groups_seen,
                                  current_position, current_game, current_bag,
                                  num_plays, plies, max_nodes, checkpoint_nodes,
                                  min_play_iterations, targets, target_count);
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
  thinking_curve_context_destroy(&context);
  config_destroy(config);
}
