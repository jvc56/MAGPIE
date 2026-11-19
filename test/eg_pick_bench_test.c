// LAB (not for merge): the endgame solver's choices with and without the
// empty-bag pick (src/impl/eg_pick.h), judged by deeper stock solves.
//
// egpickq:file=<f>,plies=<p>,refplies=<r>,threads=<t>[,first=<k>][,max=<n>]
// reads lines "<id> <cgp>" (positions with an empty bag) and, for each:
//   phase 1: solves to <p> plies, with eg_pick as the environment sets it,
//            and keeps the best move;
//   phase 2: with eg_pick suppressed, solves to <r> plies with that move as
//            the actual move.
// Prints, per position,
//   Q <id> <tiles on both racks> <value at p> <ref best> <ref of chosen>
//     <seconds of the p-ply solve> <nodes of the p-ply solve> <move>
// and the eg_pick counters of phase 1:
//   QSTATS picks=<n> candidates=<n> reply_movegens=<n> ns=<n>
// Every solve is single-threaded with its own small transposition table.

#include "eg_pick_bench_test.h"

#include "../src/compat/ctime.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/transposition_table.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/eg_pick.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/ent/equity.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  EGQ_MAX_THREADS = 16,
  EGQ_LINE = 4096,
  EGQ_ID = 64,
  EGQ_MOVE_STRING = 64,
  EGQ_TT_SIZE_POWER = 20,
  EGT_MAX_PLIES = 25,
};

static const double EGQ_TT_FRACTION = 0.0001;

typedef struct EgqPosition {
  char id[EGQ_ID];
  char *cgp;
  int tiles;
  int value;
  double seconds;
  uint64_t nodes;
  bool has_move;
  Move move;
  char move_string[EGQ_MOVE_STRING];
  int ref_best;
  int ref_actual;
  // Whether ref_best and ref_actual came from known= instead of a solve.
  bool ref_known;
} EgqPosition;

typedef struct EgqShared {
  const Config *config;
  EgqPosition *positions;
  int num_positions;
  int plies;
  int ref_plies;
  bool reference_phase;
  int next;
  pthread_mutex_t mutex;
} EgqShared;

static int egq_next(EgqShared *shared) {
  pthread_mutex_lock(&shared->mutex);
  const int position_idx = shared->next++;
  pthread_mutex_unlock(&shared->mutex);
  return position_idx;
}

static void egq_solve_timed(Game *game, EndgameCtx **ctx,
                            EndgameResults *results, TranspositionTable *tt,
                            int plies, const Move *actual,
                            double time_limit_seconds) {
  ThreadControl *thread_control = thread_control_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  EndgameArgs args;
  endgame_args_fill(
      thread_control, game, EGQ_TT_FRACTION, plies,
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, /*num_threads=*/1,
      /*use_heuristics=*/true, /*num_top_moves=*/1, NULL, NULL, NULL, NULL,
      NULL, NULL, DUAL_LEXICON_MODE_IGNORANT, /*forced_pass_bypass=*/true,
      /*incremental_movegen=*/true, /*disable_best_reply_shortcuts=*/false,
      /*enable_pv_display=*/false, time_limit_seconds, time_limit_seconds,
      /*seed=*/1,
      /*skip_word_pruning=*/false, tt, 0, false, 0, false, 0, 0, 0, actual,
      &args);
  ErrorStack *error_stack = error_stack_create();
  endgame_solve(ctx, &args, results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    log_fatal("egpickq: endgame solve failed");
  }
  error_stack_destroy(error_stack);
  thread_control_destroy(thread_control);
}

static void egq_solve(Game *game, EndgameCtx **ctx, EndgameResults *results,
                      TranspositionTable *tt, int plies, const Move *actual) {
  egq_solve_timed(game, ctx, results, tt, plies, actual, 0.0);
}

// Equal-time mode (spec has tlim=<milliseconds>): each position is solved
// twice under the same time limit, stock (eg_pick suppressed) and with eg_pick
// as the environment sets it, alternating which goes first. Where the two
// choose different moves, each move is valued by a stock solve under
// jtlim=<milliseconds> with that move as the actual move. Prints
//   T <id> <tiles> <stock value> <stock depth> <switch value> <switch depth>
//     <judge of stock's move> <its depth> <judge of switch's move> <its depth>
//     <stock move> | <switch move>
// Judge fields are 0 0 0 0 where the moves agree. The suppress switch is
// global, so this mode takes it under a mutex around worker creation only:
// each arm keeps its own context, created while the switch is in its state.
typedef struct EgtShared {
  const Config *config;
  EgqPosition *positions;
  int num_positions;
  double time_limit_seconds;
  double judge_limit_seconds;
  int next;
  pthread_mutex_t mutex;
  pthread_mutex_t create_mutex;
} EgtShared;

