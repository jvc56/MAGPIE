// Recursive rollouts (Phase 2 of docs/lamc-plan.md): the CI test, and the
// continuation regret harness that compares sim settings on late positions.
//
// Continuation regret (on demand, rlamccr). For each of a set of positions,
// two sim settings, A and B, each choose a play. Where they choose the same
// play, the position adds nothing and costs nothing more. Where they differ,
// both plays are played out to the end of the game many times, on common
// random numbers: continuation c draws the opponent's rack and orders the bag
// from one seed, for both plays, and from then on a fixed continuation engine
// plays both sides, a sim while the bag has tiles and the endgame solver once
// it is empty. A's regret against B in the position is the mean result of B's
// play less A's, in wins (a tie is half a win) and in spread. Positions are
// the independent units of the summary.
//
// The continuation engine stands in for good play after the choice: neither
// A nor B, so neither is favored by its own model of the future.
//
// Settings come from the environment:
//   RCR_POSITIONS   positions (default 200)
//   RCR_FIRST_GAME  games to skip, for a fresh set of positions (default 0)
//   RCR_MIN_BAG     fewest tiles in the bag of a position (default 1)
//   RCR_MAX_BAG     most tiles in the bag of a position (default 20)
//   RCR_LEX         lexicon (default CSW21)
//   RCR_BASE        settings both A and B take
//   RCR_A, RCR_B    each one's settings, after RCR_BASE
//   RCR_CONT        the continuation engine's sim settings
//   RCR_CONTS       continuations per disagreement (default 32)
//   RCR_EG_PLIES    the continuation's endgame depth (default 4)
//   RCR_THREADS     positions solved at once, one thread each (default 10)
//   RCR_DUMP        1: print each position's CGP, the opponent's rack unseen
//   RCR_ONLY        only these positions, by index, comma-separated
//   RCR_SHOW        the number of the sim's top plays to print per choice
//   RCR_TABLES      the lexical tables every config loads (default
//                   RCR_DEFAULT_TABLES: the word info and rack info tables,
//                   the RIT mapped so processes share it; every config
//                   loads the wordmap besides); a
//                   lexicon without a WIT or RIT, such as CSW21, needs
//                   "-rit false -wit false"
//   RCR_CHOOSE      A or B: only that method chooses, nothing is played
//                   out, and the summary has a hash of its choices, for
//                   timing and for checking that a speedup is exact
//   RPEGM_FROM      rlamcpegm: the first position to choose and solve, to
//                   resume a run (default 0); positions keep their indices

#include "rlamc_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/cpthread_defs.h"
#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/autoplay_solver_settings.h"
#include "../src/ent/bag.h"
#include "../src/ent/cross_set_cache.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/rlamc_args.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/transposition_table.h"
#include "../src/impl/autoplay_solvers.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/impl/random_variable.h"
#include "../src/impl/rlamc.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/str/sim_string.h"
#include "../src/util/fnv.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  RCR_MAX_THREADS = 64,
  RCR_MOVE_STRING_SIZE = 64,
  RCR_NUM_METHODS = 3,
  RCR_METHOD_A = 0,
  RCR_METHOD_B = 1,
  RCR_METHOD_CONT = 2,
  RCR_MAX_TURNS = 200,
};

// The share of memory the continuations' endgame table takes.
static const double RCR_TT_FRACTION = 0.04;

static uint64_t rcr_mix(uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

static int rcr_env_int(const char *name, const int fallback) {
  const char *value = getenv(name);
  return value ? (int)strtol(value, NULL, 10) : fallback;
}

static const char *rcr_env_string(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value ? value : fallback;
}

#define RCR_DEFAULT_TABLES "-wit true -rit true -ritmmap true"

static const char *rcr_tables(void) {
  return rcr_env_string("RCR_TABLES", RCR_DEFAULT_TABLES);
}

typedef struct NcrPosition {
  Game *game;
  int bag;
  Move chosen[2];
  char chosen_string[2][RCR_MOVE_STRING_SIZE];
  double seconds[2];
  bool differ;
  // B's result less A's, over the continuations.
  double win_diff;
  double win_diff_se;
  double spread_diff;
  double spread_diff_se;
} NcrPosition;

typedef struct NcrShared {
  NcrPosition *positions;
  int num_positions;
  int num_threads;
  int num_conts;
  int eg_plies;
  SimArgs base_args[RCR_NUM_METHODS];
  int num_plays[RCR_NUM_METHODS];
  TranspositionTable *tt;
  // RCR_ONLY with a comma before and after, or NULL for every position.
  char *only;
  // RCR_SHOW: the sim's top plays to print after each choice (0: none).
  int show;
  // RCR_CHOOSE: the one method that chooses, or -1 for both.
  int choose_only;
  atomic_int next_position;
  atomic_int done;
} NcrShared;

typedef struct NcrWorker {
  NcrShared *shared;
  int worker_idx;
  Game *game;
  MoveList *lists[RCR_NUM_METHODS];
  SimCtx *ctxs[RCR_NUM_METHODS];
  SimResults *results[RCR_NUM_METHODS];
  AutoplaySolverCtx *solver;
  AutoplaySolverSettings solver_settings;
  ErrorStack *error_stack;
} NcrWorker;

// The play method chooses in the worker's game, copied into out.
static void rcr_choose(NcrWorker *worker, const int method, const uint64_t seed,
                       Move *out) {
  NcrShared *shared = worker->shared;
  SimArgs sim_args = shared->base_args[method];
  sim_args.game = worker->game;
  sim_args.move_list = worker->lists[method];
  sim_args.num_threads = 1;
  sim_args.bai_options.num_threads = 1;
  sim_args.bai_options.parent_worker_thread_index = worker->worker_idx;
  sim_args.seed = seed;
  bool simulated = false;
  const Move *move = get_top_simming_move(
      worker->game, worker->lists[method], &sim_args, &worker->ctxs[method],
      worker->results[method], &simulated, worker->error_stack);
  if (!error_stack_is_empty(worker->error_stack)) {
    error_stack_print_and_reset(worker->error_stack);
    log_fatal("RLAMC continuation regret: a sim failed");
  }
  move_copy(out, move);
}

// Plays the worker's game to its end with the continuation engine. Returns
// the result for player mover: 1 for a win, 0.5 for a tie, 0 for a loss; and
// the final spread for mover in *spread.
static double rcr_play_out(NcrWorker *worker, const int mover,
                           const uint64_t seed, double *spread) {
  Game *game = worker->game;
  Move move;
  for (int turn = 0; turn < RCR_MAX_TURNS && !game_over(game); turn++) {
    if (bag_get_letters(game_get_bag(game)) == 0 &&
        worker->shared->eg_plies > 0) {
      const Move *solved = autoplay_solver_solve(
          worker->solver, &worker->solver_settings, game, worker->shared->tt, 1,
          rcr_mix(seed + 7919ULL * (uint64_t)turn) | 1ULL, NULL, false,
          worker->error_stack);
      if (solved == NULL || !error_stack_is_empty(worker->error_stack)) {
        error_stack_print_and_reset(worker->error_stack);
        log_fatal("RLAMC continuation regret: an endgame solve failed");
      } else {
        move_copy(&move, solved);
      }
    } else {
      rcr_choose(worker, RCR_METHOD_CONT,
                 rcr_mix(seed + 104729ULL * (uint64_t)turn), &move);
    }
    play_move(&move, game, NULL);
  }
  const Equity final_spread =
      player_get_score(game_get_player(game, mover)) -
      player_get_score(game_get_player(game, 1 - mover));
  *spread = equity_to_double(final_spread);
  if (final_spread > 0) {
    return 1.0;
  }
  return final_spread == 0 ? 0.5 : 0.0;
}

static void rcr_evaluate(NcrWorker *worker, NcrPosition *position,
                         const uint64_t pos_seed) {
  const NcrShared *shared = worker->shared;
  const int mover = game_get_player_on_turn_index(position->game);
  const int other = 1 - mover;
  double win_sum = 0.0;
  double win_sum_sq = 0.0;
  double spread_sum = 0.0;
  double spread_sum_sq = 0.0;
  for (int cont_idx = 0; cont_idx < shared->num_conts; cont_idx++) {
    const uint64_t cont_seed = rcr_mix(pos_seed ^ (0x51ED27ULL + cont_idx));
    double wins[2];
    double spreads[2];
    for (int branch = 0; branch < 2; branch++) {
      game_copy(worker->game, position->game);
      // The opponent's rack, as the mover sees it: drawn from the unseen
      // tiles.
      return_rack_to_bag(worker->game, other);
      game_seed(worker->game, cont_seed);
      draw_to_full_rack(worker->game, other);
      play_move(&position->chosen[branch], worker->game, NULL);
      wins[branch] = rcr_play_out(worker, mover, cont_seed, &spreads[branch]);
    }
    const double win_diff = wins[1] - wins[0];
    const double spread_diff = spreads[1] - spreads[0];
    win_sum += win_diff;
    win_sum_sq += win_diff * win_diff;
    spread_sum += spread_diff;
    spread_sum_sq += spread_diff * spread_diff;
  }
  const double count = (double)shared->num_conts;
  position->win_diff = win_sum / count;
  position->spread_diff = spread_sum / count;
  const double win_var =
      (win_sum_sq - win_sum * position->win_diff) / (count - 1);
  const double spread_var =
      (spread_sum_sq - spread_sum * position->spread_diff) / (count - 1);
  position->win_diff_se = sqrt(fmax(win_var, 0.0) / count);
  position->spread_diff_se = sqrt(fmax(spread_var, 0.0) / count);
}

static void *rcr_worker_run(void *data) {
  NcrWorker *worker = (NcrWorker *)data;
  NcrShared *shared = worker->shared;
  while (true) {
    const int pos_idx = atomic_fetch_add(&shared->next_position, 1);
    if (pos_idx >= shared->num_positions) {
      break;
    }
    NcrPosition *position = &shared->positions[pos_idx];
    if (shared->only != NULL) {
      char *delimited = get_formatted_string(",%d,", pos_idx);
      const bool listed = has_substring(shared->only, delimited);
      free(delimited);
      if (!listed) {
        continue;
      }
    }
    const uint64_t pos_seed = rcr_mix(1000003ULL * (uint64_t)(pos_idx + 1));
    for (int method = 0; method < 2; method++) {
      if (shared->choose_only >= 0 && method != shared->choose_only) {
        continue;
      }
      game_copy(worker->game, position->game);
      const double start = ((double)ctimer_monotonic_ns() / 1e9);
      rcr_choose(worker, method, pos_seed, &position->chosen[method]);
      position->seconds[method] = ((double)ctimer_monotonic_ns() / 1e9) - start;
      if (shared->show > 0) {
        char *table = sim_results_get_string(
            worker->game, worker->results[method], shared->show, 1, -1, -1,
            NULL, 0, false, false, false, NULL);
        printf("RCR show pos %d method %c\n%s", pos_idx, 'A' + method, table);
        free(table);
      }
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(position->game),
                              &position->chosen[method],
                              game_get_ld(position->game), false);
      (void)snprintf(position->chosen_string[method], RCR_MOVE_STRING_SIZE,
                     "%s", string_builder_peek(sb));
      string_builder_destroy(sb);
    }
    if (shared->choose_only >= 0) {
      const int method = shared->choose_only;
      printf("RCR pos %d bag %d %c %s (%.2fs)\n", pos_idx, position->bag,
             'A' + method, position->chosen_string[method],
             position->seconds[method]);
      (void)fflush(stdout);
      continue;
    }
    position->differ =
        compare_moves_without_equity(&position->chosen[0], &position->chosen[1],
                                     true) != -1;
    if (position->differ) {
      rcr_evaluate(worker, position, pos_seed);
    } else {
      position->win_diff = 0.0;
      position->win_diff_se = 0.0;
      position->spread_diff = 0.0;
      position->spread_diff_se = 0.0;
    }
    printf("RCR pos %d bag %d A %s (%.2fs) B %s (%.2fs) win %+.4f (%.4f) "
           "spread %+.2f (%.2f)\n",
           pos_idx, position->bag, position->chosen_string[0],
           position->seconds[0], position->chosen_string[1],
           position->seconds[1], position->win_diff, position->win_diff_se,
           position->spread_diff, position->spread_diff_se);
    (void)fflush(stdout);
  }
  return NULL;
}

