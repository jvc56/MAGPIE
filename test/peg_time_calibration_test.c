#include "peg_time_calibration_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <glob.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  PEG_CAL_DEFAULT_THREADS = 10,
  PEG_CAL_REFINE_CANDS = 32,
  PEG_CAL_MAX_EVENTS = 32768,
  PEG_CAL_MAX_NOMINEES = 16,
  PEG_CAL_MAX_STAGES = 16,
};

typedef struct PegCalEvent {
  int stage;
  int rank;
  Move move;
  double win;
  double spread;
  int scenarios;
  uint64_t nested_endgame_nodes;
  double process_cpu_seconds;
  int64_t completed_ns;
} PegCalEvent;

typedef struct PegCalTrace {
  int64_t start_ns;
  double cpu_start;
  atomic_int event_count;
  PegCalEvent events[PEG_CAL_MAX_EVENTS];
} PegCalTrace;

static const char *peg_cal_required_env(const char *name) {
  const char *value = getenv(name);
  if (value == NULL || *value == '\0') {
    log_fatal("peg time calibration requires %s", name);
  }
  return value;
}

static int peg_cal_env_int(const char *name, int default_value) {
  const char *value = getenv(name);
  return value != NULL && *value != '\0' ? (int)strtol(value, NULL, 10)
                                         : default_value;
}

static double peg_cal_env_double(const char *name, double default_value) {
  const char *value = getenv(name);
  return value != NULL && *value != '\0' ? strtod(value, NULL) : default_value;
}

static int peg_cal_parse_schedule(const char *text, int *schedule,
                                  int schedule_capacity) {
  if (text == NULL || *text == '\0') {
    schedule[0] = PEG_CAL_REFINE_CANDS;
    return 1;
  }
  char *copy = string_duplicate(text);
  char *saveptr = NULL;
  int count = 0;
  for (char *token = strtok_r(copy, ",", &saveptr); token != NULL;
       token = strtok_r(NULL, ",", &saveptr)) {
    if (count >= schedule_capacity) {
      free(copy);
      log_fatal("PEG calibration schedule has more than %d stages",
                schedule_capacity);
    }
    char *end = NULL;
    const long value = strtol(token, &end, 10);
    if (end == token || *end != '\0' || value < 2 || value > INT32_MAX) {
      free(copy);
      log_fatal("invalid PEG calibration stage cap '%s'", token);
    }
    schedule[count++] = (int)value;
  }
  free(copy);
  if (count == 0) {
    log_fatal("PEG calibration schedule is empty");
  }
  return count;
}

static double peg_cal_process_cpu_seconds(void) {
  struct timespec ts;
  // glibc defines this clock id in an internal header that include-cleaner
  // cannot attribute to the directly included <time.h>.
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
  }
  return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void peg_cal_move_string(const Game *game, const Move *move, char *out,
                                size_t out_size) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_ucgi_move(sb, move, game_get_board(game),
                               game_get_ld(game));
  (void)snprintf(out, out_size, "%s", string_builder_peek(sb));
  string_builder_destroy(sb);
}

static void peg_cal_on_cand_done(int stage, int rank, const Move *move,
                                 double win, double spread, int scenarios,
                                 uint64_t nested_endgame_nodes,
                                 int64_t completed_ns, bool reordered,
                                 void *user_data) {
  (void)reordered;
  PegCalTrace *trace = (PegCalTrace *)user_data;
  const int event_idx =
      atomic_fetch_add_explicit(&trace->event_count, 1, memory_order_relaxed);
  if (event_idx >= PEG_CAL_MAX_EVENTS) {
    return;
  }
  PegCalEvent *event = &trace->events[event_idx];
  event->stage = stage;
  event->rank = rank;
  event->move = *move;
  event->win = win;
  event->spread = spread;
  event->scenarios = scenarios;
  event->nested_endgame_nodes = nested_endgame_nodes;
  event->process_cpu_seconds = peg_cal_process_cpu_seconds() - trace->cpu_start;
  event->completed_ns = completed_ns;
}