typedef struct EgtArm {
  EndgameCtx *ctx;
  EndgameResults *results;
  TranspositionTable *tt;
} EgtArm;

typedef enum {
  EGT_ARM_SWITCH,
  EGT_ARM_STOCK,
  EGT_ARM_FLAGS_OFF,
} egt_arm_kind_t;

static void egt_arm_create(EgtShared *shared, EgtArm *arm, Game *game,
                           egt_arm_kind_t suppressed) {
  arm->ctx = NULL;
  arm->results = endgame_results_create();
  arm->tt = transposition_table_create_with_threads(0.0, EGQ_TT_SIZE_POWER, 1);
  // The first solve creates the context's worker, which reads the switch.
  pthread_mutex_lock(&shared->create_mutex);
  if (suppressed == EGT_ARM_FLAGS_OFF) {
    eg_pick_set_arm_flags_suppressed(true);
  } else {
    eg_pick_set_suppressed(suppressed == EGT_ARM_STOCK);
  }
  egq_solve_timed(game, &arm->ctx, arm->results, arm->tt, 1, NULL, 0.0);
  eg_pick_set_suppressed(false);
  pthread_mutex_unlock(&shared->create_mutex);
}

static void egt_arm_destroy(EgtArm *arm) {
  transposition_table_destroy(arm->tt);
  endgame_results_destroy(arm->results);
  endgame_ctx_destroy(arm->ctx);
}

static void egt_move_string(const Game *game, const Move *move, char *out,
                            size_t out_size) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          false);
  (void)snprintf(out, out_size, "%s", string_builder_peek(sb));
  string_builder_destroy(sb);
}

