// Nested rollouts (Phase 2 of docs/lamc-plan.md): the CI test, and the
// continuation regret harness that compares sim settings on late positions.
//
// Continuation regret (on demand, nestedcr). For each of a set of positions,
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
//   NCR_POSITIONS   positions (default 200)
//   NCR_FIRST_GAME  games to skip, for a fresh set of positions (default 0)
//   NCR_MIN_BAG     fewest tiles in the bag of a position (default 1)
//   NCR_MAX_BAG     most tiles in the bag of a position (default 20)
//   NCR_LEX         lexicon (default CSW21)
//   NCR_BASE        settings both A and B take
//   NCR_A, NCR_B    each one's settings, after NCR_BASE
//   NCR_CONT        the continuation engine's sim settings
//   NCR_CONTS       continuations per disagreement (default 32)
//   NCR_EG_PLIES    the continuation's endgame depth (default 4)
//   NCR_THREADS     positions solved at once, one thread each (default 10)

#include "nested_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/cpthread_defs.h"
#include "../src/compat/ctime.h"
#include "../src/def/game_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/autoplay_solver_settings.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/nested_args.h"
#include "../src/ent/player.h"
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
#include "../src/impl/random_variable.h"
#include "../src/impl/rollout_nested.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
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
  NCR_MAX_THREADS = 64,
  NCR_MOVE_STRING_SIZE = 64,
  NCR_NUM_METHODS = 3,
  NCR_METHOD_A = 0,
  NCR_METHOD_B = 1,
  NCR_METHOD_CONT = 2,
  NCR_MAX_TURNS = 200,
};

// The share of memory the continuations' endgame table takes.
static const double NCR_TT_FRACTION = 0.04;

static uint64_t ncr_mix(uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

static int ncr_env_int(const char *name, const int fallback) {
  const char *value = getenv(name);
  return value ? atoi(value) : fallback;
}

static const char *ncr_env_string(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value ? value : fallback;
}

typedef struct NcrPosition {
  Game *game;
  int bag;
  Move chosen[2];
  char chosen_string[2][NCR_MOVE_STRING_SIZE];
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
  SimArgs base_args[NCR_NUM_METHODS];
  int num_plays[NCR_NUM_METHODS];
  TranspositionTable *tt;
  atomic_int next_position;
  atomic_int done;
} NcrShared;

typedef struct NcrWorker {
  NcrShared *shared;
  int worker_idx;
  Game *game;
  MoveList *lists[NCR_NUM_METHODS];
  SimCtx *ctxs[NCR_NUM_METHODS];
  SimResults *results[NCR_NUM_METHODS];
  AutoplaySolverCtx *solver;
  AutoplaySolverSettings solver_settings;
  ErrorStack *error_stack;
} NcrWorker;

// The play method chooses in the worker's game, copied into out.
static void ncr_choose(NcrWorker *worker, const int method, const uint64_t seed,
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
    log_fatal("nested continuation regret: a sim failed");
  }
  move_copy(out, move);
}