// Positions from games of top static equity plays, each stopped once the bag
// holds at most a target that varies between min_bag and max_bag.
static int rcr_make_positions(const Game *base, NcrPosition *positions,
                              const int num_positions, const int min_bag,
                              const int max_bag, const uint64_t first_game) {
  MoveList *move_list = move_list_create(1);
  int made = 0;
  for (uint64_t game_idx = first_game + 1; made < num_positions; game_idx++) {
    Game *game = game_duplicate(base);
    game_reset(game);
    game_seed(game, 7919ULL * game_idx + 17);
    draw_starting_racks(game);
    const int bag_target =
        min_bag + (int)(rcr_mix(game_idx) % (uint64_t)(max_bag - min_bag + 1));
    bool usable = true;
    while (usable && bag_get_letters(game_get_bag(game)) > bag_target) {
      play_move(get_top_equity_move(game, move_list), game, NULL);
      usable = !game_over(game);
    }
    const int bag = bag_get_letters(game_get_bag(game));
    if (!usable || bag < min_bag) {
      game_destroy(game);
      continue;
    }
    positions[made].game = game;
    positions[made].bag = bag;
    made++;
  }
  move_list_destroy(move_list);
  return made;
}

static Config *rcr_config(const char *lexicon, const char *settings) {
  char *full = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-threads 1 -savesettings false -hr false -sinfer false %s %s",
      lexicon, rcr_tables(), settings);
  Config *config = config_create_or_die(full);
  free(full);
  // Without options: EMPTY_CGP names CSW21, which would replace the lexicon.
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return config;
}

static void rcr_mean_se(const double *values, const int count, double *mean,
                        double *se) {
  double sum = 0.0;
  double sum_sq = 0.0;
  for (int idx = 0; idx < count; idx++) {
    sum += values[idx];
    sum_sq += values[idx] * values[idx];
  }
  *mean = count > 0 ? sum / count : 0.0;
  const double var = count > 1 ? (sum_sq - sum * (*mean)) / (count - 1) : 0.0;
  *se = count > 0 ? sqrt(fmax(var, 0.0) / count) : 0.0;
}

void test_rlamc_cr(void) {
  log_set_level(LOG_FATAL);
  const int num_positions = rcr_env_int("RCR_POSITIONS", 200);
  const int min_bag = rcr_env_int("RCR_MIN_BAG", 1);
  const int max_bag = rcr_env_int("RCR_MAX_BAG", 20);
  int num_threads = rcr_env_int("RCR_THREADS", 10);
  if (num_threads > RCR_MAX_THREADS) {
    num_threads = RCR_MAX_THREADS;
  }
  const char *lexicon = rcr_env_string("RCR_LEX", "CSW21");
  const char *base = rcr_env_string(
      "RCR_BASE",
      "-plies 2 -numplays 10 -scond none -tlim 0 -minplayiterations 10");
  const char *method_settings[RCR_NUM_METHODS] = {
      rcr_env_string("RCR_A", "-it 1000"),
      rcr_env_string("RCR_B", "-it 1000 -rlamc plies=0,lamc=0,k=6,m=8,h=1"),
      rcr_env_string("RCR_CONT",
                     "-plies 2 -numplays 8 -it 300 -scond none "
                     "-tlim 0 -minplayiterations 10 -sbagcycle false"),
  };

  NcrShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.num_threads = num_threads;
  shared.num_conts = rcr_env_int("RCR_CONTS", 32);
  shared.eg_plies = rcr_env_int("RCR_EG_PLIES", 4);
  Config *configs[RCR_NUM_METHODS];
  for (int method = 0; method < RCR_NUM_METHODS; method++) {
    char *settings =
        method == RCR_METHOD_CONT
            ? string_duplicate(method_settings[method])
            : get_formatted_string("%s %s", base, method_settings[method]);
    configs[method] = rcr_config(lexicon, settings);
    free(settings);
    thread_control_set_status(config_get_thread_control(configs[method]),
                              THREAD_CONTROL_STATUS_STARTED);
    config_fill_sim_args(configs[method], NULL, NULL, NULL, NULL,
                         &shared.base_args[method]);
    shared.num_plays[method] = shared.base_args[method].num_plays;
  }
  shared.tt = transposition_table_create(RCR_TT_FRACTION);
  shared.positions = calloc_or_die(num_positions, sizeof(NcrPosition));
  shared.num_positions = rcr_make_positions(
      config_get_game(configs[RCR_METHOD_A]), shared.positions, num_positions,
      min_bag, max_bag, (uint64_t)rcr_env_int("RCR_FIRST_GAME", 0));
  const char *only = rcr_env_string("RCR_ONLY", "");
  shared.only = only[0] != '\0' ? get_formatted_string(",%s,", only) : NULL;
  shared.show = rcr_env_int("RCR_SHOW", 0);
  const char *choose = rcr_env_string("RCR_CHOOSE", "");
  shared.choose_only = -1;
  if (choose[0] == 'A' || choose[0] == 'B') {
    shared.choose_only = choose[0] - 'A';
  }
  if (rcr_env_int("RCR_DUMP", 0) != 0) {
    for (int pos_idx = 0; pos_idx < shared.num_positions; pos_idx++) {
      Game *copy = game_duplicate(shared.positions[pos_idx].game);
      return_rack_to_bag(copy, 1 - game_get_player_on_turn_index(copy));
      char *cgp = game_get_cgp(copy, true);
      printf("RCR cgp %d bag %d %s\n", pos_idx, shared.positions[pos_idx].bag,
             cgp);
      free(cgp);
      game_destroy(copy);
    }
  }
  atomic_init(&shared.next_position, 0);
  atomic_init(&shared.done, 0);
  printf("RCR: %d positions, bag %d to %d, %d continuations, %d threads\n"
         "RCR base: %s\nRCR A: %s\nRCR B: %s\nRCR cont: %s\n",
         shared.num_positions, min_bag, max_bag, shared.num_conts, num_threads,
         base, method_settings[0], method_settings[1], method_settings[2]);
  (void)fflush(stdout);
  rlamc_totals_reset();

  NcrWorker *workers = calloc_or_die(num_threads, sizeof(NcrWorker));
  cpthread_t *threads = malloc_or_die(sizeof(cpthread_t) * num_threads);
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    NcrWorker *worker = &workers[worker_idx];
    worker->shared = &shared;
    worker->worker_idx = worker_idx;
    worker->game = game_duplicate(shared.positions[0].game);
    for (int method = 0; method < RCR_NUM_METHODS; method++) {
      worker->lists[method] = move_list_create(shared.num_plays[method]);
      worker->ctxs[method] = NULL;
      worker->results[method] = sim_results_create(0.0);
    }
    worker->solver = autoplay_solver_ctx_create();
    autoplay_solver_settings_set_defaults(&worker->solver_settings);
    worker->solver_settings.endgame_plies = shared.eg_plies;
    worker->solver_settings.peg_max_bag = 0;
    worker->error_stack = error_stack_create();
    cpthread_create(&threads[worker_idx], rcr_worker_run, worker);
  }
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    cpthread_join(threads[worker_idx]);
  }

  if (shared.choose_only >= 0) {
    // A hash of the choices, by position, so that two builds' choices
    // compare at a glance.
    uint64_t hash = 0;
    double total_seconds = 0.0;
    for (int pos_idx = 0; pos_idx < shared.num_positions; pos_idx++) {
      const NcrPosition *position = &shared.positions[pos_idx];
      const char *chosen = position->chosen_string[shared.choose_only];
      uint64_t value = (uint64_t)pos_idx;
      for (int char_idx = 0; chosen[char_idx] != '\0'; char_idx++) {
        value = rcr_mix(value ^ (uint64_t)(unsigned char)chosen[char_idx]);
      }
      hash = rcr_mix(hash ^ value);
      total_seconds += position->seconds[shared.choose_only];
    }
    const RlamcTotals choose_totals = rlamc_get_totals();
    printf("RCR choose summary: %d positions, hash %016llx, %.3f s per "
           "choice, RLAMC searches %llu, samples %llu, decision hits %llu, "
           "stopped %llu, finishes %llu, finish hits %llu, static rollouts "
           "%llu, rollout hits %llu\n",
           shared.num_positions, (unsigned long long)hash,
           total_seconds / shared.num_positions,
           (unsigned long long)choose_totals.searches,
           (unsigned long long)choose_totals.samples,
           (unsigned long long)choose_totals.decision_hits,
           (unsigned long long)choose_totals.stopped,
           (unsigned long long)choose_totals.finishes,
           (unsigned long long)choose_totals.finish_hits,
           (unsigned long long)choose_totals.static_rollouts,
           (unsigned long long)choose_totals.rollout_hits);
    (void)fflush(stdout);
  }

  // B's regret against A is A's mean result less B's: positive favors A.
  double *win_diffs = malloc_or_die(sizeof(double) * shared.num_positions);
  double *spread_diffs = malloc_or_die(sizeof(double) * shared.num_positions);
  double seconds[2] = {0.0, 0.0};
  int differ = 0;
  for (int pos_idx = 0; pos_idx < shared.num_positions; pos_idx++) {
    const NcrPosition *position = &shared.positions[pos_idx];
    win_diffs[pos_idx] = position->win_diff;
    spread_diffs[pos_idx] = position->spread_diff;
    differ += position->differ ? 1 : 0;
    seconds[0] += position->seconds[0];
    seconds[1] += position->seconds[1];
  }
  double win_mean;
  double win_se;
  double spread_mean;
  double spread_se;
  rcr_mean_se(win_diffs, shared.num_positions, &win_mean, &win_se);
  rcr_mean_se(spread_diffs, shared.num_positions, &spread_mean, &spread_se);
  const RlamcTotals totals = rlamc_get_totals();
  printf(
      "RCR summary: %d positions, %d differ (%.1f%%)\n"
      "RCR B - A per position: win %+.5f (SE %.5f), spread %+.3f (SE "
      "%.3f)\n"
      "RCR per differing position: win %+.4f, spread %+.2f\n"
      "RCR seconds per choice: A %.3f, B %.3f\n"
      "RCR RLAMC searches %llu, samples %llu, candidates %.2f each, "
      "switched %.1f%%\n",
      shared.num_positions, differ,
      100.0 * differ / (double)shared.num_positions, win_mean, win_se,
      spread_mean, spread_se,
      differ > 0 ? win_mean * shared.num_positions / differ : 0.0,
      differ > 0 ? spread_mean * shared.num_positions / differ : 0.0,
      seconds[0] / shared.num_positions, seconds[1] / shared.num_positions,
      (unsigned long long)totals.searches, (unsigned long long)totals.samples,
      totals.searches > 0 ? (double)totals.candidates / (double)totals.searches
                          : 0.0,
      totals.searches > 0
          ? 100.0 * (double)totals.switched / (double)totals.searches
          : 0.0);
  (void)fflush(stdout);

  free(win_diffs);
  free(spread_diffs);
  free(shared.only);
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    NcrWorker *worker = &workers[worker_idx];
    game_destroy(worker->game);
    for (int method = 0; method < RCR_NUM_METHODS; method++) {
      move_list_destroy(worker->lists[method]);
      sim_ctx_destroy(worker->ctxs[method]);
      sim_results_destroy(worker->results[method]);
    }
    autoplay_solver_ctx_destroy(worker->solver);
    error_stack_destroy(worker->error_stack);
  }
  free(workers);
  free(threads);
  for (int pos_idx = 0; pos_idx < shared.num_positions; pos_idx++) {
    game_destroy(shared.positions[pos_idx].game);
  }
  free(shared.positions);
  transposition_table_destroy(shared.tt);
  for (int method = 0; method < RCR_NUM_METHODS; method++) {
    config_destroy(configs[method]);
  }
}