static int peg_cal_event_cmp(const void *lhs, const void *rhs) {
  const PegCalEvent *a = (const PegCalEvent *)lhs;
  const PegCalEvent *b = (const PegCalEvent *)rhs;
  if (a->completed_ns < b->completed_ns) {
    return -1;
  }
  if (a->completed_ns > b->completed_ns) {
    return 1;
  }
  if (a->stage != b->stage) {
    return a->stage - b->stage;
  }
  return a->rank - b->rank;
}

static uint64_t peg_cal_print_events(PegCalTrace *trace, const Game *game,
                                     const char *mode, const char *position,
                                     const char *label) {
  int event_count =
      atomic_load_explicit(&trace->event_count, memory_order_relaxed);
  const int dropped =
      event_count > PEG_CAL_MAX_EVENTS ? event_count - PEG_CAL_MAX_EVENTS : 0;
  if (event_count > PEG_CAL_MAX_EVENTS) {
    event_count = PEG_CAL_MAX_EVENTS;
  }
  qsort(trace->events, (size_t)event_count, sizeof(PegCalEvent),
        peg_cal_event_cmp);
  uint64_t cumulative_scenarios = 0;
  for (int event_idx = 0; event_idx < event_count; event_idx++) {
    const PegCalEvent *event = &trace->events[event_idx];
    if (event->scenarios > 0) {
      cumulative_scenarios += (uint64_t)event->scenarios;
    }
    char move[64];
    peg_cal_move_string(game, &event->move, move, sizeof(move));
    (void)printf(
        "PEGCAL_EVENT\tmode=%s\tposition=%s\tlabel=%s\tstage=%d\trank=%d"
        "\tmove=%s\twin=%.9f\tspread=%.6f\tscenarios=%d"
        "\tcumulative_scenarios=%" PRIu64 "\tnested_endgame_nodes=%" PRIu64
        "\tcompleted_seconds=%.9f\tprocess_cpu_seconds=%.9f\n",
        mode, position, label, event->stage, event->rank, move, event->win,
        event->spread, event->scenarios, cumulative_scenarios,
        event->nested_endgame_nodes,
        (double)(event->completed_ns - trace->start_ns) / 1.0e9,
        event->process_cpu_seconds);
  }
  if (dropped > 0) {
    (void)printf("PEGCAL_WARNING\tmode=%s\tposition=%s\tlabel=%s"
                 "\tevents_dropped=%d\n",
                 mode, position, label, dropped);
  }
  return cumulative_scenarios;
}

static Config *peg_cal_load_config(void) {
  const int threads =
      peg_cal_env_int("PEG_CAL_THREADS", PEG_CAL_DEFAULT_THREADS);
  char *config_command = get_formatted_string(
      "set -lex CSW24 -wmp true -rit true -ritmmap true -wit false "
      "-threads %d -s1 equity -s2 equity -r1 all -r2 all",
      threads);
  Config *config = config_create_or_die(config_command);
  free(config_command);
  const char *cgp = peg_cal_required_env("PEG_CAL_CGP");
  char *command = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, command);
  free(command);
  return config;
}

static void peg_cal_print_stages(const PegResult *result, const char *mode,
                                 const char *position, const char *label,
                                 int64_t solve_start_ns) {
  for (int stage = 0; stage < result->n_stage_history; stage++) {
    const PegStageSnapshot *snapshot = &result->stage_history[stage];
    const double start_seconds =
        (double)(snapshot->start_ns - solve_start_ns) / 1.0e9;
    const double end_seconds =
        snapshot->end_ns > 0
            ? (double)(snapshot->end_ns - solve_start_ns) / 1.0e9
            : -1.0;
    (void)printf("PEGCAL_STAGE\tmode=%s\tposition=%s\tlabel=%s\tstage=%d"
                 "\tfidelity=%d\tcandidate_total=%d\tcompleted_candidates=%d"
                 "\tstart_seconds=%.9f\tend_seconds=%.9f\n",
                 mode, position, label, stage, snapshot->fidelity_plies,
                 snapshot->field_size, snapshot->cands_done, start_seconds,
                 end_seconds);
  }
}