// Plays the worker's game to its end with the continuation engine. Returns
// the result for player mover: 1 for a win, 0.5 for a tie, 0 for a loss; and
// the final spread for mover in *spread.
static double ncr_play_out(NcrWorker *worker, const int mover,
                           const uint64_t seed, double *spread) {
  Game *game = worker->game;
  Move move;
  for (int turn = 0; turn < NCR_MAX_TURNS && !game_over(game); turn++) {
    if (bag_get_letters(game_get_bag(game)) == 0 &&
        worker->shared->eg_plies > 0) {
      const Move *solved = autoplay_solver_solve(
          worker->solver, &worker->solver_settings, game, worker->shared->tt, 1,
          ncr_mix(seed + 7919ULL * (uint64_t)turn) | 1ULL, NULL, false,
          worker->error_stack);
      if (solved == NULL || !error_stack_is_empty(worker->error_stack)) {
        error_stack_print_and_reset(worker->error_stack);
        log_fatal("nested continuation regret: an endgame solve failed");
      } else {
        move_copy(&move, solved);
      }
    } else {
      ncr_choose(worker, NCR_METHOD_CONT,
                 ncr_mix(seed + 104729ULL * (uint64_t)turn), &move);
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

static void ncr_evaluate(NcrWorker *worker, NcrPosition *position,
                         const uint64_t pos_seed) {
  const NcrShared *shared = worker->shared;
  const int mover = game_get_player_on_turn_index(position->game);
  const int other = 1 - mover;
  double win_sum = 0.0;
  double win_sum_sq = 0.0;
  double spread_sum = 0.0;
  double spread_sum_sq = 0.0;
  for (int cont_idx = 0; cont_idx < shared->num_conts; cont_idx++) {
    const uint64_t cont_seed = ncr_mix(pos_seed ^ (0x51ED27ULL + cont_idx));
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
      wins[branch] = ncr_play_out(worker, mover, cont_seed, &spreads[branch]);
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

static void *ncr_worker_run(void *data) {
  NcrWorker *worker = (NcrWorker *)data;
  NcrShared *shared = worker->shared;
  while (true) {
    const int pos_idx = atomic_fetch_add(&shared->next_position, 1);
    if (pos_idx >= shared->num_positions) {
      break;
    }
    NcrPosition *position = &shared->positions[pos_idx];
    const uint64_t pos_seed = ncr_mix(1000003ULL * (uint64_t)(pos_idx + 1));
    for (int method = 0; method < 2; method++) {
      game_copy(worker->game, position->game);
      const double start = ((double)ctimer_monotonic_ns() / 1e9);
      ncr_choose(worker, method, pos_seed, &position->chosen[method]);
      position->seconds[method] = ((double)ctimer_monotonic_ns() / 1e9) - start;
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(position->game),
                              &position->chosen[method],
                              game_get_ld(position->game), false);
      (void)snprintf(position->chosen_string[method], NCR_MOVE_STRING_SIZE,
                     "%s", string_builder_peek(sb));
      string_builder_destroy(sb);
    }
    position->differ =
        compare_moves_without_equity(&position->chosen[0], &position->chosen[1],
                                     true) != -1;
    if (position->differ) {
      ncr_evaluate(worker, position, pos_seed);
    } else {
      position->win_diff = 0.0;
      position->win_diff_se = 0.0;
      position->spread_diff = 0.0;
      position->spread_diff_se = 0.0;
    }
    printf("NCR pos %d bag %d A %s (%.2fs) B %s (%.2fs) win %+.4f (%.4f) "
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
static int ncr_make_positions(const Game *base, NcrPosition *positions,
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
        min_bag + (int)(ncr_mix(game_idx) % (uint64_t)(max_bag - min_bag + 1));
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

static Config *ncr_config(const char *lexicon, const char *settings) {
  char *full = get_formatted_string(
      "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-threads 1 -savesettings false -hr false -sinfer false %s",
      lexicon, settings);
  Config *config = config_create_or_die(full);
  free(full);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  ErrorStack *error_stack = error_stack_create();
  config_load_win_pcts(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return config;
}

static void ncr_mean_se(const double *values, const int count, double *mean,
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

void test_nested_cr(void) {
  log_set_level(LOG_FATAL);
  const int num_positions = ncr_env_int("NCR_POSITIONS", 200);
  const int min_bag = ncr_env_int("NCR_MIN_BAG", 1);
  const int max_bag = ncr_env_int("NCR_MAX_BAG", 20);
  int num_threads = ncr_env_int("NCR_THREADS", 10);
  if (num_threads > NCR_MAX_THREADS) {
    num_threads = NCR_MAX_THREADS;
  }
  const char *lexicon = ncr_env_string("NCR_LEX", "CSW21");
  const char *base = ncr_env_string(
      "NCR_BASE",
      "-plies 2 -numplays 10 -scond none -tlim 0 -minplayiterations 10");
  const char *method_settings[NCR_NUM_METHODS] = {
      ncr_env_string("NCR_A", "-it 1000"),
      ncr_env_string("NCR_B", "-it 1000 -nested plies=0,k=6,m=8,h=1"),
      ncr_env_string("NCR_CONT",
                     "-plies 2 -numplays 8 -it 300 -scond none "
                     "-tlim 0 -minplayiterations 10 -sbagcycle false"),
  };

  NcrShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.num_threads = num_threads;
  shared.num_conts = ncr_env_int("NCR_CONTS", 32);
  shared.eg_plies = ncr_env_int("NCR_EG_PLIES", 4);
  Config *configs[NCR_NUM_METHODS];
  for (int method = 0; method < NCR_NUM_METHODS; method++) {
    char *settings =
        method == NCR_METHOD_CONT
            ? string_duplicate(method_settings[method])
            : get_formatted_string("%s %s", base, method_settings[method]);
    configs[method] = ncr_config(lexicon, settings);
    free(settings);
    thread_control_set_status(config_get_thread_control(configs[method]),
                              THREAD_CONTROL_STATUS_STARTED);
    config_fill_sim_args(configs[method], NULL, NULL, NULL, NULL,
                         &shared.base_args[method]);
    shared.num_plays[method] = shared.base_args[method].num_plays;
  }
  shared.tt = transposition_table_create(NCR_TT_FRACTION);
  shared.positions = calloc_or_die(num_positions, sizeof(NcrPosition));
  shared.num_positions = ncr_make_positions(
      config_get_game(configs[NCR_METHOD_A]), shared.positions, num_positions,
      min_bag, max_bag, (uint64_t)ncr_env_int("NCR_FIRST_GAME", 0));
  atomic_init(&shared.next_position, 0);
  atomic_init(&shared.done, 0);
  printf("NCR: %d positions, bag %d to %d, %d continuations, %d threads\n"
         "NCR base: %s\nNCR A: %s\nNCR B: %s\nNCR cont: %s\n",
         shared.num_positions, min_bag, max_bag, shared.num_conts, num_threads,
         base, method_settings[0], method_settings[1], method_settings[2]);
  (void)fflush(stdout);
  nested_totals_reset();

  NcrWorker *workers = calloc_or_die(num_threads, sizeof(NcrWorker));
  cpthread_t *threads = malloc_or_die(sizeof(cpthread_t) * num_threads);
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    NcrWorker *worker = &workers[worker_idx];
    worker->shared = &shared;
    worker->worker_idx = worker_idx;
    worker->game = game_duplicate(shared.positions[0].game);
    for (int method = 0; method < NCR_NUM_METHODS; method++) {
      worker->lists[method] = move_list_create(shared.num_plays[method]);
      worker->ctxs[method] = NULL;
      worker->results[method] = sim_results_create(0.0);
    }
    worker->solver = autoplay_solver_ctx_create();
    autoplay_solver_settings_set_defaults(&worker->solver_settings);
    worker->solver_settings.endgame_plies = shared.eg_plies;
    worker->solver_settings.peg_max_bag = 0;
    worker->error_stack = error_stack_create();
    cpthread_create(&threads[worker_idx], ncr_worker_run, worker);
  }
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    cpthread_join(threads[worker_idx]);
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
  ncr_mean_se(win_diffs, shared.num_positions, &win_mean, &win_se);
  ncr_mean_se(spread_diffs, shared.num_positions, &spread_mean, &spread_se);
  const NestedTotals totals = nested_get_totals();
  printf(
      "NCR summary: %d positions, %d differ (%.1f%%)\n"
      "NCR B - A per position: win %+.5f (SE %.5f), spread %+.3f (SE "
      "%.3f)\n"
      "NCR per differing position: win %+.4f, spread %+.2f\n"
      "NCR seconds per choice: A %.3f, B %.3f\n"
      "NCR nested searches %llu, samples %llu, candidates %.2f each, "
      "switched %.1f%%\n",
      shared.num_positions, differ,
      100.0 * differ / (double)shared.num_positions, win_mean, win_se,
      spread_mean, spread_se,
      differ > 0 ? win_mean * shared.num_positions / differ : 0.0,
      differ > 0 ? spread_mean * shared.num_positions / differ : 0.0,
      seconds[0] / shared.num_positions, seconds[1] / shared.num_positions,
      (unsigned long long)totals.searches, (unsigned long long)totals.samples,
      totals.searches > 0 ? (double)totals.candidates / totals.searches : 0.0,
      totals.searches > 0 ? 100.0 * totals.switched / totals.searches : 0.0);
  (void)fflush(stdout);

  free(win_diffs);
  free(spread_diffs);
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    NcrWorker *worker = &workers[worker_idx];
    game_destroy(worker->game);
    for (int method = 0; method < NCR_NUM_METHODS; method++) {
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
  for (int method = 0; method < NCR_NUM_METHODS; method++) {
    config_destroy(configs[method]);
  }
}

// A fingerprint of a nested sim's results.
static uint64_t nested_test_sim(const int num_threads, const char *nested) {
  char *settings = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 5 -plies 3 -threads %d -iterations 300 "
      "-minplayiterations 10 -scond none -tlim 0 -seed 42 -sinfer false "
      "-savesettings false -hr false -nested %s",
      num_threads, nested);
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(config, "cgp " JOSH2_CGP);
  load_and_exec_config_or_die(config, "gen");
  nested_totals_reset();
  load_and_exec_config_or_die(config, "sim");
  const NestedTotals totals = nested_get_totals();
  const SimResults *sim_results = config_get_sim_results(config);
  uint64_t hash = FNV_64_OFFSET_BASIS;
  const int num_plays = sim_results_get_number_of_plays(sim_results);
  assert(num_plays == 5);
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    const SimmedPlay *simmed_play =
        sim_results_get_simmed_play(sim_results, play_idx);
    const double mean = simmed_play_get_exact_win_pct_mean(simmed_play);
    if (getenv("NESTED_TEST_DEBUG")) {
      fprintf(stderr, "NTD %d threads play %d mean %.17g n %llu\n", num_threads,
              play_idx, mean,
              (unsigned long long)stat_get_num_samples(
                  simmed_play_get_equity_stat(simmed_play)));
    }
    uint64_t bits;
    memcpy(&bits, &mean, sizeof(bits));
    hash = fnv64a_step(hash, bits);
    hash = fnv64a_step(
        hash, stat_get_num_samples(simmed_play_get_equity_stat(simmed_play)));
  }
  if (getenv("NESTED_TEST_DEBUG")) {
    fprintf(stderr, "NTD %d threads searches %llu samples %llu switched %llu\n",
            num_threads, (unsigned long long)totals.searches,
            (unsigned long long)totals.samples,
            (unsigned long long)totals.switched);
  }
  // The totals are not part of the fingerprint: with several threads, BAI
  // can take samples it then discards, whose searches count too.
  if (strings_equal(nested, "off")) {
    assert(totals.searches == 0);
  } else {
    assert(totals.searches > 0);
    assert(totals.samples + totals.cache_misses > 0);
  }
  config_destroy(config);
  return hash;
}

static void test_nested_parse(void) {
  NestedArgs args;
  ErrorStack *error_stack = error_stack_create();
  nested_args_parse("plies=0+2,k=4,m=3,h=2,margin=12,maxbag=20,minbag=1,"
                    "alloc=race,z=1.5,switch=0.01",
                    &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(args.enabled);
  assert(args.plies_mask == 5U);
  assert(args.num_candidates == 4);
  assert(args.num_samples == 3);
  assert(args.nested_plies == 2);
  assert(args.margin == 12);
  assert(args.max_bag == 20);
  assert(args.min_bag == 1);
  assert(args.alloc == NESTED_ALLOC_RACE);
  assert(args.race_z == 1.5);
  assert(args.switch_margin == 0.01);
  nested_args_parse("off", &args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!args.enabled);
  const char *const bad[] = {"k=0", "plies=", "m=x", "alloc=fast", "q=1", "k"};
  for (int bad_idx = 0; bad_idx < 6; bad_idx++) {
    nested_args_parse(bad[bad_idx], &args, error_stack);
    assert(error_stack_top(error_stack) == ERROR_STATUS_NESTED_BAD_SPEC);
    error_stack_reset(error_stack);
  }
  error_stack_destroy(error_stack);
}

void test_nested(void) {
  test_nested_parse();
  const char *const specs[] = {"off", "plies=0,k=4,m=3,h=1",
                               "plies=0+1,k=3,m=2,h=2,alloc=race,z=1",
                               "plies=0+1,k=5,m=4,share=1"};
  for (int spec_idx = 0; spec_idx < 4; spec_idx++) {
    const uint64_t one = nested_test_sim(1, specs[spec_idx]);
    const uint64_t several = nested_test_sim(4, specs[spec_idx]);
    assert(one == several);
  }
}

// On demand (nestedbench): one sim of NB_CGP with NB_SETTINGS, timed, with
// the nested totals.
void test_nested_bench(void) {
  const char *cgp = ncr_env_string(
      "NB_CGP", "15/15/10T4/10AD3/10MO3/5ZEK2EW3/6MITT1N3/7DOWLY3/8POI4/"
                "3ALBUGoS5/15/15/15/15/15 CGILLRS/ 98/167 0");
  const char *settings = ncr_env_string(
      "NB_SETTINGS", "-numplays 10 -plies 2 -threads 2 -iterations 3000 "
                     "-minplayiterations 10 -scond none -tlim 0 -seed 42 "
                     "-sbagcycle false -nested plies=0,k=6,m=8,share=1");
  char *full = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-sinfer false -savesettings false -hr false %s",
      settings);
  Config *config = config_create_or_die(full);
  free(full);
  char *load = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, load);
  free(load);
  const int bag = ncr_env_int("NB_BAG", 0);
  if (bag > 0) {
    // A position from a game of static plays, with bag tiles in the bag.
    NcrPosition position;
    ncr_make_positions(config_get_game(config), &position, 1, bag, bag,
                       (uint64_t)ncr_env_int("NB_GAME", 0));
    // The opponent's rack is unknown to the sim.
    return_rack_to_bag(position.game,
                       1 - game_get_player_on_turn_index(position.game));
    char *position_cgp = game_get_cgp(position.game, true);
    game_destroy(position.game);
    load = get_formatted_string("cgp %s", position_cgp);
    printf("NB position: %s\n", position_cgp);
    free(position_cgp);
    load_and_exec_config_or_die(config, load);
    free(load);
  }
  load_and_exec_config_or_die(config, "gen");
  nested_totals_reset();
  const double start = (double)ctimer_monotonic_ns() / 1e9;
  load_and_exec_config_or_die(config, "sim");
  const double seconds = (double)ctimer_monotonic_ns() / 1e9 - start;
  const NestedTotals totals = nested_get_totals();
  const SimResults *sim_results = config_get_sim_results(config);
  printf(
      "NB %.3f s, %llu rollouts, nested searches %llu, candidates %.2f, "
      "switched %.1f%%, samples %llu, cache hits %llu misses %llu "
      "(%.1f%% hit)\n",
      seconds, (unsigned long long)sim_results_get_iteration_count(sim_results),
      (unsigned long long)totals.searches,
      totals.searches ? (double)totals.candidates / totals.searches : 0.0,
      totals.searches ? 100.0 * totals.switched / totals.searches : 0.0,
      (unsigned long long)totals.samples, (unsigned long long)totals.cache_hits,
      (unsigned long long)totals.cache_misses,
      (totals.cache_hits + totals.cache_misses) > 0
          ? 100.0 * totals.cache_hits /
                (double)(totals.cache_hits + totals.cache_misses)
          : 0.0);
  config_destroy(config);
}

// Ground truth tables (on demand: nestedgt, nestedeval).
//
// nestedgt writes a table of positions and, for each, the continuation value
// of each of its top plays by static equity: NGT_CANDS plays, each played out
// NCR_CONTS times by the continuation engine on the same continuation seeds,
// so that the plays' values are compared on common random numbers.
// nestedeval then scores sim settings against the table without playing
// anything out: each settings' play in each position is looked up, and its
// regret is the best value in the position less its own. A play outside the
// table is counted, not scored.
//
// Settings (and NCR_* as above for positions and the continuation engine):
//   NGT_FILE     the table (default nested_gt.txt)
//   NGT_CANDS    plays valued per position (default 10)
//   NE_METHODS   |-separated sim settings to score, each after NCR_BASE

enum {
  NGT_MAX_CANDS = 64,
  NGT_MAX_CONTS = 1024,
  NGT_LINE_SIZE = 65536,
};

typedef struct GtPosition {
  Game *game;
  int bag;
  int pos_id;
  int num_cands;
  Move cands[NGT_MAX_CANDS];
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
  return ncr_mix(ncr_mix(0x6A09E667ULL + (uint64_t)pos_id) ^
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
    const double win = ncr_play_out(worker, mover, cont_seed, &spread);
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
    printf("NGT position %d (bag %d) done\n", position->pos_id, position->bag);
    (void)fflush(stdout);
  }
  return NULL;
}

// The workers, each with its own game, lists and sim contexts for settings
// base_args, and endgame solving at eg_plies.
static NcrWorker *ncr_workers_create(NcrShared *shared, const Game *game,
                                     const int num_threads) {
  NcrWorker *workers = calloc_or_die(num_threads, sizeof(NcrWorker));
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    NcrWorker *worker = &workers[worker_idx];
    worker->shared = shared;
    worker->worker_idx = worker_idx;
    worker->game = game_duplicate(game);
    for (int method = 0; method < NCR_NUM_METHODS; method++) {
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

static void ncr_workers_destroy(NcrWorker *workers, const int num_threads) {
  for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
    NcrWorker *worker = &workers[worker_idx];
    game_destroy(worker->game);
    for (int method = 0; method < NCR_NUM_METHODS; method++) {
      move_list_destroy(worker->lists[method]);
      sim_ctx_destroy(worker->ctxs[method]);
      sim_results_destroy(worker->results[method]);
    }
    autoplay_solver_ctx_destroy(worker->solver);
    error_stack_destroy(worker->error_stack);
  }
  free(workers);
}

void test_nested_gt(void) {
  log_set_level(LOG_FATAL);
  const int num_positions = ncr_env_int("NCR_POSITIONS", 200);
  const int min_bag = ncr_env_int("NCR_MIN_BAG", 1);
  const int max_bag = ncr_env_int("NCR_MAX_BAG", 20);
  int num_threads = ncr_env_int("NCR_THREADS", 10);
  if (num_threads > NCR_MAX_THREADS) {
    num_threads = NCR_MAX_THREADS;
  }
  const char *lexicon = ncr_env_string("NCR_LEX", "CSW21");
  const char *cont_settings = ncr_env_string(
      "NCR_CONT", "-plies 2 -numplays 8 -it 300 -scond none -tlim 0 "
                  "-minplayiterations 10 -sbagcycle false");
  NcrShared shared;
  memset(&shared, 0, sizeof(shared));
  shared.num_threads = num_threads;
  shared.eg_plies = ncr_env_int("NCR_EG_PLIES", 4);
  Config *config = ncr_config(lexicon, cont_settings);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  for (int method = 0; method < NCR_NUM_METHODS; method++) {
    config_fill_sim_args(config, NULL, NULL, NULL, NULL,
                         &shared.base_args[method]);
    shared.num_plays[method] = shared.base_args[method].num_plays;
  }
  shared.tt = transposition_table_create(NCR_TT_FRACTION);

  GtShared gt;
  memset(&gt, 0, sizeof(gt));
  gt.num_cands = ncr_env_int("NGT_CANDS", 10);
  gt.num_conts = ncr_env_int("NCR_CONTS", 32);
  assert(gt.num_cands <= NGT_MAX_CANDS && gt.num_conts <= NGT_MAX_CONTS);
  NcrPosition *made = calloc_or_die(num_positions, sizeof(NcrPosition));
  const int first_game = ncr_env_int("NCR_FIRST_GAME", 0);
  gt.num_positions =
      ncr_make_positions(config_get_game(config), made, num_positions, min_bag,
                         max_bag, (uint64_t)first_game);
  gt.positions = calloc_or_die(gt.num_positions, sizeof(GtPosition));
  for (int pos_idx = 0; pos_idx < gt.num_positions; pos_idx++) {
    gt.positions[pos_idx].game = made[pos_idx].game;
    gt.positions[pos_idx].bag = made[pos_idx].bag;
    gt.positions[pos_idx].pos_id = first_game * 1000 + pos_idx;
  }
  free(made);
  const char *file = ncr_env_string("NGT_FILE", "nested_gt.txt");
  gt.out = fopen_or_die(file, "a");
  cpthread_mutex_init(&gt.out_mutex);
  atomic_init(&gt.next_position, 0);
  printf("NGT: %d positions, bag %d to %d, %d plays, %d continuations, %d "
         "threads, to %s\nNGT cont: %s\n",
         gt.num_positions, min_bag, max_bag, gt.num_cands, gt.num_conts,
         num_threads, file, cont_settings);
  (void)fflush(stdout);
  NcrWorker *workers =
      ncr_workers_create(&shared, gt.positions[0].game, num_threads);
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
  ncr_workers_destroy(workers, num_threads);
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
    ncr_choose(worker, NCR_METHOD_A,
               ncr_mix(0x243F6A88ULL + (uint64_t)position->pos_id), &chosen);
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
  char *line = malloc_or_die(NGT_LINE_SIZE);
  int capacity = 64;
  int count = 0;
  GtPosition *positions = calloc_or_die(capacity, sizeof(GtPosition));
  GtPosition *current = NULL;
  while (fgets(line, NGT_LINE_SIZE, in)) {
    if (strncmp(line, "GTPOS ", 6) == 0) {
      int pos_id = 0;
      int bag = 0;
      int cands = 0;
      int conts = 0;
      int consumed = 0;
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
      if (sscanf(line, "GTCAND %d %d%n", &pos_id, &cand_idx, &consumed) != 2 ||
          current == NULL || pos_id != current->pos_id) {
        log_fatal("bad table line: %s", line);
      }
      const char *cursor = line + consumed;
      for (int cont_idx = 0; cont_idx < *num_conts; cont_idx++) {
        double win = 0;
        double spread = 0;
        int used = 0;
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

void test_nested_eval(void) {
  log_set_level(LOG_FATAL);
  int num_threads = ncr_env_int("NCR_THREADS", 10);
  if (num_threads > NCR_MAX_THREADS) {
    num_threads = NCR_MAX_THREADS;
  }
  const char *lexicon = ncr_env_string("NCR_LEX", "CSW21");
  const char *base =
      ncr_env_string("NCR_BASE", "-plies 2 -numplays 10 -scond none -tlim 0 "
                                 "-minplayiterations 10");
  StringSplitter *methods = split_string(
      ncr_env_string("NE_METHODS", "-it 1000|-it 1000 -nested plies=0"), '|',
      false);
  const int num_methods = string_splitter_get_number_of_items(methods);
  Config *loader = ncr_config(lexicon, "");
  EvalShared eval;
  memset(&eval, 0, sizeof(eval));
  eval.num_positions = gt_load(ncr_env_string("NGT_FILE", "nested_gt.txt"),
                               loader, &eval.positions, &eval.num_conts);
  eval.num_methods = num_methods;
  eval.chosen = malloc_or_die(sizeof(int) * num_methods * eval.num_positions);
  eval.seconds =
      malloc_or_die(sizeof(double) * num_methods * eval.num_positions);
  printf("NE: %d positions, %d continuations each, %d threads\nNE base: %s\n",
         eval.num_positions, eval.num_conts, num_threads, base);
  for (int method = 0; method < num_methods; method++) {
    char *settings = get_formatted_string(
        "%s %s", base, string_splitter_get_item(methods, method));
    Config *config = ncr_config(lexicon, settings);
    free(settings);
    thread_control_set_status(config_get_thread_control(config),
                              THREAD_CONTROL_STATUS_STARTED);
    NcrShared shared;
    memset(&shared, 0, sizeof(shared));
    for (int slot = 0; slot < NCR_NUM_METHODS; slot++) {
      config_fill_sim_args(config, NULL, NULL, NULL, NULL,
                           &shared.base_args[slot]);
      shared.num_plays[slot] = shared.base_args[slot].num_plays;
    }
    NcrWorker *workers =
        ncr_workers_create(&shared, eval.positions[0].game, num_threads);
    EvalWorkerArgs *worker_args =
        malloc_or_die(sizeof(EvalWorkerArgs) * num_threads);
    cpthread_t *threads = malloc_or_die(sizeof(cpthread_t) * num_threads);
    eval.method = method;
    atomic_init(&eval.next_position, 0);
    nested_totals_reset();
    for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
      worker_args[worker_idx].worker = &workers[worker_idx];
      worker_args[worker_idx].eval = &eval;
      cpthread_create(&threads[worker_idx], eval_worker_run,
                      &worker_args[worker_idx]);
    }
    for (int worker_idx = 0; worker_idx < num_threads; worker_idx++) {
      cpthread_join(threads[worker_idx]);
    }
    ncr_workers_destroy(workers, num_threads);
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
      const int group = position->bag <= 3    ? 0
                        : position->bag <= 7  ? 1
                        : position->bag <= 12 ? 2
                                              : 3;
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
      if (getenv("NE_CHOICES")) {
        printf("NE choice %d %d %d %d\n", method, position->pos_id,
               position->bag, index);
      }
      differ += index != base_index ? 1 : 0;
    }
    const double n = inside > 0 ? inside : 1;
    const double wr_mean = win_regret[0] / n;
    const double sr_mean = spread_regret[0] / n;
    const double d_mean = diff[0] / n;
    const NestedTotals totals = nested_get_totals();
    printf("NE method %d [%s]: regret win %.5f (SE %.5f) spread %.3f (SE "
           "%.3f); vs method 0 win %+.5f (SE %.5f), %d differ; %d of %d "
           "inside; %.3f s each; nested switched %.1f%%\n",
           method, string_splitter_get_item(methods, method), wr_mean,
           sqrt(fmax(win_regret[1] / n - wr_mean * wr_mean, 0) / n), sr_mean,
           sqrt(fmax(spread_regret[1] / n - sr_mean * sr_mean, 0) / n), d_mean,
           sqrt(fmax(diff[1] / n - d_mean * d_mean, 0) / n), differ, inside,
           eval.num_positions, seconds / eval.num_positions,
           totals.searches ? 100.0 * totals.switched / totals.searches : 0.0);
    const double sd_mean = spread_diff[0] / n;
    printf("NE method %d vs method 0 spread %+.3f (SE %.3f)\n", method, sd_mean,
           sqrt(fmax(spread_diff[1] / n - sd_mean * sd_mean, 0) / n));
    const double or_mean = oracle_regret[0] / n;
    printf(
        "NE method %d oracle regret %.5f (SE %.5f); by bag 1-3 %.4f (%d), "
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

// On demand (nestedvar): rollout data for studying what explains a
// rollout's outcome. For each position of the table NGT_FILE and each of its
// plays, NV_SEEDS rollouts on the seeds every play shares, each written as a
// line: the position, the play, the seed's index, the rollout's utility, the
// opponent's rack the seed drew, and the mover's rack after the play's draw.
// Sim settings come from NCR_BASE; the output goes to NV_FILE.

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
    sim_args.seed = ncr_mix(0x13198A2EULL + (uint64_t)position->pos_id);
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
          ncr_mix(ncr_mix((uint64_t)position->pos_id) + (uint64_t)seed_idx);
      for (int cand_idx = 0; cand_idx < move_list_get_count(move_list);
           cand_idx++) {
        const double utility = rvs_sample_with_seed(
            rvs, (uint64_t)cand_idx, seed, args->worker_idx, NULL, NULL);
        // The racks the rollout drew, drawn again the same way.
        game_copy(copy, position->game);
        return_rack_to_bag(copy, 1 - mover);
        game_seed(copy, seed);
        draw_to_full_rack(copy, 1 - mover);
        string_builder_add_formatted_string(sb, "NV %d %d %d %.6f ",
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

void test_nested_var(void) {
  log_set_level(LOG_FATAL);
  int num_threads = ncr_env_int("NCR_THREADS", 10);
  if (num_threads > NCR_MAX_THREADS) {
    num_threads = NCR_MAX_THREADS;
  }
  const char *lexicon = ncr_env_string("NCR_LEX", "CSW21");
  const char *base =
      ncr_env_string("NCR_BASE", "-plies 2 -numplays 10 -scond none -tlim 0 "
                                 "-minplayiterations 10 -sbagcycle false");
  Config *config = ncr_config(lexicon, base);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  VarShared var;
  memset(&var, 0, sizeof(var));
  int num_conts = 0;
  var.num_positions = gt_load(ncr_env_string("NGT_FILE", "nested_gt.txt"),
                              config, &var.positions, &num_conts);
  const int num_loaded = var.num_positions;
  const int limit = ncr_env_int("NV_POSITIONS", var.num_positions);
  if (limit < var.num_positions) {
    var.num_positions = limit;
  }
  var.num_seeds = ncr_env_int("NV_SEEDS", 200);
  config_fill_sim_args(config, NULL, NULL, NULL, NULL, &var.base_args);
  var.out = fopen_or_die(ncr_env_string("NV_FILE", "nested_var.txt"), "w");
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