// A fingerprint of a recursive sim's results.
static uint64_t rlamc_test_sim(const int num_threads, const char *rlamc) {
  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 5 -plies 3 -threads %d -iterations 300 "
      "-minplayiterations 10 -scond none -tlim 0 -seed 42 -sinfer false "
      "-savesettings false -hr false -rlamc %s",
      num_threads, rlamc);
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  load_and_exec_config_or_die(config, "gen");
  rlamc_totals_reset();
  load_and_exec_config_or_die(config, "sim");
  const RlamcTotals totals = rlamc_get_totals();
  const SimResults *sim_results = config_get_sim_results(config);
  uint64_t hash = FNV_64_OFFSET_BASIS;
  const int num_plays = sim_results_get_number_of_plays(sim_results);
  assert(num_plays == 5);
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    const SimmedPlay *simmed_play =
        sim_results_get_simmed_play(sim_results, play_idx);
    const double mean = simmed_play_get_exact_win_pct_mean(simmed_play);
    if (getenv("RLAMC_TEST_DEBUG")) {
      (void)fprintf(stderr, "NTD %d threads play %d mean %.17g n %llu\n",
                    num_threads, play_idx, mean,
                    (unsigned long long)stat_get_num_samples(
                        simmed_play_get_equity_stat(simmed_play)));
    }
    uint64_t bits;
    memcpy(&bits, &mean, sizeof(bits));
    hash = fnv64a_step(hash, bits);
    hash = fnv64a_step(
        hash, stat_get_num_samples(simmed_play_get_equity_stat(simmed_play)));
  }
  if (getenv("RLAMC_TEST_DEBUG")) {
    (void)fprintf(stderr,
                  "NTD %d threads searches %llu samples %llu switched %llu\n",
                  num_threads, (unsigned long long)totals.searches,
                  (unsigned long long)totals.samples,
                  (unsigned long long)totals.switched);
  }
  // The totals are not part of the fingerprint: with several threads, BAI
  // can take samples it then discards, whose searches count too.
  if (strings_equal(rlamc, "off")) {
    assert(totals.searches == 0);
  } else {
    assert(totals.searches > 0);
    assert(totals.samples + totals.cache_misses > 0);
  }
  config_destroy(config);
  return hash;
}