static void peg_cal_run_arm(const Config *config, const char *mode,
                            const char *position, const char *label) {
  static const int sentinel_schedule[] = {8};
  const bool greedy = strcmp(label, "greedy") == 0;
  const bool sentinel = strcmp(mode, "sentinel") == 0;
  const int threads =
      peg_cal_env_int("PEG_CAL_THREADS", PEG_CAL_DEFAULT_THREADS);
  if (threads < 1) {
    log_fatal("invalid PEG calibration thread count %d", threads);
  }
  const double budget =
      sentinel ? 0.0 : peg_cal_env_double("PEG_CAL_BUDGET", 0.0);
  int refine_schedule[PEG_CAL_MAX_STAGES] = {0};
  const int refine_stages =
      sentinel ? 1
               : peg_cal_parse_schedule(getenv("PEG_CAL_SCHEDULE"),
                                        refine_schedule, PEG_CAL_MAX_STAGES);
  const int *schedule = sentinel ? sentinel_schedule : refine_schedule;
  const int requested_stages = sentinel ? 1 : refine_stages;

  PegPoll *poll = peg_poll_create();
  PegCalTrace *trace = calloc_or_die(1, sizeof(*trace));
  atomic_init(&trace->event_count, 0);
  trace->start_ns = ctimer_monotonic_ns();
  trace->cpu_start = peg_cal_process_cpu_seconds();

  PegArgs args = {0};
  args.game = config_get_game(config);
  args.thread_control = config_get_thread_control(config);
  args.num_threads = threads;
  args.time_budget_seconds = budget;
  args.greedy_seed_only = greedy;
  args.stage_top_k = schedule;
  args.num_stages = requested_stages;
  args.scenario_stride = 1;
  args.nested_enabled = false;
  args.on_cand_done = peg_cal_on_cand_done;
  args.user_data = trace;
  args.poll = poll;

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  const double cpu_start = peg_cal_process_cpu_seconds();
  peg_solve(&args, &result, error_stack);
  const double cpu_seconds = peg_cal_process_cpu_seconds() - cpu_start;
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("PEG calibration arm failed");
  }

  const double wall_seconds = ctimer_elapsed_seconds(&result.timer);
  peg_cal_print_stages(&result, mode, position, label, trace->start_ns);
  const uint64_t cumulative_scenarios = peg_cal_print_events(
      trace, config_get_game(config), mode, position, label);
  const int stage_idx = result.n_stage_history > 1 ? 1 : -1;
  const int refine_total =
      stage_idx >= 0 ? result.stage_history[stage_idx].field_size : 0;
  const int refine_completed =
      stage_idx >= 0 ? result.stage_history[stage_idx].cands_done : 0;
  const int deepest_stage_idx =
      result.n_stage_history > requested_stages ? requested_stages : -1;
  const int deepest_total =
      deepest_stage_idx >= 0
          ? result.stage_history[deepest_stage_idx].field_size
          : 0;
  const int deepest_completed =
      deepest_stage_idx >= 0
          ? result.stage_history[deepest_stage_idx].cands_done
          : 0;
  const int root_total =
      result.n_stage_history > 0 ? result.stage_history[0].field_size : 0;
  const int root_completed =
      result.n_stage_history > 0 ? result.stage_history[0].cands_done : 0;
  const bool refine_is_complete =
      greedy || (deepest_stage_idx >= 0 &&
                 result.stage_history[deepest_stage_idx].end_ns > 0 &&
                 deepest_completed == deepest_total &&
                 result.last_completed_stage == requested_stages &&
                 !result.last_stage_partial);
  const bool refine_is_partial =
      !greedy && stage_idx >= 0 && !refine_is_complete;
  const char *status = refine_is_complete
                           ? "completed"
                           : (stage_idx >= 0 ? "partial" : "seed_only");
  char move[64];
  peg_cal_move_string(config_get_game(config), &result.best_move, move,
                      sizeof(move));
  const double occupancy =
      wall_seconds > 0.0 ? cpu_seconds / (wall_seconds * (double)threads) : 0.0;
  (void)printf(
      "PEGCAL_ARM\tmode=%s\tposition=%s\tlabel=%s\tbudget_seconds=%.3f"
      "\tmove=%s\tself_win=%.9f\tself_spread=%.6f"
      "\tlast_completed_stage=%d\tstatus=%s\tpartial=%d"
      "\tpublished_partial=%d"
      "\troot_candidate_total=%d\troot_completed_candidates=%d"
      "\trefine_candidate_total=%d\trefine_completed_candidates=%d"
      "\trequested_stages=%d\tdeepest_candidate_total=%d"
      "\tdeepest_completed_candidates=%d"
      "\tcumulative_scenarios=%" PRIu64 "\tnested_endgame_nodes=%" PRIu64
      "\twall_seconds=%.9f\tprocess_cpu_seconds=%.9f"
      "\tscheduled_core_occupancy=%.9f\tthreads=%d\n",
      mode, position, label, budget, move, result.best_win, result.best_spread,
      result.last_completed_stage, status, refine_is_partial ? 1 : 0,
      result.last_stage_partial ? 1 : 0, root_total, root_completed,
      refine_total, refine_completed, requested_stages, deepest_total,
      deepest_completed, cumulative_scenarios, result.nested_endgame_nodes,
      wall_seconds, cpu_seconds, occupancy, threads);
  (void)fflush(stdout);

  error_stack_destroy(error_stack);
  peg_result_destroy(&result);
  peg_poll_destroy(poll);
  free(trace);
}