static void *egt_worker_run(void *arg) {
  EgtShared *shared = (EgtShared *)arg;
  Game *game = config_game_create(shared->config);
  load_cgp_or_die(game, shared->positions[0].cgp);
  EgtArm arms[2];
  EgtArm judge;
  Game *after = config_game_create(shared->config);
  // MAGPIE_EGPICK_BASE (any value): the reference arm keeps the pick and only
  // the "arm" flags differ between the two arms.
  egt_arm_create(shared, &arms[0], game,
                 getenv("MAGPIE_EGPICK_BASE") != NULL ? EGT_ARM_FLAGS_OFF
                                                      : EGT_ARM_STOCK);
  egt_arm_create(shared, &arms[1], game, EGT_ARM_SWITCH);
  egt_arm_create(shared, &judge, game, EGT_ARM_STOCK);
  for (;;) {
    pthread_mutex_lock(&shared->mutex);
    const int position_idx = shared->next++;
    pthread_mutex_unlock(&shared->mutex);
    if (position_idx >= shared->num_positions) {
      break;
    }
    const EgqPosition *position = &shared->positions[position_idx];
    load_cgp_or_die(game, position->cgp);
    const int tiles =
        rack_get_total_letters(player_get_rack(game_get_player(game, 0))) +
        rack_get_total_letters(player_get_rack(game_get_player(game, 1)));
    int values[2] = {0, 0};
    int depths[2] = {0, 0};
    double elapsed[2] = {0.0, 0.0};
    bool has_move[2] = {false, false};
    Move moves[2];
    char move_strings[2][EGQ_MOVE_STRING] = {"-", "-"};
    for (int order_idx = 0; order_idx < 2; order_idx++) {
      const int arm_idx = position_idx % 2 == 0 ? order_idx : 1 - order_idx;
      EgtArm *arm = &arms[arm_idx];
      transposition_table_reset(arm->tt);
      const int64_t arm_start_ns = ctimer_monotonic_ns();
      egq_solve_timed(game, &arm->ctx, arm->results, arm->tt,
                      EGT_MAX_PLIES, NULL, shared->time_limit_seconds);
      elapsed[arm_idx] = (double)(ctimer_monotonic_ns() - arm_start_ns) / 1e9;
      values[arm_idx] =
          endgame_results_get_value(arm->results, ENDGAME_RESULT_BEST);
      depths[arm_idx] =
          endgame_results_get_depth(arm->results, ENDGAME_RESULT_BEST);
      const PVLine *pv =
          endgame_results_get_pvline(arm->results, ENDGAME_RESULT_BEST);
      has_move[arm_idx] = pv->num_moves > 0;
      if (has_move[arm_idx]) {
        small_move_to_move(&moves[arm_idx], &pv->moves[0],
                           game_get_board(game));
        egt_move_string(game, &moves[arm_idx], move_strings[arm_idx],
                        EGQ_MOVE_STRING);
      }
    }
    int judged[2] = {0, 0};
    int judge_depths[2] = {0, 0};
    int regrets[2] = {0, 0};
    const bool differ = has_move[0] && has_move[1] &&
                        strcmp(move_strings[0], move_strings[1]) != 0;
    if (differ) {
      // Each move is valued by playing it and solving the position it leaves
      // for the opponent: the move's gain in spread less the opponent's value.
      const int mover_idx = game_get_player_on_turn_index(game);
      const int spread_before =
          equity_to_int(player_get_score(game_get_player(game, mover_idx)) -
                        player_get_score(game_get_player(game, 1 - mover_idx)));
      for (int arm_idx = 0; arm_idx < 2; arm_idx++) {
        game_copy(after, game);
        play_move(&moves[arm_idx], after, NULL);
        judged[arm_idx] =
            equity_to_int(
                player_get_score(game_get_player(after, mover_idx)) -
                player_get_score(game_get_player(after, 1 - mover_idx))) -
            spread_before;
        if (!game_over(after)) {
          transposition_table_reset(judge.tt);
          egq_solve_timed(after, &judge.ctx, judge.results, judge.tt,
                          EGT_MAX_PLIES, NULL, shared->judge_limit_seconds);
          judged[arm_idx] -=
              endgame_results_get_value(judge.results, ENDGAME_RESULT_BEST);
          judge_depths[arm_idx] =
              endgame_results_get_depth(judge.results, ENDGAME_RESULT_BEST);
        }
      }
      const int best_judged = judged[0] > judged[1] ? judged[0] : judged[1];
      regrets[0] = best_judged - judged[0];
      regrets[1] = best_judged - judged[1];
    }
    pthread_mutex_lock(&shared->mutex);
    printf("T %s %d %d %d %d %d %d %d %d %d %s | %s\n", position->id, tiles,
           values[0], depths[0], values[1], depths[1], judged[0],
           judge_depths[0], judged[1], judge_depths[1], move_strings[0],
           move_strings[1]);
    printf("TT %s %.6f %.6f %d %d %d\n", position->id, elapsed[0], elapsed[1],
           regrets[0], regrets[1], differ ? 1 : 0);
    (void)fflush(stdout);
    pthread_mutex_unlock(&shared->mutex);
  }
  egt_arm_destroy(&arms[0]);
  egt_arm_destroy(&arms[1]);
  egt_arm_destroy(&judge);
  game_destroy(after);
  game_destroy(game);
  return NULL;
}

static void *egq_worker_run(void *arg) {
  EgqShared *shared = (EgqShared *)arg;
  Game *game = config_game_create(shared->config);
  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();
  for (;;) {
    const int position_idx = egq_next(shared);
    if (position_idx >= shared->num_positions) {
      break;
    }
    EgqPosition *position = &shared->positions[position_idx];
    load_cgp_or_die(game, position->cgp);
    TranspositionTable *tt =
        transposition_table_create_with_threads(0.0, EGQ_TT_SIZE_POWER, 1);
    if (!shared->reference_phase) {
      position->tiles =
          rack_get_total_letters(player_get_rack(game_get_player(game, 0))) +
          rack_get_total_letters(player_get_rack(game_get_player(game, 1)));
      const int64_t start_ns = ctimer_monotonic_ns();
      egq_solve(game, &ctx, results, tt, shared->plies, NULL);
      position->seconds = (double)(ctimer_monotonic_ns() - start_ns) / 1e9;
      position->nodes = endgame_ctx_get_nodes_searched(ctx);
      position->value =
          endgame_results_get_value(results, ENDGAME_RESULT_BEST);
      const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
      position->has_move = pv->num_moves > 0;
      if (position->has_move) {
        small_move_to_move(&position->move, &pv->moves[0], game_get_board(game));
        StringBuilder *sb = string_builder_create();
        string_builder_add_move(sb, game_get_board(game), &position->move,
                                game_get_ld(game), false);
        (void)snprintf(position->move_string, sizeof(position->move_string),
                       "%s", string_builder_peek(sb));
        string_builder_destroy(sb);
      }
    } else if (position->has_move && !position->ref_known) {
      egq_solve(game, &ctx, results, tt, shared->ref_plies, &position->move);
      if (!endgame_results_get_actual_move_found(results)) {
        log_fatal("egpickq: chosen move not found in %s", position->id);
      }
      position->ref_best =
          endgame_results_get_value(results, ENDGAME_RESULT_BEST);
      position->ref_actual =
          endgame_results_get_value(results, ENDGAME_RESULT_ACTUAL);
    }
    transposition_table_destroy(tt);
  }
  endgame_results_destroy(results);
  endgame_ctx_destroy(ctx);
  game_destroy(game);
  return NULL;
}