static void test_rlamc_parse(void) {
  RlamcArgs args;
  ErrorStack *error_stack = error_stack_create();
  rlamc_args_parse("plies=0+2,k=4,m=3,h=2,margin=12,maxbag=20,minbag=1,"
                   "alloc=race,z=1.5,switch=0.01",
                   &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(args.enabled);
  assert(args.depth == 1);
  assert(args.levels[0].plies_mask == 5U);
  assert(args.levels[0].num_candidates == 4);
  assert(args.levels[0].num_samples == 3);
  assert(args.levels[0].horizon_plies == 2);
  assert(args.levels[0].margin == 12);
  assert(args.levels[0].max_bag == 20);
  assert(args.levels[0].min_bag == 1);
  assert(args.levels[0].alloc == RLAMC_ALLOC_RACE);
  assert(args.levels[0].race_z == 1.5);
  assert(args.levels[0].switch_margin == 0.01);
  // Levels: each starts from the one before it.
  rlamc_args_parse("plies=0+1,lamc=1,m=6/plies=0,m=2,finalists=5", &args,
                   error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(args.depth == 2);
  assert(args.finalists == 5);
  assert(args.levels[0].plies_mask == 3U);
  assert(args.levels[0].num_samples == 6);
  assert(args.levels[1].plies_mask == 1U);
  assert(args.levels[1].num_samples == 2);
  assert(args.levels[1].lamc_search);
  assert(!args.levels[0].paired);
  assert(args.control_samples == 0);
  rlamc_args_parse("plies=0,lamc=1,cv=8", &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(args.control_samples == 8);
  rlamc_args_parse("plies=0,lamc=1,pair=1", &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(args.levels[0].paired);
  assert(!args.levels[0].dynamic);
  rlamc_args_parse("plies=0,lamc=1,dyn=1,step=3", &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(args.levels[0].dynamic && args.levels[0].step == 3);
  assert(!args.levels[0].bag_cycle);
  rlamc_args_parse("plies=0,lamc=1,cycle=1", &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(args.levels[0].bag_cycle);
  assert(!args.levels[0].skip_finish);
  rlamc_args_parse("plies=0,lamc=1,fin=0", &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(args.levels[0].skip_finish);
  // Defaults: the best setting, but with finishes.
  rlamc_args_parse("on", &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(args.enabled && args.depth == 1);
  assert(args.levels[0].plies_mask == 3U);
  assert(args.levels[0].lamc_search);
  assert(args.levels[0].decision_cache);
  assert(args.levels[0].num_samples == 4);
  assert(args.levels[0].horizon_plies == 5);
  assert(!args.levels[0].skip_finish);
  rlamc_args_parse("off", &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!args.enabled);
  const char *const bad[] = {
      "k=0",     "plies=",  "m=x",         "alloc=fast",          "q=1",
      "k",       "k=2/k=0", "share=1/k=2", "m=1/m=1/m=1/m=1/m=1", "pair=2",
      "cycle=2", "fin=2",   "cv=65"};
  for (int bad_idx = 0; bad_idx < 13; bad_idx++) {
    rlamc_args_parse(bad[bad_idx], &args, error_stack);
    assert(error_stack_top(error_stack) == ERROR_STATUS_RLAMC_BAD_SPEC);
    error_stack_reset(error_stack);
  }
  error_stack_destroy(error_stack);
}

void test_rlamc(void) {
  test_rlamc_parse();
  const char *const specs[] = {
      "off",
      "on",
      "plies=0,lamc=0,k=4,m=3,h=1,dcache=0",
      "plies=0+1,lamc=0,k=3,m=2,h=2,alloc=race,z=1,dcache=0",
      "plies=0+1,lamc=0,k=5,m=4,share=1,dcache=0",
      "plies=0+1,lamc=0,k=4,m=3,dcache=1",
      "plies=0,lamc=1,pool=5,keep=2,rounds=3,m=2,stop=1,dcache=0",
      "plies=0,lamc=0,k=3,m=2,h=1,dcache=0/plies=0,k=2,m=1,dcache=1",
      "plies=0,lamc=1,pool=4,keep=2,rounds=2,m=2,dcache=1/plies=0,lamc=0,k=2",
      "plies=0,lamc=1,pool=5,keep=2,rounds=3,m=2,pair=1,dcache=0",
      "plies=0,lamc=1,pool=5,rounds=3,m=2,dyn=1,step=1,pair=1,dcache=0",
      "plies=0,lamc=1,pool=5,keep=2,rounds=3,m=2,cycle=1,dcache=0",
      "plies=0,lamc=1,pool=5,keep=2,rounds=3,m=2,fin=0,cv=3,dcache=0"};
  for (int spec_idx = 0; spec_idx < 13; spec_idx++) {
    const uint64_t one = rlamc_test_sim(1, specs[spec_idx]);
    const uint64_t several = rlamc_test_sim(4, specs[spec_idx]);
    assert(one == several);
  }
}

// On demand (rlamcbench): one sim of RB_CGP with RB_SETTINGS, timed, with
// the recursive totals.
void test_rlamc_bench(void) {
  const char *cgp = rcr_env_string(
      "RB_CGP", "15/15/10T4/10AD3/10MO3/5ZEK2EW3/6MITT1N3/7DOWLY3/8POI4/"
                "3ALBUGoS5/15/15/15/15/15 CGILLRS/ 98/167 0");
  const char *settings = rcr_env_string(
      "RB_SETTINGS", "-numplays 10 -plies 2 -threads 2 -iterations 3000 "
                     "-minplayiterations 10 -scond none -tlim 0 -seed 42 "
                     "-sbagcycle false -rlamc plies=0,lamc=0,k=6,m=8,share=1");
  char *full = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-sinfer false -savesettings false -hr false %s",
      settings);
  Config *config = config_create_or_die(full);
  free(full);
  char *load = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, load);
  free(load);
  const int bag = rcr_env_int("RB_BAG", 0);
  if (bag > 0) {
    // A position from a game of static plays, with bag tiles in the bag.
    NcrPosition position;
    rcr_make_positions(config_get_game(config), &position, 1, bag, bag,
                       (uint64_t)rcr_env_int("RB_GAME", 0));
    // The opponent's rack is unknown to the sim.
    return_rack_to_bag(position.game,
                       1 - game_get_player_on_turn_index(position.game));
    char *position_cgp = game_get_cgp(position.game, true);
    game_destroy(position.game);
    load = get_formatted_string("cgp %s", position_cgp);
    printf("RB position: %s\n", position_cgp);
    free(position_cgp);
    load_and_exec_config_or_die(config, load);
    free(load);
  }
  load_and_exec_config_or_die(config, "gen");
  rlamc_totals_reset();
  const double start = (double)ctimer_monotonic_ns() / 1e9;
  load_and_exec_config_or_die(config, "sim");
  const double seconds = (double)ctimer_monotonic_ns() / 1e9 - start;
  const RlamcTotals totals = rlamc_get_totals();
  const SimResults *sim_results = config_get_sim_results(config);
  printf(
      "RB %.3f s, %llu rollouts, RLAMC searches %llu, candidates %.2f, "
      "switched %.1f%%, samples %llu, cache hits %llu misses %llu "
      "(%.1f%% hit), decision hits %llu, inner searches %llu, stopped %llu\n",
      seconds, (unsigned long long)sim_results_get_iteration_count(sim_results),
      (unsigned long long)totals.searches,
      totals.searches ? (double)totals.candidates / (double)totals.searches
                      : 0.0,
      totals.searches
          ? 100.0 * (double)totals.switched / (double)totals.searches
          : 0.0,
      (unsigned long long)totals.samples, (unsigned long long)totals.cache_hits,
      (unsigned long long)totals.cache_misses,
      (totals.cache_hits + totals.cache_misses) > 0
          ? 100.0 * (double)totals.cache_hits /
                (double)(totals.cache_hits + totals.cache_misses)
          : 0.0,
      (unsigned long long)totals.decision_hits,
      (unsigned long long)totals.inner_searches,
      (unsigned long long)totals.stopped);
  config_destroy(config);
}

// Ground truth tables (on demand: rlamcgt, rlamceval).
//
// rlamcgt writes a table of positions and, for each, the continuation value
// of each of its top plays by static equity: RGT_CANDS plays, each played out
// RCR_CONTS times by the continuation engine on the same continuation seeds,
// so that the plays' values are compared on common random numbers.
// rlamceval then scores sim settings against the table without playing
// anything out: each settings' play in each position is looked up, and its
// regret is the best value in the position less its own. A play outside the
// table is counted, not scored.
//
// Settings (and NCR_* as above for positions and the continuation engine):
//   RGT_FILE     the table (default rlamc_gt.txt)
//   RGT_CANDS    plays valued per position (default 10)
//   RE_METHODS   |-separated sim settings to score, each after RCR_BASE

enum {
  RGT_MAX_CANDS = 64,
  RGT_MAX_CONTS = 1024,
  RGT_LINE_SIZE = 65536,
};

typedef struct GtPosition {
  Game *game;
  int bag;
  int pos_id;
  int num_cands;
  Move cands[RGT_MAX_CANDS];
  // num_cands x num_conts results for the mover.
  double *wins;
  double *spreads;
} GtPosition;

typedef struct GtShared {
  GtPosition *positions;
  int num_positions;
  int num_cands;
  int num_conts;
  FILE *out;
  cpthread_mutex_t out_mutex;
  atomic_int next_position;
} GtShared;

// The top num_cands plays of game by static equity, best first.
static int gt_candidates(const Game *game, const int num_cands, Move *out) {
  MoveList *move_list = move_list_create(num_cands);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&gen_args);
  move_list_sort_moves(move_list);
  const int count = move_list_get_count(move_list);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    move_copy(&out[move_idx], move_list_get_move(move_list, move_idx));
  }
  move_list_destroy(move_list);
  return count;
}

static uint64_t gt_cont_seed(const int pos_id, const int cont_idx) {
  return rcr_mix(rcr_mix(0x6A09E667ULL + (uint64_t)pos_id) ^
                 (0x51ED27ULL + (uint64_t)cont_idx));
}

// Plays candidate cand_idx of position out num_conts times.
static void gt_value_candidate(NcrWorker *worker, GtPosition *position,
                               const int cand_idx, const int num_conts) {
  const int mover = game_get_player_on_turn_index(position->game);
  const int other = 1 - mover;
  for (int cont_idx = 0; cont_idx < num_conts; cont_idx++) {
    const uint64_t cont_seed = gt_cont_seed(position->pos_id, cont_idx);
    game_copy(worker->game, position->game);
    return_rack_to_bag(worker->game, other);
    game_seed(worker->game, cont_seed);
    draw_to_full_rack(worker->game, other);
    play_move(&position->cands[cand_idx], worker->game, NULL);
    double spread;
    const double win = rcr_play_out(worker, mover, cont_seed, &spread);
    position->wins[cand_idx * num_conts + cont_idx] = win;
    position->spreads[cand_idx * num_conts + cont_idx] = spread;
  }
}

static void gt_write_position(GtShared *shared, const GtPosition *position) {
  // The opponent's rack is not part of the position: the sims and the
  // continuations draw it from the unseen tiles.
  Game *copy = game_duplicate(position->game);
  return_rack_to_bag(copy, 1 - game_get_player_on_turn_index(copy));
  char *cgp = game_get_cgp(copy, true);
  game_destroy(copy);
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb, "GTPOS %d %d %d %d %s\n", position->pos_id, position->bag,
      position->num_cands, shared->num_conts, cgp);
  free(cgp);
  for (int cand_idx = 0; cand_idx < position->num_cands; cand_idx++) {
    string_builder_add_formatted_string(sb, "GTCAND %d %d", position->pos_id,
                                        cand_idx);
    for (int cont_idx = 0; cont_idx < shared->num_conts; cont_idx++) {
      string_builder_add_formatted_string(
          sb, " %g:%g", position->wins[cand_idx * shared->num_conts + cont_idx],
          position->spreads[cand_idx * shared->num_conts + cont_idx]);
    }
    string_builder_add_string(sb, "\n");
  }
  cpthread_mutex_lock(&shared->out_mutex);
  (void)fputs(string_builder_peek(sb), shared->out);
  (void)fflush(shared->out);
  cpthread_mutex_unlock(&shared->out_mutex);
  string_builder_destroy(sb);
}

typedef struct GtWorkerArgs {
  NcrWorker *worker;
  GtShared *gt;
} GtWorkerArgs;

static void *gt_worker_run(void *data) {
  const GtWorkerArgs *args = (const GtWorkerArgs *)data;
  GtShared *gt = args->gt;
  while (true) {
    const int pos_idx = atomic_fetch_add(&gt->next_position, 1);
    if (pos_idx >= gt->num_positions) {
      break;
    }
    GtPosition *position = &gt->positions[pos_idx];
    position->num_cands =
        gt_candidates(position->game, gt->num_cands, position->cands);
    position->wins =
        malloc_or_die(sizeof(double) * position->num_cands * gt->num_conts);
    position->spreads =
        malloc_or_die(sizeof(double) * position->num_cands * gt->num_conts);
    for (int cand_idx = 0; cand_idx < position->num_cands; cand_idx++) {
      gt_value_candidate(args->worker, position, cand_idx, gt->num_conts);
    }
    gt_write_position(gt, position);
    printf("RGT position %d (bag %d) done\n", position->pos_id, position->bag);
    (void)fflush(stdout);
  }
  return NULL;
}

// The workers, each with its own game, lists and sim contexts for settings
// base_args, and endgame solving at eg_plies.
static NcrWorker *rcr_workers_create(NcrShared *shared, const Game *game,
                                     const int num_threads) {
  NcrWorker *workers = calloc_or_die(num_threads, sizeof(NcrWorker));
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    NcrWorker *worker = &workers[worker_idx];
    worker->shared = shared;
    worker->worker_idx = worker_idx;
    worker->game = game_duplicate(game);
    for (int method = 0; method < RCR_NUM_METHODS; method++) {
      worker->lists[method] = move_list_create(shared->num_plays[method]);
      worker->ctxs[method] = NULL;
      worker->results[method] = sim_results_create(0.0);
    }
    worker->solver = autoplay_solver_ctx_create();
    autoplay_solver_settings_set_defaults(&worker->solver_settings);
    worker->solver_settings.endgame_plies = shared->eg_plies;
    worker->solver_settings.peg_max_bag = 0;
    worker->error_stack = error_stack_create();
  }
  return workers;
}

static void rcr_workers_destroy(NcrWorker *workers, const int num_threads) {
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    NcrWorker *worker = &workers[worker_idx];
    game_destroy(worker->game);
    for (int method = 0; method < RCR_NUM_METHODS; method++) {
      move_list_destroy(worker->lists[method]);
      sim_ctx_destroy(worker->ctxs[method]);
      sim_results_destroy(worker->results[method]);
    }
    autoplay_solver_ctx_destroy(worker->solver);
    error_stack_destroy(worker->error_stack);
  }
  free(workers);
}

void test_rlamc_gt(void) {
  log_set_level(LOG_FATAL);
  const int num_positions = rcr_env_int("RCR_POSITIONS", 200);
  const int min_bag = rcr_env_int("RCR_MIN_BAG", 1);
  const int max_bag = rcr_env_int("RCR_MAX_BAG", 20);
  int num_threads = rcr_env_int("RCR_THREADS", 10);
  if (num_threads > RCR_MAX_THREADS) {
    num_threads = RCR_MAX_THREADS;
  }
  const char *lexicon = rcr_env_string("RCR_LEX", "CSW21");
  const char *cont_settings = rcr_env_string(
      "RCR_CONT", "-plies 2 -numplays 8 -it 300 -scond none -tlim 0 "
                  "-minplayiterations 10 -sbagcycle false");
  NcrShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.num_threads = num_threads;
  shared.eg_plies = rcr_env_int("RCR_EG_PLIES", 4);
  Config *config = rcr_config(lexicon, cont_settings);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  for (int method = 0; method < RCR_NUM_METHODS; method++) {
    config_fill_sim_args(config, NULL, NULL, NULL, NULL,
                         &shared.base_args[method]);
    shared.num_plays[method] = shared.base_args[method].num_plays;
  }
  shared.tt = transposition_table_create(RCR_TT_FRACTION);

  GtShared gt;
  memset(&gt, 0, sizeof(gt));
  gt.num_cands = rcr_env_int("RGT_CANDS", 10);
  gt.num_conts = rcr_env_int("RCR_CONTS", 32);
  assert(gt.num_cands <= RGT_MAX_CANDS && gt.num_conts <= RGT_MAX_CONTS);
  NcrPosition *made = calloc_or_die(num_positions, sizeof(NcrPosition));
  const int first_game = rcr_env_int("RCR_FIRST_GAME", 0);
  gt.num_positions =
      rcr_make_positions(config_get_game(config), made, num_positions, min_bag,
                         max_bag, (uint64_t)first_game);
  gt.positions = calloc_or_die(gt.num_positions, sizeof(GtPosition));
  for (int pos_idx = 0; pos_idx < gt.num_positions; pos_idx++) {
    gt.positions[pos_idx].game = made[pos_idx].game;
    gt.positions[pos_idx].bag = made[pos_idx].bag;
    gt.positions[pos_idx].pos_id = first_game * 1000 + pos_idx;
  }
  free(made);
  const char *file = rcr_env_string("RGT_FILE", "rlamc_gt.txt");
  gt.out = fopen_or_die(file, "a");
  cpthread_mutex_init(&gt.out_mutex);
  atomic_init(&gt.next_position, 0);
  printf("RGT: %d positions, bag %d to %d, %d plays, %d continuations, %d "
         "threads, to %s\nRGT cont: %s\n",
         gt.num_positions, min_bag, max_bag, gt.num_cands, gt.num_conts,
         num_threads, file, cont_settings);
  (void)fflush(stdout);
  NcrWorker *workers =
      rcr_workers_create(&shared, gt.positions[0].game, num_threads);
  GtWorkerArgs *worker_args = malloc_or_die(sizeof(GtWorkerArgs) * num_threads);
  cpthread_t *threads = malloc_or_die(sizeof(cpthread_t) * num_threads);
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    worker_args[worker_idx].worker = &workers[worker_idx];
    worker_args[worker_idx].gt = &gt;
    cpthread_create(&threads[worker_idx], gt_worker_run,
                    &worker_args[worker_idx]);
  }
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    cpthread_join(threads[worker_idx]);
  }
  (void)fclose(gt.out);
  rcr_workers_destroy(workers, num_threads);
  free(worker_args);
  free(threads);
  for (int pos_idx = 0; pos_idx < gt.num_positions; pos_idx++) {
    game_destroy(gt.positions[pos_idx].game);
    free(gt.positions[pos_idx].wins);
    free(gt.positions[pos_idx].spreads);
  }
  free(gt.positions);
  transposition_table_destroy(shared.tt);
  config_destroy(config);
}

// The evaluation of sim settings against a table.

typedef struct EvalShared {
  GtPosition *positions;
  int num_positions;
  int num_conts;
  int num_methods;
  // num_methods x num_positions: the index of the play chosen, or -1 when
  // it is outside the table.
  int *chosen;
  double *seconds;
  int method;
  atomic_int next_position;
} EvalShared;

typedef struct EvalWorkerArgs {
  NcrWorker *worker;
  EvalShared *eval;
} EvalWorkerArgs;

static void *eval_worker_run(void *data) {
  const EvalWorkerArgs *args = (const EvalWorkerArgs *)data;
  EvalShared *eval = args->eval;
  NcrWorker *worker = args->worker;
  while (true) {
    const int pos_idx = atomic_fetch_add(&eval->next_position, 1);
    if (pos_idx >= eval->num_positions) {
      break;
    }
    const GtPosition *position = &eval->positions[pos_idx];
    game_copy(worker->game, position->game);
    Move chosen;
    const double start = (double)ctimer_monotonic_ns() / 1e9;
    rcr_choose(worker, RCR_METHOD_A,
               rcr_mix(0x243F6A88ULL + (uint64_t)position->pos_id), &chosen);
    eval->seconds[eval->method * eval->num_positions + pos_idx] =
        (double)ctimer_monotonic_ns() / 1e9 - start;
    int index = -1;
    for (int cand_idx = 0; cand_idx < position->num_cands; cand_idx++) {
      if (compare_moves_without_equity(&chosen, &position->cands[cand_idx],
                                       true) == -1) {
        index = cand_idx;
        break;
      }
    }
    eval->chosen[eval->method * eval->num_positions + pos_idx] = index;
  }
  return NULL;
}