static int peg_cal_parse_nominees(const Game *game, ValidatedMoves **validated,
                                  const Move **nominees,
                                  char move_strings[][64]) {
  char *moves = string_duplicate(peg_cal_required_env("PEG_CAL_MOVES"));
  int count = 0;
  char *saveptr = NULL;
  for (char *token = strtok_r(moves, "|", &saveptr); token != NULL;
       token = strtok_r(NULL, "|", &saveptr)) {
    if (count >= PEG_CAL_MAX_NOMINEES) {
      free(moves);
      log_fatal("too many PEG calibration nominees");
    }
    ErrorStack *error_stack = error_stack_create();
    validated[count] = validated_moves_create(
        game, game_get_player_on_turn_index(game), token,
        /*allow_phonies=*/false, /*allow_playthrough=*/true, error_stack);
    if (!error_stack_is_empty(error_stack) ||
        validated_moves_get_number_of_moves(validated[count]) != 1) {
      error_stack_print_and_reset(error_stack);
      error_stack_destroy(error_stack);
      free(moves);
      log_fatal("failed to parse PEG calibration nominee '%s'", token);
    }
    error_stack_destroy(error_stack);
    nominees[count] = validated_moves_get_move(validated[count], 0);
    (void)snprintf(move_strings[count], 64, "%s", token);
    count++;
  }
  free(moves);
  if (count < 2) {
    log_fatal("direct PEG calibration judge requires at least two nominees");
  }
  return count;
}