static void egq_run_phase(EgqShared *shared, int threads,
                          bool reference_phase) {
  shared->reference_phase = reference_phase;
  shared->next = 0;
  pthread_t thread_ids[EGQ_MAX_THREADS];
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    pthread_create(&thread_ids[thread_idx], NULL, egq_worker_run, shared);
  }
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    pthread_join(thread_ids[thread_idx], NULL);
  }
}

// Takes reference values from earlier Q lines (same refplies) for positions
// whose chosen move they already valued.
static int egq_load_known(const char *path, EgqPosition *positions,
                          int num_positions) {
  FILE *in = fopen(path, "r");
  if (in == NULL) {
    log_fatal("egpickq: cannot open known file %s", path);
  }
  int reused = 0;
  char line[EGQ_LINE];
  while (fgets(line, sizeof(line), in) != NULL) {
    if (line[0] != 'Q' || line[1] != ' ') {
      continue;
    }
    line[strcspn(line, "\n")] = '\0';
    char id[EGQ_ID];
    int tiles = 0;
    int value = 0;
    int ref_best = 0;
    int ref_actual = 0;
    double seconds = 0.0;
    unsigned long long nodes = 0;
    int consumed = 0;
    if (sscanf(line, "Q %63s %d %d %d %d %lf %llu %n", id, &tiles, &value,
               &ref_best, &ref_actual, &seconds, &nodes, &consumed) != 7) {
      continue;
    }
    const char *move_string = line + consumed;
    for (int position_idx = 0; position_idx < num_positions; position_idx++) {
      EgqPosition *position = &positions[position_idx];
      if (!position->ref_known && position->has_move &&
          strcmp(position->id, id) == 0 &&
          strcmp(position->move_string, move_string) == 0) {
        position->ref_known = true;
        position->ref_best = ref_best;
        position->ref_actual = ref_actual;
        reused++;
      }
    }
  }
  (void)fclose(in);
  return reused;
}

static int egq_spec_int(const char *spec, const char *key, int fallback) {
  char pattern[EGQ_ID];
  (void)snprintf(pattern, sizeof(pattern), "%s=", key);
  const char *found = strstr(spec, pattern);
  if (found == NULL) {
    return fallback;
  }
  return atoi(found + strlen(pattern));
}