static double gt_mean(const GtPosition *position, const int num_conts,
                      const int cand_idx, const bool spread) {
  const double *values = spread ? position->spreads : position->wins;
  double sum = 0.0;
  for (int cont_idx = 0; cont_idx < num_conts; cont_idx++) {
    sum += values[cand_idx * num_conts + cont_idx];
  }
  return sum / num_conts;
}

// Loads the table in file, whose positions are in the lexicon of config's
// game.
static int gt_load(const char *file, Config *config, GtPosition **out,
                   int *num_conts) {
  FILE *in = fopen_or_die(file, "r");
  char *line = malloc_or_die(RGT_LINE_SIZE);
  int capacity = 64;
  int count = 0;
  GtPosition *positions = calloc_or_die(capacity, sizeof(GtPosition));
  GtPosition *current = NULL;
  while (fgets(line, RGT_LINE_SIZE, in)) {
    if (strncmp(line, "GTPOS ", 6) == 0) {
      int pos_id = 0;
      int bag = 0;
      int cands = 0;
      int conts = 0;
      int consumed = 0;
      // NOLINTNEXTLINE(cert-err34-c)
      if (sscanf(line, "GTPOS %d %d %d %d %n", &pos_id, &bag, &cands, &conts,
                 &consumed) != 4) {
        log_fatal("bad table line: %s", line);
      }
      char *cgp = line + consumed;
      cgp[strcspn(cgp, "\n")] = '\0';
      char *load = get_formatted_string("cgp %s", cgp);
      load_and_exec_config_or_die(config, load);
      free(load);
      if (count == capacity) {
        capacity *= 2;
        positions = realloc_or_die(positions, sizeof(GtPosition) * capacity);
      }
      current = &positions[count++];
      memset(current, 0, sizeof(GtPosition));
      current->game = game_duplicate(config_get_game(config));
      current->bag = bag;
      current->pos_id = pos_id;
      current->num_cands = gt_candidates(current->game, cands, current->cands);
      assert(current->num_cands == cands);
      *num_conts = conts;
      current->wins = malloc_or_die(sizeof(double) * cands * conts);
      current->spreads = malloc_or_die(sizeof(double) * cands * conts);
    } else if (strncmp(line, "GTCAND ", 7) == 0) {
      int pos_id = 0;
      int cand_idx = 0;
      int consumed = 0;
      // NOLINTNEXTLINE(cert-err34-c)
      if (sscanf(line, "GTCAND %d %d%n", &pos_id, &cand_idx, &consumed) != 2 ||
          current == NULL || pos_id != current->pos_id) {
        log_fatal("bad table line: %s", line);
      }
      const char *cursor = line + consumed;
      for (int cont_idx = 0; cont_idx < *num_conts; cont_idx++) {
        double win = 0;
        double spread = 0;
        int used = 0;
        // NOLINTNEXTLINE(cert-err34-c)
        if (sscanf(cursor, " %lf:%lf%n", &win, &spread, &used) != 2) {
          log_fatal("bad table line: %s", line);
        }
        cursor += used;
        current->wins[cand_idx * *num_conts + cont_idx] = win;
        current->spreads[cand_idx * *num_conts + cont_idx] = spread;
      }
    }
  }
  free(line);
  (void)fclose(in);
  *out = positions;
  return count;
}

// The mean of candidate cand_idx's results over the continuations of one
// half: even (half 0) or odd (half 1).
static double gt_half_mean(const GtPosition *position, const int num_conts,
                           const int cand_idx, const int half) {
  double sum = 0.0;
  int count = 0;
  for (int cont_idx = half; cont_idx < num_conts; cont_idx += 2) {
    sum += position->wins[cand_idx * num_conts + cont_idx];
    count++;
  }
  return count > 0 ? sum / count : 0.0;
}

// Regret of cand_idx against an oracle that picks the best play on one half
// of the continuations and is scored on the other, averaged over both
// splits: free of the bias of taking the best of noisy means.
static double gt_oracle_regret(const GtPosition *position, const int num_conts,
                               const int cand_idx) {
  double regret = 0.0;
  for (int half = 0; half < 2; half++) {
    int best = 0;
    for (int other = 1; other < position->num_cands; other++) {
      if (gt_half_mean(position, num_conts, other, half) >
          gt_half_mean(position, num_conts, best, half)) {
        best = other;
      }
    }
    regret += gt_half_mean(position, num_conts, best, 1 - half) -
              gt_half_mean(position, num_conts, cand_idx, 1 - half);
  }
  return regret / 2.0;
}

void test_rlamc_eval(void) {
  log_set_level(LOG_FATAL);
  int num_threads = rcr_env_int("RCR_THREADS", 10);
  if (num_threads > RCR_MAX_THREADS) {
    num_threads = RCR_MAX_THREADS;
  }
  const char *lexicon = rcr_env_string("RCR_LEX", "CSW21");
  const char *base =
      rcr_env_string("RCR_BASE", "-plies 2 -numplays 10 -scond none -tlim 0 "
                                 "-minplayiterations 10");
  StringSplitter *methods = split_string(
      rcr_env_string("RE_METHODS", "-it 1000|-it 1000 -rlamc plies=0"), '|',
      false);
  const int num_methods = string_splitter_get_number_of_items(methods);
  Config *loader = rcr_config(lexicon, "");
  EvalShared eval;
  memset(&eval, 0, sizeof(eval));
  eval.num_positions = gt_load(rcr_env_string("RGT_FILE", "rlamc_gt.txt"),
                               loader, &eval.positions, &eval.num_conts);
  eval.num_methods = num_methods;
  eval.chosen = malloc_or_die(sizeof(int) * num_methods * eval.num_positions);
  eval.seconds =
      malloc_or_die(sizeof(double) * num_methods * eval.num_positions);
  printf("RE: %d positions, %d continuations each, %d threads\nRE base: %s\n",
         eval.num_positions, eval.num_conts, num_threads, base);
  for (int method = 0; method < num_methods; method++) {
    char *settings = get_formatted_string(
        "%s %s", base, string_splitter_get_item(methods, method));
    Config *config = rcr_config(lexicon, settings);
    free(settings);
    thread_control_set_status(config_get_thread_control(config),
                              THREAD_CONTROL_STATUS_STARTED);
    NcrShared shared;
    memset(&shared, 0, sizeof(shared));
    for (int slot = 0; slot < RCR_NUM_METHODS; slot++) {
      config_fill_sim_args(config, NULL, NULL, NULL, NULL,
                           &shared.base_args[slot]);
      shared.num_plays[slot] = shared.base_args[slot].num_plays;
    }
    NcrWorker *workers =
        rcr_workers_create(&shared, eval.positions[0].game, num_threads);
    EvalWorkerArgs *worker_args =
        malloc_or_die(sizeof(EvalWorkerArgs) * num_threads);
    cpthread_t *threads = malloc_or_die(sizeof(cpthread_t) * num_threads);
    eval.method = method;
    atomic_init(&eval.next_position, 0);
    rlamc_totals_reset();
    for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
      worker_args[worker_idx].worker = &workers[worker_idx];
      worker_args[worker_idx].eval = &eval;
      cpthread_create(&threads[worker_idx], eval_worker_run,
                      &worker_args[worker_idx]);
    }
    for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
      cpthread_join(threads[worker_idx]);
    }
    rcr_workers_destroy(workers, num_threads);
    free(worker_args);
    free(threads);
    config_destroy(config);

    // Regret against the table's best, and the difference from method 0 in
    // the positions both chose inside the table.
    double win_regret[2] = {0, 0};
    double spread_regret[2] = {0, 0};
    double diff[2] = {0, 0};
    double oracle_regret[2] = {0, 0};
    double spread_diff[2] = {0, 0};
    double group_regret[4] = {0, 0, 0, 0};
    int group_count[4] = {0, 0, 0, 0};
    double seconds = 0.0;
    int inside = 0;
    int differ = 0;
    for (int pos_idx = 0; pos_idx < eval.num_positions; pos_idx++) {
      const GtPosition *position = &eval.positions[pos_idx];
      seconds += eval.seconds[method * eval.num_positions + pos_idx];
      const int index = eval.chosen[method * eval.num_positions + pos_idx];
      const int base_index = eval.chosen[pos_idx];
      if (index < 0 || base_index < 0) {
        continue;
      }
      inside++;
      double best_win = -1e9;
      double best_spread = -1e9;
      for (int cand_idx = 0; cand_idx < position->num_cands; cand_idx++) {
        best_win =
            fmax(best_win, gt_mean(position, eval.num_conts, cand_idx, false));
        best_spread = fmax(best_spread,
                           gt_mean(position, eval.num_conts, cand_idx, true));
      }
      const double wr =
          best_win - gt_mean(position, eval.num_conts, index, false);
      const double sr =
          best_spread - gt_mean(position, eval.num_conts, index, true);
      win_regret[0] += wr;
      win_regret[1] += wr * wr;
      spread_regret[0] += sr;
      spread_regret[1] += sr * sr;
      const double oracle = gt_oracle_regret(position, eval.num_conts, index);
      oracle_regret[0] += oracle;
      oracle_regret[1] += oracle * oracle;
      int group = 3;
      if (position->bag <= 3) {
        group = 0;
      } else if (position->bag <= 7) {
        group = 1;
      } else if (position->bag <= 12) {
        group = 2;
      }
      group_regret[group] += oracle;
      group_count[group]++;
      const double d = gt_mean(position, eval.num_conts, index, false) -
                       gt_mean(position, eval.num_conts, base_index, false);
      diff[0] += d;
      diff[1] += d * d;
      const double ds = gt_mean(position, eval.num_conts, index, true) -
                        gt_mean(position, eval.num_conts, base_index, true);
      spread_diff[0] += ds;
      spread_diff[1] += ds * ds;
      if (getenv("RE_CHOICES")) {
        printf("RE choice %d %d %d %d\n", method, position->pos_id,
               position->bag, index);
      }
      differ += index != base_index ? 1 : 0;
    }
    const double n = inside > 0 ? inside : 1;
    const double wr_mean = win_regret[0] / n;
    const double sr_mean = spread_regret[0] / n;
    const double d_mean = diff[0] / n;
    const RlamcTotals totals = rlamc_get_totals();
    printf("RE method %d [%s]: regret win %.5f (SE %.5f) spread %.3f (SE "
           "%.3f); vs method 0 win %+.5f (SE %.5f), %d differ; %d of %d "
           "inside; %.3f s each; RLAMC switched %.1f%%\n",
           method, string_splitter_get_item(methods, method), wr_mean,
           sqrt(fmax(win_regret[1] / n - wr_mean * wr_mean, 0) / n), sr_mean,
           sqrt(fmax(spread_regret[1] / n - sr_mean * sr_mean, 0) / n), d_mean,
           sqrt(fmax(diff[1] / n - d_mean * d_mean, 0) / n), differ, inside,
           eval.num_positions, seconds / eval.num_positions,
           totals.searches
               ? 100.0 * (double)totals.switched / (double)totals.searches
               : 0.0);
    const double sd_mean = spread_diff[0] / n;
    printf("RE method %d vs method 0 spread %+.3f (SE %.3f)\n", method, sd_mean,
           sqrt(fmax(spread_diff[1] / n - sd_mean * sd_mean, 0) / n));
    const double or_mean = oracle_regret[0] / n;
    printf(
        "RE method %d oracle regret %.5f (SE %.5f); by bag 1-3 %.4f (%d), "
        "4-7 %.4f (%d), 8-12 %.4f (%d), 13+ %.4f (%d)\n",
        method, or_mean,
        sqrt(fmax(oracle_regret[1] / n - or_mean * or_mean, 0) / n),
        group_count[0] ? group_regret[0] / group_count[0] : 0.0, group_count[0],
        group_count[1] ? group_regret[1] / group_count[1] : 0.0, group_count[1],
        group_count[2] ? group_regret[2] / group_count[2] : 0.0, group_count[2],
        group_count[3] ? group_regret[3] / group_count[3] : 0.0,
        group_count[3]);
    (void)fflush(stdout);
  }
  string_splitter_destroy(methods);
  for (int pos_idx = 0; pos_idx < eval.num_positions; pos_idx++) {
    game_destroy(eval.positions[pos_idx].game);
    free(eval.positions[pos_idx].wins);
    free(eval.positions[pos_idx].spreads);
  }
  free(eval.positions);
  free(eval.chosen);
  free(eval.seconds);
  config_destroy(loader);
}