static void peg_cal_run_judge(const Config *config, const char *position) {
  ValidatedMoves *validated[PEG_CAL_MAX_NOMINEES] = {0};
  const Move *nominees[PEG_CAL_MAX_NOMINEES] = {0};
  char requested_moves[PEG_CAL_MAX_NOMINEES][64] = {{0}};
  const int nominee_count = peg_cal_parse_nominees(
      config_get_game(config), validated, nominees, requested_moves);
  const int judge_ply = peg_cal_env_int("PEG_CAL_JUDGE_PLY", 3);
  if (judge_ply < 2 || judge_ply > PEG_CAL_MAX_STAGES + 1) {
    log_fatal("invalid PEG calibration judge ply %d", judge_ply);
  }
  const int judge_stages = judge_ply - 1;
  const int threads =
      peg_cal_env_int("PEG_CAL_THREADS", PEG_CAL_DEFAULT_THREADS);
  if (threads < 1) {
    log_fatal("invalid PEG calibration thread count %d", threads);
  }
  int judge_schedule[PEG_CAL_MAX_STAGES] = {0};
  for (int stage = 0; stage < judge_stages; stage++) {
    judge_schedule[stage] = nominee_count;
  }
  const int stride = peg_cal_env_int("PEG_CAL_STRIDE", 4);
  const double budget = peg_cal_env_double("PEG_CAL_BUDGET", 600.0);
  const char *label = peg_cal_required_env("PEG_CAL_LABEL");

  PegPoll *poll = peg_poll_create();
  PegCalTrace *trace = calloc_or_die(1, sizeof(*trace));
  atomic_init(&trace->event_count, 0);
  trace->start_ns = ctimer_monotonic_ns();
  trace->cpu_start = peg_cal_process_cpu_seconds();

  PegArgs args = {0};
  args.game = config_get_game(config);
  args.thread_control = config_get_thread_control(config);
  args.num_threads = threads;
  args.time_budget_seconds = budget;
  args.stage_top_k = judge_schedule;
  args.num_stages = judge_stages;
  args.scenario_stride = stride;
  args.force_small_bag_stride = true;
  args.nested_enabled = false;
  args.only_moves = nominees;
  args.n_only_moves = nominee_count;
  args.on_cand_done = peg_cal_on_cand_done;
  args.user_data = trace;
  args.poll = poll;

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  const double cpu_start = peg_cal_process_cpu_seconds();
  peg_solve(&args, &result, error_stack);
  const double cpu_seconds = peg_cal_process_cpu_seconds() - cpu_start;
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("PEG calibration judge failed");
  }

  const double wall_seconds = ctimer_elapsed_seconds(&result.timer);
  peg_cal_print_stages(&result, "judge", position, label, trace->start_ns);
  const uint64_t cumulative_scenarios = peg_cal_print_events(
      trace, config_get_game(config), "judge", position, label);
  const bool accepted = result.last_completed_stage == judge_stages &&
                        !result.last_stage_partial &&
                        result.n_top_cands == nominee_count;
  const int deepest_stage =
      result.n_stage_history > judge_stages ? judge_stages : -1;
  const bool partial = result.last_stage_partial ||
                       (deepest_stage >= 0 &&
                        (result.stage_history[deepest_stage].end_ns == 0 ||
                         result.stage_history[deepest_stage].cands_done <
                             result.stage_history[deepest_stage].field_size));
  if (accepted) {
    for (int rank = 0; rank < result.n_top_cands; rank++) {
      const PegRankedCand *candidate = &result.top_cands[rank];
      char move[64];
      peg_cal_move_string(config_get_game(config), &candidate->move, move,
                          sizeof(move));
      (void)printf("PEGCAL_VALUE\tposition=%s\tlabel=%s\trank=%d\tmove=%s"
                   "\twin=%.9f\tspread=%.6f\tutility=%.9f"
                   "\tscenarios=%d\tweight_sum=%" PRId64 "\n",
                   position, label, rank, move, candidate->win_pct,
                   candidate->mean_spread,
                   candidate->win_pct + 1.0e-4 * candidate->mean_spread,
                   candidate->n_scenarios, candidate->weight_sum);
    }
  }
  const double occupancy =
      wall_seconds > 0.0 ? cpu_seconds / (wall_seconds * (double)threads) : 0.0;
  (void)printf(
      "PEGCAL_JUDGE\tposition=%s\tlabel=%s\tjudge_ply=%d\tstride=%d"
      "\tbudget_seconds=%.3f\tnominee_count=%d\taccepted=%d"
      "\tlast_completed_stage=%d\tpartial=%d"
      "\tcumulative_scenarios=%" PRIu64 "\tnested_endgame_nodes=%" PRIu64
      "\twall_seconds=%.9f\tprocess_cpu_seconds=%.9f"
      "\tscheduled_core_occupancy=%.9f\tthreads=%d"
      "\tpair_conditioned=1\tcommon_sample=deterministic_weight_strata\n",
      position, label, judge_ply, stride, budget, nominee_count,
      accepted ? 1 : 0, result.last_completed_stage, partial ? 1 : 0,
      cumulative_scenarios, result.nested_endgame_nodes, wall_seconds,
      cpu_seconds, occupancy, threads);
  (void)fflush(stdout);

  error_stack_destroy(error_stack);
  peg_result_destroy(&result);
  peg_poll_destroy(poll);
  free(trace);
  for (int nominee = 0; nominee < nominee_count; nominee++) {
    validated_moves_destroy(validated[nominee]);
  }
}