void eg_pick_bench_run_spec(const char *spec) {
  const char *file_key = strstr(spec, "file=");
  if (file_key == NULL) {
    log_fatal("egpickq: spec needs file=");
  }
  char path[EGQ_LINE];
  (void)snprintf(path, sizeof(path), "%s", file_key + strlen("file="));
  path[strcspn(path, ",")] = '\0';
  const int plies = egq_spec_int(spec, "plies", 2);
  const int ref_plies = egq_spec_int(spec, "refplies", 6);
  int threads = egq_spec_int(spec, "threads", 1);
  threads = threads < 1 ? 1 : (threads > EGQ_MAX_THREADS ? EGQ_MAX_THREADS
                                                         : threads);
  const int first = egq_spec_int(spec, "first", 0);
  const int max_positions = egq_spec_int(spec, "max", 1000000);

  FILE *in = fopen(path, "r");
  if (in == NULL) {
    log_fatal("egpickq: cannot open %s", path);
  }
  int capacity = 1024;
  EgqPosition *positions = malloc_or_die(sizeof(EgqPosition) * capacity);
  int num_positions = 0;
  int line_idx = 0;
  char line[EGQ_LINE];
  while (num_positions < max_positions && fgets(line, sizeof(line), in)) {
    line[strcspn(line, "\n")] = '\0';
    char *space = strchr(line, ' ');
    if (space == NULL) {
      continue;
    }
    if (line_idx++ < first) {
      continue;
    }
    *space = '\0';
    if (num_positions == capacity) {
      capacity *= 2;
      positions = realloc_or_die(positions, sizeof(EgqPosition) * capacity);
    }
    EgqPosition *position = &positions[num_positions++];
    memset(position, 0, sizeof(*position));
    (void)snprintf(position->id, sizeof(position->id), "%s", line);
    position->cgp = string_duplicate(space + 1);
  }
  (void)fclose(in);

  const bool csw21 = strstr(spec, "lex=CSW21") != NULL;
  Config *config = config_create_or_die(
      csw21 ? "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
              "-numplays 1 -threads 1 -savesettings false -poolshift 0 "
              "-pegadjust none -pat none"
            : "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
              "-numplays 1 -threads 1 -savesettings false -poolshift 0 "
              "-pegadjust none -pat none");
  const int time_limit_ms = egq_spec_int(spec, "tlim", 0);
  if (time_limit_ms > 0 && num_positions > 0) {
    EgtShared timed = {.config = config,
                       .positions = positions,
                       .num_positions = num_positions,
                       .time_limit_seconds = time_limit_ms / 1000.0,
                       .judge_limit_seconds =
                           egq_spec_int(spec, "jtlim", 10000) / 1000.0};
    pthread_mutex_init(&timed.mutex, NULL);
    pthread_mutex_init(&timed.create_mutex, NULL);
    printf("TCFG file=%s tlim_ms=%d jtlim_ms=%d threads=%d positions=%d "
           "egpick_n=%s mode=%s\n",
           path, time_limit_ms, egq_spec_int(spec, "jtlim", 10000), threads,
           num_positions,
           getenv("MAGPIE_EGPICK_N") ? getenv("MAGPIE_EGPICK_N") : "0",
           getenv("MAGPIE_EGPICK_MODE") ? getenv("MAGPIE_EGPICK_MODE")
                                        : "full");
    (void)fflush(stdout);
    pthread_t timed_ids[EGQ_MAX_THREADS];
    for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
      pthread_create(&timed_ids[thread_idx], NULL, egt_worker_run, &timed);
    }
    for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
      pthread_join(timed_ids[thread_idx], NULL);
    }
    printf("TDONE\n");
    (void)fflush(stdout);
    for (int position_idx = 0; position_idx < num_positions; position_idx++) {
      free(positions[position_idx].cgp);
    }
    free(positions);
    config_destroy(config);
    return;
  }
  EgqShared shared = {.config = config,
                      .positions = positions,
                      .num_positions = num_positions,
                      .plies = plies,
                      .ref_plies = ref_plies};
  pthread_mutex_init(&shared.mutex, NULL);
  printf("QCFG file=%s plies=%d refplies=%d threads=%d positions=%d "
         "egpick_n=%s mode=%s\n",
         path, plies, ref_plies, threads, num_positions,
         getenv("MAGPIE_EGPICK_N") ? getenv("MAGPIE_EGPICK_N") : "0",
         getenv("MAGPIE_EGPICK_MODE") ? getenv("MAGPIE_EGPICK_MODE") : "full");
  (void)fflush(stdout);
  eg_pick_stats_reset();
  const int64_t start_ns = ctimer_monotonic_ns();
  egq_run_phase(&shared, threads, false);
  const double choose_wall = (double)(ctimer_monotonic_ns() - start_ns) / 1e9;
  EgPickStats stats;
  eg_pick_stats_get(&stats);
  const char *known_key = strstr(spec, "known=");
  if (known_key != NULL) {
    char known_path[EGQ_LINE];
    (void)snprintf(known_path, sizeof(known_path), "%s",
                   known_key + strlen("known="));
    known_path[strcspn(known_path, ",")] = '\0';
    printf("QKNOWN reused=%d\n",
           egq_load_known(known_path, positions, num_positions));
  }
  eg_pick_set_suppressed(true);
  egq_run_phase(&shared, threads, true);
  eg_pick_set_suppressed(false);
  for (int position_idx = 0; position_idx < num_positions; position_idx++) {
    const EgqPosition *position = &positions[position_idx];
    printf("Q %s %d %d %d %d %.6f %llu %s\n", position->id, position->tiles,
           position->value, position->ref_best, position->ref_actual,
           position->seconds, (unsigned long long)position->nodes,
           position->has_move ? position->move_string : "-");
  }
  printf("QSTATS picks=%lld candidates=%lld reply_movegens=%lld ns=%lld "
         "choose_wall=%.3f\n",
         (long long)stats.picks, (long long)stats.candidates,
         (long long)stats.reply_movegens, (long long)stats.nanoseconds,
         choose_wall);
  (void)fflush(stdout);
  for (int position_idx = 0; position_idx < num_positions; position_idx++) {
    free(positions[position_idx].cgp);
  }
  free(positions);
  config_destroy(config);
}