// On demand (rlamcvar): rollout data for studying what explains a
// rollout's outcome. For each position of the table RGT_FILE and each of its
// plays, RV_SEEDS rollouts on the seeds every play shares, each written as a
// line: the position, the play, the seed's index, the rollout's utility, the
// opponent's rack the seed drew, and the mover's rack after the play's draw.
// Sim settings come from RCR_BASE; the output goes to RV_FILE.

typedef struct VarShared {
  GtPosition *positions;
  int num_positions;
  int num_seeds;
  SimArgs base_args;
  FILE *out;
  cpthread_mutex_t out_mutex;
  atomic_int next_position;
} VarShared;

typedef struct VarWorkerArgs {
  VarShared *var;
  int worker_idx;
} VarWorkerArgs;

static void var_add_rack(StringBuilder *sb, const Game *game,
                         const int player_index) {
  const Rack *rack = player_get_rack(game_get_player(game, player_index));
  if (rack_is_empty(rack)) {
    string_builder_add_string(sb, "-");
  } else {
    string_builder_add_rack(sb, rack, game_get_ld(game), false);
  }
}

static void *var_worker_run(void *data) {
  const VarWorkerArgs *args = (const VarWorkerArgs *)data;
  VarShared *var = args->var;
  SimResults *sim_results = sim_results_create(0.0);
  RandomVariables *rvs = NULL;
  while (true) {
    const int pos_idx = atomic_fetch_add(&var->next_position, 1);
    if (pos_idx >= var->num_positions) {
      break;
    }
    const GtPosition *position = &var->positions[pos_idx];
    MoveList *move_list = move_list_create(position->num_cands);
    const MoveGenArgs gen_args = {
        .game = position->game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&gen_args);
    move_list_sort_moves(move_list);
    SimArgs sim_args = var->base_args;
    sim_args.game = position->game;
    sim_args.move_list = move_list;
    sim_args.num_plays = move_list_get_count(move_list);
    sim_args.num_threads = 1;
    sim_args.bai_options.num_threads = 1;
    sim_args.bai_options.parent_worker_thread_index = args->worker_idx;
    sim_args.seed = rcr_mix(0x13198A2EULL + (uint64_t)position->pos_id);
    const RandomVariablesArgs rvs_args = {
        .type = RANDOM_VARIABLES_SIMMED_PLAYS,
        .sim_args = &sim_args,
        .sim_results = sim_results,
    };
    if (rvs) {
      rvs_reset(rvs, &rvs_args);
    } else {
      rvs = rvs_create(&rvs_args);
    }
    const int mover = game_get_player_on_turn_index(position->game);
    Game *copy = game_duplicate(position->game);
    StringBuilder *sb = string_builder_create();
    for (int seed_idx = 0; seed_idx < var->num_seeds; seed_idx++) {
      const uint64_t seed =
          rcr_mix(rcr_mix((uint64_t)position->pos_id) + (uint64_t)seed_idx);
      for (int cand_idx = 0; cand_idx < move_list_get_count(move_list);
           cand_idx++) {
        const double utility = rvs_sample_with_seed(
            rvs, (uint64_t)cand_idx, seed, args->worker_idx, NULL, NULL);
        // The racks the rollout drew, drawn again the same way.
        game_copy(copy, position->game);
        return_rack_to_bag(copy, 1 - mover);
        game_seed(copy, seed);
        draw_to_full_rack(copy, 1 - mover);
        string_builder_add_formatted_string(sb, "RV %d %d %d %.6f ",
                                            position->pos_id, cand_idx,
                                            seed_idx, utility);
        var_add_rack(sb, copy, 1 - mover);
        string_builder_add_string(sb, " ");
        play_move(move_list_get_move(move_list, cand_idx), copy, NULL);
        var_add_rack(sb, copy, mover);
        string_builder_add_string(sb, "\n");
      }
    }
    cpthread_mutex_lock(&var->out_mutex);
    (void)fputs(string_builder_peek(sb), var->out);
    (void)fflush(var->out);
    cpthread_mutex_unlock(&var->out_mutex);
    string_builder_destroy(sb);
    game_destroy(copy);
    move_list_destroy(move_list);
  }
  rvs_destroy(rvs);
  sim_results_destroy(sim_results);
  return NULL;
}

void test_rlamc_var(void) {
  log_set_level(LOG_FATAL);
  int num_threads = rcr_env_int("RCR_THREADS", 10);
  if (num_threads > RCR_MAX_THREADS) {
    num_threads = RCR_MAX_THREADS;
  }
  const char *lexicon = rcr_env_string("RCR_LEX", "CSW21");
  const char *base =
      rcr_env_string("RCR_BASE", "-plies 2 -numplays 10 -scond none -tlim 0 "
                                 "-minplayiterations 10 -sbagcycle false");
  Config *config = rcr_config(lexicon, base);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  VarShared var;
  memset(&var, 0, sizeof(var));
  int num_conts = 0;
  var.num_positions = gt_load(rcr_env_string("RGT_FILE", "rlamc_gt.txt"),
                              config, &var.positions, &num_conts);
  const int num_loaded = var.num_positions;
  const int limit = rcr_env_int("RV_POSITIONS", var.num_positions);
  if (limit < var.num_positions) {
    var.num_positions = limit;
  }
  var.num_seeds = rcr_env_int("RV_SEEDS", 200);
  config_fill_sim_args(config, NULL, NULL, NULL, NULL, &var.base_args);
  var.out = fopen_or_die(rcr_env_string("RV_FILE", "rlamc_var.txt"), "w");
  cpthread_mutex_init(&var.out_mutex);
  atomic_init(&var.next_position, 0);
  VarWorkerArgs *worker_args =
      malloc_or_die(sizeof(VarWorkerArgs) * num_threads);
  cpthread_t *threads = malloc_or_die(sizeof(cpthread_t) * num_threads);
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    worker_args[worker_idx].var = &var;
    worker_args[worker_idx].worker_idx = worker_idx;
    cpthread_create(&threads[worker_idx], var_worker_run,
                    &worker_args[worker_idx]);
  }
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    cpthread_join(threads[worker_idx]);
  }
  (void)fclose(var.out);
  free(worker_args);
  free(threads);
  for (int pos_idx = 0; pos_idx < num_loaded; pos_idx++) {
    game_destroy(var.positions[pos_idx].game);
    free(var.positions[pos_idx].wins);
    free(var.positions[pos_idx].spreads);
  }
  free(var.positions);
  config_destroy(config);
}

typedef struct RlamcPegChoiceArgs {
  NcrWorker *worker;
} RlamcPegChoiceArgs;

// Each worker: the plays A and B choose in the positions it takes.
static void *rpeg_choose_run(void *data) {
  NcrWorker *worker = ((RlamcPegChoiceArgs *)data)->worker;
  NcrShared *shared = worker->shared;
  while (true) {
    const int pos_idx = atomic_fetch_add(&shared->next_position, 1);
    if (pos_idx >= shared->num_positions) {
      break;
    }
    NcrPosition *position = &shared->positions[pos_idx];
    const uint64_t pos_seed = rcr_mix(1000003ULL * (uint64_t)(pos_idx + 1));
    for (int method = 0; method < 2; method++) {
      game_copy(worker->game, position->game);
      const double start = (double)ctimer_monotonic_ns() / 1e9;
      rcr_choose(worker, method, pos_seed, &position->chosen[method]);
      position->seconds[method] = (double)ctimer_monotonic_ns() / 1e9 - start;
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(position->game),
                              &position->chosen[method],
                              game_get_ld(position->game), false);
      (void)snprintf(position->chosen_string[method], RCR_MOVE_STRING_SIZE,
                     "%s", string_builder_peek(sb));
      string_builder_destroy(sb);
    }
  }
  return NULL;
}