void test_peg_time_calibration(void) {
  log_set_level(LOG_FATAL);
  const char *mode = peg_cal_required_env("PEG_CAL_MODE");
  const char *position = peg_cal_required_env("PEG_CAL_POSITION");
  Config *config = peg_cal_load_config();
  if (strcmp(mode, "judge") == 0) {
    peg_cal_run_judge(config, position);
  } else if (strcmp(mode, "arm") == 0 || strcmp(mode, "sentinel") == 0) {
    peg_cal_run_arm(config, mode, position,
                    peg_cal_required_env("PEG_CAL_LABEL"));
  } else {
    config_destroy(config);
    log_fatal("unknown PEG_CAL_MODE '%s'", mode);
  }
  config_destroy(config);
}

static const Rack *peg_cal_next_player_rack(const GameHistory *history,
                                            int event_index, int player_index) {
  const int event_count = game_history_get_num_events(history);
  for (int index = event_index + 1; index < event_count; index++) {
    const GameEvent *event = game_history_get_event(history, index);
    const game_event_t type = game_event_get_type(event);
    if (game_event_get_player_index(event) == player_index &&
        (type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
         type == GAME_EVENT_EXCHANGE || type == GAME_EVENT_PASS)) {
      return game_event_get_const_rack(event);
    }
  }
  return NULL;
}

static int peg_cal_gcg_starting_player(const char *path) {
  FILE *stream = fopen(path, "re");
  if (stream == NULL) {
    log_fatal("failed to open autoplay GCG %s", path);
  }
  char line[4096];
  int starting_player = -1;
  while (fgets(line, sizeof(line), stream) != NULL) {
    if (line[0] != '>') {
      continue;
    }
    if (has_iprefix(">magpie1:", line)) {
      starting_player = 0;
    } else if (has_iprefix(">magpie2:", line)) {
      starting_player = 1;
    }
    break;
  }
  (void)fclose(stream);
  if (starting_player < 0) {
    log_fatal("failed to find the first autoplay move in %s", path);
  }
  return starting_player;
}

void test_peg_extract_gcg_panel(void) {
  log_set_level(LOG_FATAL);
  const char *directory = peg_cal_required_env("PEG_CAL_GCG_DIR");
  char *pattern = get_formatted_string("%s/*.gcg", directory);
  glob_t paths = {0};
  const int glob_status = glob(pattern, 0, NULL, &paths);
  free(pattern);
  if (glob_status != 0 || paths.gl_pathc == 0) {
    globfree(&paths);
    log_fatal("no GCG files found under %s", directory);
  }

  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -rit true -ritmmap true -wit false "
      "-threads 1 -s1 equity -s2 equity -r1 all -r2 all");
  load_and_exec_config_or_die(config, "new");
  ErrorStack *error_stack = error_stack_create();
  int valid_sources = 0;
  int emitted_by_bag[5] = {0};
  for (size_t source_index = 0; source_index < paths.gl_pathc; source_index++) {
    const int starting_player =
        peg_cal_gcg_starting_player(paths.gl_pathv[source_index]);
    GameHistory *history = game_history_create();
    game_reset(config_get_game(config));
    char *gcg = get_string_from_file(paths.gl_pathv[source_index], error_stack);
    if (starting_player == 1) {
      // Autoplay GCGs identify the first mover by nickname but do not emit a
      // starting-player header. The parser assumes player 1 starts. Since the
      // two autoplay policies are identical, exchange the player labels for
      // player-2-start games before replaying the same trajectory.
      for (char *cursor = gcg; *cursor != '\0'; cursor++) {
        if (strncmp(cursor, ">magpie1:", 9) == 0) {
          cursor[7] = '2';
        } else if (strncmp(cursor, ">magpie2:", 9) == 0) {
          cursor[7] = '1';
        }
      }
    }
    config_parse_gcg_string(config, gcg, history, error_stack);
    free(gcg);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      game_history_destroy(history);
      log_fatal("failed to parse autoplay GCG %s",
                paths.gl_pathv[source_index]);
    }
    Game *game = config_game_create(config);
    game_set_starting_player_index(game, 0);
    bool emitted[5] = {false};
    int move_turn = 0;
    const int event_count = game_history_get_num_events(history);
    for (int event_index = 0; event_index < event_count; event_index++) {
      const GameEvent *event = game_history_get_event(history, event_index);
      const game_event_t type = game_event_get_type(event);
      if (type != GAME_EVENT_TILE_PLACEMENT_MOVE &&
          type != GAME_EVENT_EXCHANGE && type != GAME_EVENT_PASS) {
        continue;
      }
      game_set_starting_player_index(game, 0);
      game_play_n_events(history, game, event_index, false, error_stack);
      if (!error_stack_is_empty(error_stack)) {
        error_stack_print_and_reset(error_stack);
        game_destroy(game);
        game_history_destroy(history);
        log_fatal("failed to replay %s through event %d",
                  paths.gl_pathv[source_index], event_index);
      }
      const int on_turn = game_event_get_player_index(event);
      const int off_turn = 1 - on_turn;
      const Rack *on_turn_rack = game_event_get_const_rack(event);
      const int unseen = bag_get_letters(game_get_bag(game)) +
                         rack_get_total_letters(
                             player_get_rack(game_get_player(game, off_turn)));
      const int peg_bag = unseen - RACK_SIZE;
      if (peg_bag >= 1 && peg_bag <= 4 && !emitted[peg_bag] &&
          rack_get_total_letters(on_turn_rack) == RACK_SIZE) {
        const Rack *off_turn_rack =
            peg_cal_next_player_rack(history, event_index, off_turn);
        if (off_turn_rack != NULL &&
            rack_get_total_letters(off_turn_rack) == RACK_SIZE) {
          return_rack_to_bag(game, off_turn);
          if (!draw_rack_from_bag(game, off_turn, off_turn_rack)) {
            game_destroy(game);
            game_history_destroy(history);
            globfree(&paths);
            error_stack_destroy(error_stack);
            config_destroy(config);
            log_fatal("failed to reconstruct off-turn rack for %s event %d",
                      paths.gl_pathv[source_index], event_index);
          }
          char *cgp = game_get_cgp(game, true);
          (void)printf(
              "PEG_PANEL\tsource_index=%zu\tsource=%s\tevent=%d\tturn=%d"
              "\tbag=%d\tcgp=%s -lex CSW24\n",
              source_index, paths.gl_pathv[source_index], event_index,
              move_turn, peg_bag, cgp);
          free(cgp);
          emitted[peg_bag] = true;
          emitted_by_bag[peg_bag]++;
        }
      }
      move_turn++;
    }
    bool any = false;
    for (int bag = 1; bag <= 4; bag++) {
      any = any || emitted[bag];
    }
    valid_sources += any ? 1 : 0;
    game_destroy(game);
    game_history_destroy(history);
  }
  (void)printf("PEG_PANEL_DONE\tsources=%zu\tvalid_sources=%d"
               "\tbag1=%d\tbag2=%d\tbag3=%d\tbag4=%d\n",
               paths.gl_pathc, valid_sources, emitted_by_bag[1],
               emitted_by_bag[2], emitted_by_bag[3], emitted_by_bag[4]);
  (void)fflush(stdout);

  globfree(&paths);
  error_stack_destroy(error_stack);
  config_destroy(config);
}