// On demand (rlamcpeg): sim settings A and B against the pre-endgame solver.
// Positions with RCR_MIN_BAG to RCR_MAX_BAG tiles in the bag (default 2) from
// games of static plays; A and B choose plays as in rlamccr, on one thread
// each. Where they differ, PEG solves just the two plays (peg only), from the
// mover's view of the unseen tiles, on RCR_THREADS threads, with
// RPEG_SETTINGS (for example -pegpess false for the rational opponent), and
// the difference of their exact win percentages is B's gain over A.
void test_rlamc_peg(void) {
  log_set_level(LOG_FATAL);
  const int num_positions = rcr_env_int("RCR_POSITIONS", 100);
  const int min_bag = rcr_env_int("RCR_MIN_BAG", 2);
  const int max_bag = rcr_env_int("RCR_MAX_BAG", 2);
  int num_threads = rcr_env_int("RCR_THREADS", 10);
  if (num_threads > RCR_MAX_THREADS) {
    num_threads = RCR_MAX_THREADS;
  }
  const char *lexicon = rcr_env_string("RCR_LEX", "CSW24");
  const char *base =
      rcr_env_string("RCR_BASE", "-plies 2 -numplays 10 -scond none -tlim 0 "
                                 "-minplayiterations 10");
  const char *method_settings[2] = {
      rcr_env_string("RCR_A", "-it 1000"),
      rcr_env_string("RCR_B", "-it 1000 -sfinish true"),
  };
  NcrShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.num_threads = num_threads;
  Config *configs[2];
  for (int method = 0; method < 2; method++) {
    char *settings =
        get_formatted_string("%s %s", base, method_settings[method]);
    configs[method] = rcr_config(lexicon, settings);
    free(settings);
    thread_control_set_status(config_get_thread_control(configs[method]),
                              THREAD_CONTROL_STATUS_STARTED);
    config_fill_sim_args(configs[method], NULL, NULL, NULL, NULL,
                         &shared.base_args[method]);
    shared.num_plays[method] = shared.base_args[method].num_plays;
  }
  shared.base_args[RCR_METHOD_CONT] = shared.base_args[RCR_METHOD_A];
  shared.num_plays[RCR_METHOD_CONT] = shared.num_plays[RCR_METHOD_A];
  shared.positions = calloc_or_die(num_positions, sizeof(NcrPosition));
  shared.num_positions = rcr_make_positions(
      config_get_game(configs[0]), shared.positions, num_positions, min_bag,
      max_bag, (uint64_t)rcr_env_int("RCR_FIRST_GAME", 0));
  printf("RPEG: %d positions, bag %d to %d\nRPEG base: %s\nRPEG A: %s\nRPEG "
         "B: %s\n",
         shared.num_positions, min_bag, max_bag, base, method_settings[0],
         method_settings[1]);
  (void)fflush(stdout);

  // The choices, on one thread each.
  NcrWorker *workers =
      rcr_workers_create(&shared, shared.positions[0].game, num_threads);
  atomic_init(&shared.next_position, 0);
  RlamcPegChoiceArgs *choice_args =
      malloc_or_die(sizeof(RlamcPegChoiceArgs) * num_threads);
  cpthread_t *threads = malloc_or_die(sizeof(cpthread_t) * num_threads);
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    choice_args[worker_idx].worker = &workers[worker_idx];
    cpthread_create(&threads[worker_idx], rpeg_choose_run,
                    &choice_args[worker_idx]);
  }
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    cpthread_join(threads[worker_idx]);
  }
  rcr_workers_destroy(workers, num_threads);
  free(choice_args);
  free(threads);

  // PEG on the positions where they differ.
  char *peg_settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all -threads "
      "%d -savesettings false -hr false %s %s",
      lexicon, num_threads, rcr_tables(), rcr_env_string("RPEG_SETTINGS", ""));
  Config *peg_config = config_create_or_die(peg_settings);
  free(peg_settings);
  load_and_exec_config_or_die(peg_config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);
  double sum = 0.0;
  double sum_sq = 0.0;
  int differ = 0;
  for (int pos_idx = 0; pos_idx < shared.num_positions; pos_idx++) {
    NcrPosition *position = &shared.positions[pos_idx];
    position->differ =
        compare_moves_without_equity(&position->chosen[0], &position->chosen[1],
                                     true) != -1;
    if (!position->differ) {
      continue;
    }
    differ++;
    Game *copy = game_duplicate(position->game);
    return_rack_to_bag(copy, 1 - game_get_player_on_turn_index(copy));
    char *cgp = game_get_cgp(copy, true);
    game_destroy(copy);
    char *load = get_formatted_string("cgp %s", cgp);
    free(cgp);
    load_and_exec_config_or_die(peg_config, load);
    free(load);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "peg only ");
    for (int method = 0; method < 2; method++) {
      if (method > 0) {
        string_builder_add_string(sb, ",");
      }
      // peg only takes plays as position.word.
      StringBuilder *move_sb = string_builder_create();
      string_builder_add_ucgi_move(move_sb, &position->chosen[method],
                                   game_get_board(position->game),
                                   game_get_ld(position->game));
      char *move = string_builder_dump_and_destroy(move_sb, NULL);
      for (char *cursor = move; *cursor != '\0'; cursor++) {
        if (*cursor == ' ') {
          *cursor = '.';
        }
      }
      string_builder_add_string(sb, move);
      free(move);
    }
    const double start = (double)ctimer_monotonic_ns() / 1e9;
    load_and_exec_config_or_die(peg_config, string_builder_peek(sb));
    const double seconds = (double)ctimer_monotonic_ns() / 1e9 - start;
    const PegResult *result = config_get_peg_result(peg_config);
    double wins[2] = {-1.0, -1.0};
    for (int cand_idx = 0; cand_idx < result->n_top_cands; cand_idx++) {
      for (int method = 0; method < 2; method++) {
        if (compare_moves_without_equity(&result->top_cands[cand_idx].move,
                                         &position->chosen[method],
                                         true) == -1) {
          wins[method] = result->top_cands[cand_idx].win_pct;
        }
      }
    }
    if (wins[0] < 0 || wins[1] < 0) {
      log_fatal("peg did not solve both plays: %s", string_builder_peek(sb));
    }
    position->win_diff = wins[1] - wins[0];
    sum += position->win_diff;
    sum_sq += position->win_diff * position->win_diff;
    printf("RPEG pos %d bag %d A %s %.4f B %s %.4f diff %+.4f (%.1fs; %s)\n",
           pos_idx, position->bag, position->chosen_string[0], wins[0],
           position->chosen_string[1], wins[1], position->win_diff, seconds,
           string_builder_peek(sb));
    (void)fflush(stdout);
    string_builder_destroy(sb);
  }
  const double n = shared.num_positions;
  const double mean = sum / n;
  double seconds[2] = {0.0, 0.0};
  for (int pos_idx = 0; pos_idx < shared.num_positions; pos_idx++) {
    seconds[0] += shared.positions[pos_idx].seconds[0];
    seconds[1] += shared.positions[pos_idx].seconds[1];
  }
  printf("RPEG summary: %d positions, %d differ; B - A win per position "
         "%+.5f (SE %.5f); sim seconds per position A %.3f, B %.3f\n",
         shared.num_positions, differ, mean,
         sqrt(fmax(sum_sq / n - mean * mean, 0.0) / n), seconds[0] / n,
         seconds[1] / n);
  for (int pos_idx = 0; pos_idx < shared.num_positions; pos_idx++) {
    game_destroy(shared.positions[pos_idx].game);
  }
  free(shared.positions);
  config_destroy(peg_config);
  for (int method = 0; method < 2; method++) {
    config_destroy(configs[method]);
  }
}

// On demand (rlamcpegm): several sim settings against the pre-endgame
// solver at once, with a cache of its verdicts. RPEG_METHODS holds the
// settings, |-separated, each after RCR_BASE; each is compared with the
// first. In each position, PEG solves together every play some setting chose
// that the cache (RPEG_CACHE, a file it reads and appends to) lacks. A
// verdict is cached by the position, the play and RPEG_SETTINGS.

typedef struct PegmShared {
  NcrPosition *positions;
  int num_positions;
  // RPEGM_FROM: positions before it are neither chosen nor solved.
  int first_position;
  int method;
  // methods x positions.
  Move *chosen;
  double *seconds;
  atomic_int next_position;
} PegmShared;

typedef struct PegmWorkerArgs {
  NcrWorker *worker;
  PegmShared *pegm;
} PegmWorkerArgs;

static void *pegm_choose_run(void *data) {
  const PegmWorkerArgs *args = (const PegmWorkerArgs *)data;
  PegmShared *pegm = args->pegm;
  NcrWorker *worker = args->worker;
  while (true) {
    const int pos_idx = atomic_fetch_add(&pegm->next_position, 1);
    if (pos_idx >= pegm->num_positions) {
      break;
    }
    const NcrPosition *position = &pegm->positions[pos_idx];
    game_copy(worker->game, position->game);
    const double start = (double)ctimer_monotonic_ns() / 1e9;
    rcr_choose(worker, RCR_METHOD_A,
               rcr_mix(1000003ULL * (uint64_t)(pos_idx + 1)),
               &pegm->chosen[pegm->method * pegm->num_positions + pos_idx]);
    pegm->seconds[pegm->method * pegm->num_positions + pos_idx] =
        (double)ctimer_monotonic_ns() / 1e9 - start;
  }
  return NULL;
}

// A play as peg only takes it: position.word.
static char *pegm_move_string(const Game *game, const Move *move) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_ucgi_move(sb, move, game_get_board(game),
                               game_get_ld(game));
  char *string = string_builder_dump_and_destroy(sb, NULL);
  for (char *cursor = string; *cursor != '\0'; cursor++) {
    if (*cursor == ' ') {
      *cursor = '.';
    }
  }
  return string;
}

typedef struct PegmCache {
  uint64_t *keys;
  double *wins;
  int count;
  int capacity;
} PegmCache;

static uint64_t pegm_key(const char *cgp, const char *move,
                         const char *settings) {
  uint64_t hash = FNV_64_OFFSET_BASIS;
  for (const char *parts[3] = {cgp, move, settings}, **part = parts;
       part < parts + 3; part++) {
    for (const char *cursor = *part; *cursor != '\0'; cursor++) {
      hash = fnv64a_step(hash, (uint64_t)(unsigned char)*cursor);
    }
    hash = fnv64a_step(hash, (uint64_t)'|');
  }
  return hash;
}

static bool pegm_cache_find(const PegmCache *cache, const uint64_t key,
                            double *win) {
  for (int idx = 0; idx < cache->count; idx++) {
    if (cache->keys[idx] == key) {
      *win = cache->wins[idx];
      return true;
    }
  }
  return false;
}

static void pegm_cache_add(PegmCache *cache, const uint64_t key,
                           const double win) {
  if (cache->count == cache->capacity) {
    cache->capacity = cache->capacity > 0 ? cache->capacity * 2 : 256;
    cache->keys =
        realloc_or_die(cache->keys, sizeof(uint64_t) * cache->capacity);
    cache->wins = realloc_or_die(cache->wins, sizeof(double) * cache->capacity);
  }
  cache->keys[cache->count] = key;
  cache->wins[cache->count] = win;
  cache->count++;
}

void test_rlamc_pegm(void) {
  log_set_level(LOG_FATAL);
  const int num_positions = rcr_env_int("RCR_POSITIONS", 100);
  const int min_bag = rcr_env_int("RCR_MIN_BAG", 2);
  const int max_bag = rcr_env_int("RCR_MAX_BAG", 2);
  int num_threads = rcr_env_int("RCR_THREADS", 10);
  if (num_threads > RCR_MAX_THREADS) {
    num_threads = RCR_MAX_THREADS;
  }
  const char *lexicon = rcr_env_string("RCR_LEX", "CSW24");
  const char *base =
      rcr_env_string("RCR_BASE", "-plies 5 -numplays 30 -scond none -tlim 0 "
                                 "-minplayiterations 10 -it 3000");
  const char *peg_extra = rcr_env_string("RPEG_SETTINGS", "");
  StringSplitter *methods = split_string(
      rcr_env_string("RPEG_METHODS", "-sfinish true|-sfinish true -rlamc "
                                     "plies=0+1,lamc=0,k=6,m=8,dcache=1"),
      '|', false);
  const int num_methods = string_splitter_get_number_of_items(methods);

  Config *base_config = rcr_config(lexicon, base);
  PegmShared pegm;
  memset(&pegm, 0, sizeof(pegm));
  pegm.positions = calloc_or_die(num_positions, sizeof(NcrPosition));
  pegm.num_positions = rcr_make_positions(
      config_get_game(base_config), pegm.positions, num_positions, min_bag,
      max_bag, (uint64_t)rcr_env_int("RCR_FIRST_GAME", 0));
  pegm.first_position = rcr_env_int("RPEGM_FROM", 0);
  if (pegm.first_position < 0 || pegm.first_position >= pegm.num_positions) {
    log_fatal("RPEGM_FROM %d is not a position index below %d",
              pegm.first_position, pegm.num_positions);
  }
  pegm.chosen = malloc_or_die(sizeof(Move) * num_methods * pegm.num_positions);
  pegm.seconds =
      malloc_or_die(sizeof(double) * num_methods * pegm.num_positions);
  printf("RPEGM: %d positions, bag %d to %d, %d settings, PEG settings '%s'\n"
         "RPEGM base: %s\n",
         pegm.num_positions, min_bag, max_bag, num_methods, peg_extra, base);
  for (int method = 0; method < num_methods; method++) {
    char *settings = get_formatted_string(
        "%s %s", base, string_splitter_get_item(methods, method));
    Config *config = rcr_config(lexicon, settings);
    free(settings);
    thread_control_set_status(config_get_thread_control(config),
                              THREAD_CONTROL_STATUS_STARTED);
    NcrShared shared;
    memset(&shared, 0, sizeof(shared));
    for (int slot = 0; slot < RCR_NUM_METHODS; slot++) {
      config_fill_sim_args(config, NULL, NULL, NULL, NULL,
                           &shared.base_args[slot]);
      shared.num_plays[slot] = shared.base_args[slot].num_plays;
    }
    NcrWorker *workers =
        rcr_workers_create(&shared, pegm.positions[0].game, num_threads);
    PegmWorkerArgs *worker_args =
        malloc_or_die(sizeof(PegmWorkerArgs) * num_threads);
    cpthread_t *threads = malloc_or_die(sizeof(cpthread_t) * num_threads);
    pegm.method = method;
    atomic_init(&pegm.next_position, pegm.first_position);
    for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
      worker_args[worker_idx].worker = &workers[worker_idx];
      worker_args[worker_idx].pegm = &pegm;
      cpthread_create(&threads[worker_idx], pegm_choose_run,
                      &worker_args[worker_idx]);
    }
    for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
      cpthread_join(threads[worker_idx]);
    }
    rcr_workers_destroy(workers, num_threads);
    free(worker_args);
    free(threads);
    config_destroy(config);
    printf("RPEGM chose with setting %d\n", method);
    (void)fflush(stdout);
  }

  // The cache.
  PegmCache cache;
  memset(&cache, 0, sizeof(cache));
  const char *cache_file = rcr_env_string("RPEG_CACHE", "rlamc_peg_cache.txt");
  // The cache may not exist yet.
  // NOLINTNEXTLINE(android-cloexec-fopen)
  FILE *cache_in = fopen(cache_file, "r");
  if (cache_in != NULL) {
    unsigned long long key = 0;
    double win = 0;
    // NOLINTNEXTLINE(cert-err34-c)
    while (fscanf(cache_in, "PEGC %llx %lf\n", &key, &win) == 2) {
      pegm_cache_add(&cache, (uint64_t)key, win);
    }
    (void)fclose(cache_in);
  }
  FILE *cache_out = fopen_or_die(cache_file, "a");

  char *peg_settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all -threads "
      "%d -savesettings false -hr false %s %s",
      lexicon, num_threads, rcr_tables(), peg_extra);
  Config *peg_config = config_create_or_die(peg_settings);
  free(peg_settings);
  load_and_exec_config_or_die(peg_config, "cgp " EMPTY_CGP_WITHOUT_OPTIONS);
  double *wins = malloc_or_die(sizeof(double) * num_methods);
  double *diff_sum = calloc_or_die(num_methods, sizeof(double));
  double *diff_sum_sq = calloc_or_die(num_methods, sizeof(double));
  int *differ = calloc_or_die(num_methods, sizeof(int));
  int solved = 0;
  for (int pos_idx = pegm.first_position; pos_idx < pegm.num_positions;
       pos_idx++) {
    const NcrPosition *position = &pegm.positions[pos_idx];
    const Move *base_move = &pegm.chosen[pos_idx];
    bool any_differ = false;
    for (int method = 1; method < num_methods; method++) {
      if (compare_moves_without_equity(
              &pegm.chosen[method * pegm.num_positions + pos_idx], base_move,
              true) != -1) {
        any_differ = true;
      }
    }
    if (!any_differ) {
      continue;
    }
    Game *copy = game_duplicate(position->game);
    return_rack_to_bag(copy, 1 - game_get_player_on_turn_index(copy));
    char *cgp = game_get_cgp(copy, true);
    game_destroy(copy);
    // The plays to solve: those not cached.
    StringBuilder *only = string_builder_create();
    // The plays listed so far, each between commas, so that a play is
    // matched whole: 14f.QiN is not 14f.QiNS.
    StringBuilder *listed = string_builder_create();
    string_builder_add_string(listed, ",");
    int num_to_solve = 0;
    for (int method = 0; method < num_methods; method++) {
      char *move = pegm_move_string(
          position->game, &pegm.chosen[method * pegm.num_positions + pos_idx]);
      char *delimited = get_formatted_string(",%s,", move);
      double win = 0;
      if (!pegm_cache_find(&cache, pegm_key(cgp, move, peg_extra), &win) &&
          !has_substring(string_builder_peek(listed), delimited)) {
        string_builder_add_formatted_string(only, "%s%s",
                                            num_to_solve > 0 ? "," : "", move);
        string_builder_add_formatted_string(listed, "%s,", move);
        num_to_solve++;
      }
      free(delimited);
      free(move);
    }
    string_builder_destroy(listed);
    if (num_to_solve > 0) {
      char *load = get_formatted_string("cgp %s", cgp);
      load_and_exec_config_or_die(peg_config, load);
      free(load);
      char *command =
          get_formatted_string("peg only %s", string_builder_peek(only));
      load_and_exec_config_or_die(peg_config, command);
      free(command);
      solved++;
      const PegResult *result = config_get_peg_result(peg_config);
      // Each solved play is cached under the string of the chosen play it
      // is, matched by the move itself: PEG may write a play otherwise.
      for (int cand_idx = 0; cand_idx < result->n_top_cands; cand_idx++) {
        for (int method = 0; method < num_methods; method++) {
          const Move *chosen =
              &pegm.chosen[method * pegm.num_positions + pos_idx];
          if (compare_moves_without_equity(&result->top_cands[cand_idx].move,
                                           chosen, true) != -1) {
            continue;
          }
          char *move = pegm_move_string(position->game, chosen);
          const uint64_t key = pegm_key(cgp, move, peg_extra);
          double cached = 0;
          if (!pegm_cache_find(&cache, key, &cached)) {
            pegm_cache_add(&cache, key, result->top_cands[cand_idx].win_pct);
            (void)fprintf(cache_out, "PEGC %llx %.6f\n",
                          (unsigned long long)key,
                          result->top_cands[cand_idx].win_pct);
          }
          free(move);
        }
      }
      (void)fflush(cache_out);
    }
    string_builder_destroy(only);
    for (int method = 0; method < num_methods; method++) {
      char *move = pegm_move_string(
          position->game, &pegm.chosen[method * pegm.num_positions + pos_idx]);
      if (!pegm_cache_find(&cache, pegm_key(cgp, move, peg_extra),
                           &wins[method])) {
        log_fatal("peg did not solve %s", move);
      }
      free(move);
    }
    for (int method = 1; method < num_methods; method++) {
      const double diff = wins[method] - wins[0];
      diff_sum[method] += diff;
      diff_sum_sq[method] += diff * diff;
      differ[method] += compare_moves_without_equity(
                            &pegm.chosen[method * pegm.num_positions + pos_idx],
                            base_move, true) != -1
                            ? 1
                            : 0;
      if (diff != 0.0) {
        char *base_string = pegm_move_string(position->game, base_move);
        char *move_string = pegm_move_string(
            position->game,
            &pegm.chosen[method * pegm.num_positions + pos_idx]);
        printf("RPEGM pos %d bag %d setting %d diff %+.4f: %s %.4f -> %s "
               "%.4f | %s\n",
               pos_idx, position->bag, method, diff, base_string, wins[0],
               move_string, wins[method], cgp);
        free(base_string);
        free(move_string);
      }
    }
    free(cgp);
  }
  (void)fclose(cache_out);
  const double n = pegm.num_positions - pegm.first_position;
  for (int method = 0; method < num_methods; method++) {
    double seconds = 0.0;
    for (int pos_idx = pegm.first_position; pos_idx < pegm.num_positions;
         pos_idx++) {
      seconds += pegm.seconds[method * pegm.num_positions + pos_idx];
    }
    const double mean = diff_sum[method] / n;
    printf("RPEGM setting %d [%s]: vs setting 0 win %+.5f (SE %.5f) per "
           "position, %d differ; sim %.3f s per position\n",
           method, string_splitter_get_item(methods, method), mean,
           sqrt(fmax(diff_sum_sq[method] / n - mean * mean, 0.0) / n),
           differ[method], seconds / n);
  }
  printf("RPEGM: %d positions solved, cache %d entries\n", solved, cache.count);
  free(wins);
  free(diff_sum);
  free(diff_sum_sq);
  free(differ);
  free(cache.keys);
  free(cache.wins);
  free(pegm.chosen);
  free(pegm.seconds);
  for (int pos_idx = 0; pos_idx < pegm.num_positions; pos_idx++) {
    game_destroy(pegm.positions[pos_idx].game);
  }
  free(pegm.positions);
  string_splitter_destroy(methods);
  config_destroy(peg_config);
  config_destroy(base_config);
}

// On demand (rlamctrace): what a recursive search chooses. In the position
// RT_CGP (its player on turn's rack is drawn), RT_SAMPLES racks are drawn
// for the player on turn from the unseen tiles, and the recursive search of
// RT_SPEC (with RT_SETTINGS for the sim's scoring) chooses a play for each.
// Prints each distinct play chosen, how often, and its rank among the
// player's plays by static equity for the racks it was chosen with.
void test_rlamc_trace(void) {
  log_set_level(LOG_FATAL);
  const char *cgp = getenv("RT_CGP");
  if (cgp == NULL) {
    log_fatal("RT_CGP is required");
  }
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-threads 1 -savesettings false -hr false %s %s",
      rcr_env_string("RCR_LEX", "CSW24"), rcr_tables(),
      rcr_env_string("RT_SETTINGS", ""));
  Config *config = config_create_or_die(settings);
  free(settings);
  char *load = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, load);
  free(load);
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  RlamcArgs args;
  rlamc_args_parse(rcr_env_string("RT_SPEC", "plies=0,lamc=1"), &args,
                   error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  const RlamcEval eval = {
      .win_pcts = config_get_win_pcts(config),
      .utility_w_winpct = 1.0,
      .utility_w_spread = 0.5,
      .utility_spread_scale = 100.0,
      .finish_endgame = rcr_env_int("RT_FINISH", 1) != 0,
      .finish_max_bag = 0,
  };
  const Game *base = config_get_game(config);
  Game *game = game_duplicate(base);
  CrossSetCache *cross_set_cache = cross_set_cache_create(16);
  RlamcWorker *worker = rlamc_worker_create(game, cross_set_cache);
  rlamc_worker_reset(worker, 12345);
  MoveList *all = move_list_create(5000);
  const int num_samples = rcr_env_int("RT_SAMPLES", 200);
  enum { RT_MAX_DISTINCT = 2048 };
  char *names[RT_MAX_DISTINCT];
  int counts[RT_MAX_DISTINCT];
  int rank_sums[RT_MAX_DISTINCT];
  int num_distinct = 0;
  int top_static = 0;
  const int mover = game_get_player_on_turn_index(base);
  rlamc_totals_reset();
  const int64_t start_ns = ctimer_monotonic_ns();
  for (int sample = 0; sample < num_samples; sample++) {
    game_copy(game, base);
    return_rack_to_bag(game, mover);
    game_seed(game, rcr_mix(777 + (uint64_t)sample));
    draw_to_full_rack(game, mover);
    Move chosen;
    move_copy(&chosen, rlamc_choose_move(worker, &args, &eval, game,
                                         rcr_mix(31 + (uint64_t)sample), NULL));
    // Its rank by static equity.
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = all,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&gen_args);
    move_list_sort_moves(all);
    int rank = -1;
    for (int move_idx = 0; move_idx < move_list_get_count(all); move_idx++) {
      if (compare_moves_without_equity(move_list_get_move(all, move_idx),
                                       &chosen, true) == -1) {
        rank = move_idx;
        break;
      }
    }
    top_static += rank == 0 ? 1 : 0;
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &chosen,
                            game_get_ld(game), false);
    const char *name = string_builder_peek(sb);
    int found = -1;
    for (int idx = 0; idx < num_distinct; idx++) {
      if (strings_equal(names[idx], name)) {
        found = idx;
        break;
      }
    }
    if (found < 0 && num_distinct < RT_MAX_DISTINCT) {
      found = num_distinct++;
      names[found] = string_duplicate(name);
      counts[found] = 0;
      rank_sums[found] = 0;
    }
    if (found >= 0) {
      counts[found]++;
      rank_sums[found] += rank;
    }
    string_builder_destroy(sb);
  }
  const double seconds = (double)(ctimer_monotonic_ns() - start_ns) / 1e9;
  const RlamcTotals totals = rlamc_get_totals();
  printf("RT %d racks, %d distinct plays, top static play chosen %d times\n",
         num_samples, num_distinct, top_static);
  printf("RT %.3f s, searches %llu (inner %llu), rollouts %llu, stopped "
         "%llu, decision hits %llu\n",
         seconds, (unsigned long long)totals.searches,
         (unsigned long long)totals.inner_searches,
         (unsigned long long)totals.samples, (unsigned long long)totals.stopped,
         (unsigned long long)totals.decision_hits);
  for (int idx = 0; idx < num_distinct; idx++) {
    printf("RT play %-20s chosen %4d mean static rank %.1f\n", names[idx],
           counts[idx], (double)rank_sums[idx] / counts[idx]);
    free(names[idx]);
  }
  move_list_destroy(all);
  rlamc_worker_destroy(worker);
  cross_set_cache_destroy(cross_set_cache);
  game_destroy(game);
  config_destroy(config);
}
