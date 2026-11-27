#include "rlamc.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/lamc_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/play_feature_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/best_move_carry.h"
#include "../ent/board.h"
#include "../ent/cross_set_cache.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/play_feature.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/rlamc_args.h"
#include "../ent/sim_args.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "census_rollout.h"
#include "gameplay.h"
#include "gp.h"
#include "move_gen.h"
#include "play_feature_registry.h"
#include "rack_draw.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Rounds every candidate gets before a race may drop any.
  RLAMC_RACE_MIN_ROUNDS = 2,
  // The shared cache's entries, a power of 2, and the most probes a lookup
  // makes before it gives up and computes the entry again.
  RLAMC_CACHE_BITS = 14,
  RLAMC_CACHE_PROBES = 8,
  // The most tiles a pool holds.
  RLAMC_MAX_POOL = 1024,
  // A paired LAMC search's alternating fits, and the fewest degrees of
  // freedom its noise estimate needs.
  RLAMC_PAIRED_ITERATIONS = 20,
  RLAMC_PAIRED_MIN_DOF = 4,
  // The most static plies a recursive rollout plays to finish a game, and the
  // ply it tells the census they are (past any the census serves).
  RLAMC_FINISH_MAX_PLIES = 40,
  RLAMC_FINISH_PLY = 1000,
  // The finish cache's and the rollout cache's entries, powers of 2.
  RLAMC_FINISH_CACHE_BITS = 16,
  RLAMC_ROLLOUT_CACHE_BITS = 16,
};

// A feature column whose standard deviation is below this is constant.
#define RLAMC_MIN_COLUMN_STDEV 1e-9
// The LAMC search's rollout variance floor and the rollout variance it
// assumes before any play has two samples (as Phase 1's, in lamc.c). Its
// signal variance is chosen by gp_fit_by_likelihood.
#define RLAMC_MIN_ROLLOUT_VARIANCE 1e-4
#define RLAMC_DEFAULT_ROLLOUT_VARIANCE 0.1
// The LAMC search's length scale, in standard deviations of each column, and
// the multiples of it each fit chooses among.
#define RLAMC_LENGTH_SCALE 1.5
// The relative slack on the LAMC search's bound on upper confidence bounds.
#define RLAMC_BOUND_SLACK 1e-9

// One cached reply: the replying player's best play to a board, from one
// sample's racks.
typedef struct RlamcReply {
  // The reply's score plus the value of the leave it keeps.
  Equity equity;
  // After the reply: tiles in the bag, and on the replier's rack.
  uint8_t bag_after;
  uint8_t replier_rack_after;
} RlamcReply;

// A play and its standardized static equity, for ordering plays by the LAMC
// search's prior mean.
typedef struct RlamcEquityRank {
  double equity;
  int play_idx;
} RlamcEquityRank;

// Higher equity first; ties by play.
static int rlamc_equity_rank_compare(const void *a, const void *b) {
  const RlamcEquityRank *first = (const RlamcEquityRank *)a;
  const RlamcEquityRank *second = (const RlamcEquityRank *)b;
  if (first->equity != second->equity) {
    return first->equity > second->equity ? -1 : 1;
  }
  return first->play_idx - second->play_idx;
}

static const double rlamc_length_multiples[] = {0.5, 1.0, 2.0};

// Standardizes count values, stride apart: a constant column becomes zeros.
static void rlamc_standardize(double *values, const int stride,
                              const int count) {
  double sum = 0.0;
  double squares = 0.0;
  for (int idx = 0; idx < count; idx++) {
    const double value = values[(ptrdiff_t)idx * stride];
    sum += value;
    squares += value * value;
  }
  const double mean = sum / count;
  const double variance = squares / count - mean * mean;
  const double stdev = variance > 0.0 ? sqrt(variance) : 0.0;
  for (int idx = 0; idx < count; idx++) {
    double *value = values + (ptrdiff_t)idx * stride;
    *value = stdev < RLAMC_MIN_COLUMN_STDEV ? 0.0 : (*value - mean) / stdev;
  }
}

struct RlamcWorker {
  // The level of search the worker makes (an index into RlamcArgs.levels),
  // the settings of its current search, and the worker of the level below
  // it, made when first needed.
  int level;
  const RlamcArgs *spec;
  RlamcWorker *inner;
  CrossSetCache *cross_set_cache;
  // The copy of the rollout's game the recursive rollouts play on.
  Game *game;
  // The candidates of the current search, best by static equity first.
  MoveList *candidate_list;
  int candidate_capacity;
  int num_candidates;
  Move candidates[RLAMC_MAX_CANDIDATES];
  // For the static plies of recursive rollouts.
  MoveList *reply_list;
  // Per candidate: whether it is still in the race, and its utility sums.
  bool alive[RLAMC_MAX_CANDIDATES];
  double sums[RLAMC_MAX_CANDIDATES];
  int counts[RLAMC_MAX_CANDIDATES];
  // Per candidate and round: the utilities, for paired differences.
  double *values;
  int values_capacity;
  Move chosen;
  // The shared cache: keys, and num_samples replies per entry.
  uint64_t sim_seed;
  uint64_t *cache_keys;
  RlamcReply *cache_replies;
  int cache_samples;
  // Whether the worker's game holds the search's position.
  bool game_is_current;
  // The census that steers the sim's rollouts, if one does, the thread's
  // worker of it, and the rollout ply of the replies the search plays.
  CensusRolloutWorker *census_worker;
  const CensusRollout *census;
  int reply_ply;
  // The decision cache (RlamcLevel.decision_cache): keys, and the play each
  // decision chose.
  uint64_t *decision_keys;
  Move *decision_moves;
  // The finish cache: keys of positions with an empty bag that a rollout
  // played to the end by static plays, and each player's points from there
  // to the end (see rlamc_finish).
  uint64_t *finish_keys;
  Equity *finish_points;
  // The rollout cache: keys of the positions after a candidate that leave
  // the bag empty, from which the rest of a rollout is static plays, and the
  // rollout's utility (see rlamc_rollout_key).
  uint64_t *rollout_keys;
  double *rollout_values;
  // For diverse candidates (RlamcLevel.universe): the play features, and the
  // embedding of the generated plays with its scratch.
  bool features[NUMBER_OF_PLAY_FEATURES];
  bool features_ready;
  PlayFeatureCtx feature_ctx;
  double *embedding;
  int embedding_capacity;
  // The capacity of the per-play arrays of the LAMC search below.
  int play_capacity;
  double *min_distance;
  bool *picked;
  // For the LAMC search (RlamcLevel.lamc_search), per generated play: its
  // standardized static equity, its recursive rollouts' sums and count, and
  // its upper confidence bound; the pool, the evaluated plays, and the
  // surrogate.
  double *static_equity;
  double *play_sums;
  double *play_squares;
  int *play_counts;
  double *upper_bounds;
  // The plays by standardized static equity, highest first.
  RlamcEquityRank *equity_order;
  // For a paired search (RlamcLevel.paired): each play's index among the
  // evaluated plays, or -1; the evaluated plays' values by seed,
  // seed_capacity a play; and per seed, its effect and its plays.
  int *eval_index;
  double *seed_values;
  int seed_capacity;
  double *seed_effects;
  int *seed_plays;
  int pool[RLAMC_MAX_UNIVERSE];
  int evaluated[RLAMC_MAX_EVALUATED];
  int num_evaluated;
  double gp_points[RLAMC_MAX_EVALUATED * RLAMC_MAX_COLUMNS];
  double gp_values[RLAMC_MAX_EVALUATED];
  double gp_noise[RLAMC_MAX_EVALUATED];
  // The base length scales of each fit, the same in every column.
  double inverse_length_scales[RLAMC_MAX_COLUMNS];
  double mean_intercept;
  double mean_slope;
  GaussianProcess *gp;
  int gp_num_dims;
};

static _Atomic uint64_t rlamc_total_searches;
static _Atomic uint64_t rlamc_total_inner_searches;
static _Atomic uint64_t rlamc_total_samples;
static _Atomic uint64_t rlamc_total_candidates;
static _Atomic uint64_t rlamc_total_switched;
static _Atomic uint64_t rlamc_total_cache_hits;
static _Atomic uint64_t rlamc_total_cache_misses;
static _Atomic uint64_t rlamc_total_decision_hits;
static _Atomic uint64_t rlamc_total_stopped;
static _Atomic uint64_t rlamc_total_finishes;
static _Atomic uint64_t rlamc_total_finish_hits;
static _Atomic uint64_t rlamc_total_static_rollouts;
static _Atomic uint64_t rlamc_total_rollout_hits;

void rlamc_totals_reset(void) {
  atomic_store(&rlamc_total_searches, 0);
  atomic_store(&rlamc_total_inner_searches, 0);
  atomic_store(&rlamc_total_samples, 0);
  atomic_store(&rlamc_total_candidates, 0);
  atomic_store(&rlamc_total_switched, 0);
  atomic_store(&rlamc_total_cache_hits, 0);
  atomic_store(&rlamc_total_cache_misses, 0);
  atomic_store(&rlamc_total_decision_hits, 0);
  atomic_store(&rlamc_total_stopped, 0);
  atomic_store(&rlamc_total_finishes, 0);
  atomic_store(&rlamc_total_finish_hits, 0);
  atomic_store(&rlamc_total_static_rollouts, 0);
  atomic_store(&rlamc_total_rollout_hits, 0);
}

RlamcTotals rlamc_get_totals(void) {
  RlamcTotals totals = {
      .searches = atomic_load(&rlamc_total_searches),
      .inner_searches = atomic_load(&rlamc_total_inner_searches),
      .samples = atomic_load(&rlamc_total_samples),
      .candidates = atomic_load(&rlamc_total_candidates),
      .switched = atomic_load(&rlamc_total_switched),
      .cache_hits = atomic_load(&rlamc_total_cache_hits),
      .cache_misses = atomic_load(&rlamc_total_cache_misses),
      .decision_hits = atomic_load(&rlamc_total_decision_hits),
      .stopped = atomic_load(&rlamc_total_stopped),
      .finishes = atomic_load(&rlamc_total_finishes),
      .finish_hits = atomic_load(&rlamc_total_finish_hits),
      .static_rollouts = atomic_load(&rlamc_total_static_rollouts),
      .rollout_hits = atomic_load(&rlamc_total_rollout_hits),
  };
  return totals;
}

static RlamcWorker *rlamc_worker_create_level(const Game *game,
                                              CrossSetCache *cross_set_cache,
                                              const int level) {
  RlamcWorker *worker = malloc_or_die(sizeof(RlamcWorker));
  worker->level = level;
  worker->spec = NULL;
  worker->inner = NULL;
  worker->cross_set_cache = cross_set_cache;
  worker->game = game_duplicate(game);
  game_set_backup_mode(worker->game, BACKUP_MODE_SIMULATION);
  game_enable_rollout_board_undo(worker->game);
  game_set_cross_set_cache(worker->game, cross_set_cache);
  worker->candidate_list = NULL;
  worker->candidate_capacity = 0;
  worker->num_candidates = 0;
  worker->reply_list = move_list_create(1);
  worker->values = NULL;
  worker->values_capacity = 0;
  worker->sim_seed = 0;
  worker->cache_keys = NULL;
  worker->cache_replies = NULL;
  worker->cache_samples = 0;
  worker->game_is_current = false;
  worker->census_worker = NULL;
  worker->census = NULL;
  worker->reply_ply = 0;
  worker->decision_keys = NULL;
  worker->decision_moves = NULL;
  worker->features_ready = false;
  worker->embedding = NULL;
  worker->embedding_capacity = 0;
  worker->min_distance = NULL;
  worker->picked = NULL;
  worker->play_capacity = 0;
  worker->static_equity = NULL;
  worker->play_sums = NULL;
  worker->play_squares = NULL;
  worker->play_counts = NULL;
  worker->upper_bounds = NULL;
  worker->equity_order = NULL;
  worker->eval_index = NULL;
  worker->seed_values = NULL;
  worker->seed_capacity = 0;
  worker->seed_effects = NULL;
  worker->seed_plays = NULL;
  worker->finish_keys = NULL;
  worker->finish_points = NULL;
  worker->rollout_keys = NULL;
  worker->rollout_values = NULL;
  worker->num_evaluated = 0;
  worker->gp = NULL;
  worker->gp_num_dims = 0;
  return worker;
}

RlamcWorker *rlamc_worker_create(const Game *game,
                                 CrossSetCache *cross_set_cache) {
  return rlamc_worker_create_level(game, cross_set_cache, 0);
}

void rlamc_worker_set_census(RlamcWorker *worker,
                             CensusRolloutWorker *census_worker,
                             const CensusRollout *census, const int reply_ply) {
  worker->census_worker = census_worker;
  worker->census = census;
  worker->reply_ply = reply_ply;
}

// The top static play of the player on turn in game, steered by the census
// when the sim has one: the same play, found faster.
static const Move *rlamc_top_play(RlamcWorker *worker, Game *game,
                                  const int ply, BestMoveCarry *carry) {
  if (worker->census != NULL) {
    return census_rollout_get_best_move(worker->census_worker, worker->census,
                                        game, worker->reply_list, ply);
  }
  return get_top_equity_move_with_carry(game, worker->reply_list, carry);
}

void rlamc_worker_reset(RlamcWorker *worker, const uint64_t sim_seed) {
  worker->sim_seed = sim_seed;
  if (worker->inner != NULL) {
    rlamc_worker_reset(worker->inner, sim_seed);
  }
  if (worker->cache_keys != NULL) {
    memset(worker->cache_keys, 0,
           sizeof(uint64_t) * ((size_t)1 << RLAMC_CACHE_BITS));
  }
  if (worker->decision_keys != NULL) {
    memset(worker->decision_keys, 0,
           sizeof(uint64_t) * ((size_t)1 << RLAMC_CACHE_BITS));
  }
  // A finish's plays and a rollout's utility depend on the players' settings
  // and the sim's, which another sim may change.
  if (worker->finish_keys != NULL) {
    memset(worker->finish_keys, 0,
           sizeof(uint64_t) * ((size_t)1 << RLAMC_FINISH_CACHE_BITS));
  }
  if (worker->rollout_keys != NULL) {
    memset(worker->rollout_keys, 0,
           sizeof(uint64_t) * ((size_t)1 << RLAMC_ROLLOUT_CACHE_BITS));
  }
}

void rlamc_worker_destroy(RlamcWorker *worker) {
  if (!worker) {
    return;
  }
  rlamc_worker_destroy(worker->inner);
  game_destroy(worker->game);
  move_list_destroy(worker->candidate_list);
  move_list_destroy(worker->reply_list);
  free(worker->values);
  free(worker->cache_keys);
  free(worker->cache_replies);
  free(worker->decision_keys);
  free(worker->decision_moves);
  free(worker->finish_keys);
  free(worker->finish_points);
  free(worker->rollout_keys);
  free(worker->rollout_values);
  free(worker->embedding);
  free(worker->min_distance);
  free(worker->picked);
  free(worker->static_equity);
  free(worker->play_sums);
  free(worker->play_squares);
  free(worker->play_counts);
  free(worker->upper_bounds);
  free(worker->equity_order);
  free(worker->eval_index);
  free(worker->seed_values);
  free(worker->seed_effects);
  free(worker->seed_plays);
  gp_destroy(worker->gp);
  free(worker);
}

static uint64_t rlamc_mix(uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

// Generates the candidates of the player on turn in worker's game: up to
// num_candidates plays by static equity, within margin points of the best.
// Embeds the worker's generated plays (worker->candidate_list) in the
// Phase 1 play embedding (see play_feature_registry.h), every continuous
// column standardized across them, and their static equity, standardized,
// in worker->static_equity. Returns the number of columns.
static int rlamc_embed(RlamcWorker *worker, const Game *game) {
  const MoveList *plays = worker->candidate_list;
  const int count = move_list_get_count(plays);
  if (!worker->features_ready) {
    ErrorStack *error_stack = error_stack_create();
    play_feature_parse_set(PLAY_FEATURE_DEFAULT_SET, worker->features,
                           error_stack);
    if (!error_stack_is_empty(error_stack)) {
      log_fatal("the default play feature set does not parse");
    }
    error_stack_destroy(error_stack);
    worker->features_ready = true;
  }
  int num_columns = 0;
  for (int feature_idx = 0; feature_idx < NUMBER_OF_PLAY_FEATURES;
       feature_idx++) {
    if (worker->features[feature_idx]) {
      num_columns += play_feature_get_num_columns((play_feature_t)feature_idx);
    }
  }
  if (worker->embedding_capacity < count * num_columns ||
      worker->play_capacity < count) {
    free(worker->embedding);
    free(worker->min_distance);
    free(worker->picked);
    free(worker->static_equity);
    free(worker->play_sums);
    free(worker->play_squares);
    free(worker->play_counts);
    free(worker->upper_bounds);
    free(worker->equity_order);
    free(worker->eval_index);
    worker->embedding_capacity = count * num_columns;
    worker->play_capacity = count;
    worker->embedding =
        malloc_or_die(sizeof(double) * (size_t)worker->embedding_capacity);
    worker->min_distance = malloc_or_die(sizeof(double) * (size_t)count);
    worker->picked = malloc_or_die(sizeof(bool) * (size_t)count);
    worker->static_equity = malloc_or_die(sizeof(double) * (size_t)count);
    worker->play_sums = malloc_or_die(sizeof(double) * (size_t)count);
    worker->play_squares = malloc_or_die(sizeof(double) * (size_t)count);
    worker->play_counts = malloc_or_die(sizeof(int) * (size_t)count);
    worker->upper_bounds = malloc_or_die(sizeof(double) * (size_t)count);
    worker->equity_order =
        malloc_or_die(sizeof(RlamcEquityRank) * (size_t)count);
    worker->eval_index = malloc_or_die(sizeof(int) * (size_t)count);
  }
  double *embedding = worker->embedding;
  play_feature_ctx_init(&worker->feature_ctx, game);
  int column = 0;
  for (int feature_idx = 0; feature_idx < NUMBER_OF_PLAY_FEATURES;
       feature_idx++) {
    if (!worker->features[feature_idx]) {
      continue;
    }
    const play_feature_t feature = (play_feature_t)feature_idx;
    play_feature_prepare(feature, &worker->feature_ctx);
    play_feature_extract(feature, &worker->feature_ctx, plays, num_columns,
                         embedding + column);
    const int feature_columns = play_feature_get_num_columns(feature);
    if (play_feature_get_kind(feature) == PLAY_FEATURE_KIND_CONTINUOUS) {
      for (int offset = 0; offset < feature_columns; offset++) {
        rlamc_standardize(embedding + column + offset, num_columns, count);
      }
    }
    column += feature_columns;
  }
  play_feature_extract(PLAY_FEATURE_EQUITY, &worker->feature_ctx, plays, 1,
                       worker->static_equity);
  rlamc_standardize(worker->static_equity, 1, count);
  return num_columns;
}

static double rlamc_distance_squared(const double *a, const double *b,
                                     const int num_columns) {
  double distance = 0.0;
  for (int col = 0; col < num_columns; col++) {
    const double diff = a[col] - b[col];
    distance += diff * diff;
  }
  return distance;
}

// Picks wanted plays of the embedded worker->candidate_list into
// worker->pool: the first num_seeds (the best by static equity), then each
// time the play farthest from those already picked.
static int rlamc_farthest_points(RlamcWorker *worker, const int num_columns,
                                 const int wanted, int num_seeds) {
  const int count = move_list_get_count(worker->candidate_list);
  const double *embedding = worker->embedding;
  if (num_seeds < 1) {
    num_seeds = 1;
  }
  for (int play_idx = 0; play_idx < count; play_idx++) {
    worker->min_distance[play_idx] = INFINITY;
    worker->picked[play_idx] = false;
  }
  int picks = 0;
  for (int pick = 0; pick < wanted && pick < count; pick++) {
    int chosen = pick;
    if (pick >= num_seeds) {
      double farthest = -1.0;
      for (int play_idx = 0; play_idx < count; play_idx++) {
        if (!worker->picked[play_idx] &&
            worker->min_distance[play_idx] > farthest) {
          farthest = worker->min_distance[play_idx];
          chosen = play_idx;
        }
      }
    }
    worker->picked[chosen] = true;
    worker->pool[picks++] = chosen;
    const double *chosen_row = embedding + (ptrdiff_t)chosen * num_columns;
    for (int play_idx = 0; play_idx < count; play_idx++) {
      const double distance =
          rlamc_distance_squared(embedding + (ptrdiff_t)play_idx * num_columns,
                                 chosen_row, num_columns);
      if (distance < worker->min_distance[play_idx]) {
        worker->min_distance[play_idx] = distance;
      }
    }
  }
  return picks;
}

// Picks the worker's candidates from its generated plays by the play
// embedding: the best static_seeds plays, then plays spread over the
// embedding by farthest-point sampling, until there are num_candidates.
static void rlamc_pick_diverse(RlamcWorker *worker, const RlamcLevel *args,
                               const Game *game) {
  const int num_columns = rlamc_embed(worker, game);
  const int picks = rlamc_farthest_points(
      worker, num_columns, args->num_candidates, args->static_seeds);
  worker->num_candidates = 0;
  for (int pick = 0; pick < picks; pick++) {
    move_copy(&worker->candidates[worker->num_candidates++],
              move_list_get_move(worker->candidate_list, worker->pool[pick]));
  }
}

// Generates the candidates of the player on turn in game: up to
// num_candidates plays by static equity, within margin points of the best,
// or with a universe, a diverse pick from that many (rlamc_pick_diverse).
static void rlamc_generate_candidates(RlamcWorker *worker,
                                      const RlamcLevel *args,
                                      const Game *game) {
  int capacity = args->num_candidates;
  if (args->lamc_search) {
    capacity = args->universe > 0 ? args->universe : RLAMC_MAX_UNIVERSE;
  } else if (args->universe > 0) {
    capacity = args->universe;
  }
  if (worker->candidate_capacity != capacity) {
    move_list_destroy(worker->candidate_list);
    worker->candidate_list = move_list_create(capacity);
    worker->candidate_capacity = capacity;
  }
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = worker->candidate_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&gen_args);
  move_list_sort_moves(worker->candidate_list);
  const int count = move_list_get_count(worker->candidate_list);
  worker->num_candidates = 0;
  if (count == 0) {
    return;
  }
  if (args->lamc_search) {
    // rlamc_choose_lamc searches the plays as generated.
    return;
  }
  if (args->universe > 0) {
    rlamc_pick_diverse(worker, args, game);
    return;
  }
  const Equity best =
      move_get_equity(move_list_get_move(worker->candidate_list, 0));
  const Equity floor = best - int_to_equity(args->margin);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const Move *move = move_list_get_move(worker->candidate_list, move_idx);
    if (move_idx > 0 && move_get_equity(move) < floor) {
      break;
    }
    move_copy(&worker->candidates[worker->num_candidates++], move);
  }
}

// The utility for player mover of the recursive rollout's last state, as the
// sim scores a rollout's horizon (see sim_rollout). The utility of a game that
// has ended with spread for the mover: its result and final spread, with no
// leftover, as the sim scores an ended game, wherever it ended.
static double rlamc_ended_score(const RlamcEval *eval, const Equity spread) {
  double win_pct;
  if (spread == 0) {
    win_pct = 0.5;
  } else {
    win_pct = spread > 0 ? 1.0 : 0.0;
  }
  return sim_utility_blend(win_pct, spread, eval->utility_w_winpct,
                           eval->utility_w_spread, eval->utility_spread_scale);
}

static double rlamc_score(const RlamcEval *eval, const Game *game,
                          const int mover, const Equity leftover) {
  const Equity spread = player_get_score(game_get_player(game, mover)) -
                        player_get_score(game_get_player(game, 1 - mover));
  if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
    return rlamc_ended_score(eval, spread);
  }
  const int on_turn = game_get_player_on_turn_index(game);
  int spread_plus_leftover = (int)lround(equity_to_double(spread + leftover));
  if (on_turn != mover) {
    spread_plus_leftover = -spread_plus_leftover;
  }
  const int bag_tiles = bag_get_letters(game_get_bag(game));
  const int on_turn_rack =
      rack_get_total_letters(player_get_rack(game_get_player(game, on_turn)));
  const int off_turn_rack = rack_get_total_letters(
      player_get_rack(game_get_player(game, 1 - on_turn)));
  double win_pct = (double)win_pct_get(
      eval->win_pcts, spread_plus_leftover, (unsigned int)bag_tiles,
      (unsigned int)on_turn_rack, (unsigned int)off_turn_rack);
  if (on_turn != mover) {
    win_pct = 1.0 - win_pct;
  }
  return sim_utility_blend(win_pct, spread + leftover, eval->utility_w_winpct,
                           eval->utility_w_spread, eval->utility_spread_scale);
}

static const Move *rlamc_choose_level(RlamcWorker *worker,
                                      const RlamcEval *eval, const Game *game,
                                      uint64_t seed, BestMoveCarry *carry);

// The play at ply of a recursive rollout of worker's search: a search of the
// next level if there is one and it names ply, else the top static play.
static const Move *rlamc_ply_play(RlamcWorker *worker, const RlamcEval *eval,
                                  Game *game, const int ply,
                                  const uint64_t sample_seed,
                                  BestMoveCarry *carry) {
  const RlamcArgs *spec = worker->spec;
  const int inner_level = worker->level + 1;
  if (inner_level < spec->depth &&
      rlamc_level_applies(&spec->levels[inner_level], ply,
                          bag_get_letters(game_get_bag(game)))) {
    if (worker->inner == NULL) {
      worker->inner =
          rlamc_worker_create_level(game, worker->cross_set_cache, inner_level);
      worker->inner->sim_seed = worker->sim_seed;
    }
    RlamcWorker *inner = worker->inner;
    inner->spec = spec;
    inner->census_worker = worker->census_worker;
    inner->census = worker->census;
    inner->reply_ply = worker->reply_ply + ply + 1;
    return rlamc_choose_level(
        inner, eval, game,
        rlamc_mix(sample_seed ^ ((uint64_t)(ply + 1) * 0xA24BAED4963EE407ULL)),
        carry);
  }
  return rlamc_top_play(worker, game, worker->reply_ply + ply, carry);
}

// A hash of a square holding a letter.
static uint64_t rlamc_square_hash(const int row, const int col,
                                  const MachineLetter letter) {
  return rlamc_mix(0xC2B2AE3D27D4EB4FULL ^
                   ((uint64_t)((row * BOARD_DIM + col) * 256 + letter)));
}

static uint64_t rlamc_board_hash(const Board *board) {
  uint64_t hash = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter letter = board_get_letter(board, row, col);
      if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
        hash ^= rlamc_square_hash(row, col, letter);
      }
    }
  }
  return hash;
}

// The rotations of a bag-cycled LAMC search (RlamcLevel.bag_cycle) in game:
// how many racks of RACK_SIZE letters it takes for every letter unseen by
// the player on turn to start on the other player's rack once.
static int rlamc_cycle_rotations(const Game *game) {
  const int unseen = bag_get_letters(game_get_bag(game)) +
                     rack_get_total_letters(player_get_rack(game_get_player(
                         game, 1 - game_get_player_on_turn_index(game))));
  return unseen > RACK_SIZE ? (unseen + RACK_SIZE - 1) / RACK_SIZE : 1;
}

// The hash of the board move leaves, from the hash of the board it is
// played on. Exchanges and passes leave the board as it is.
static uint64_t rlamc_board_hash_after(const uint64_t board_hash,
                                       const Move *move) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return board_hash;
  }
  uint64_t hash = board_hash;
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const bool vertical = move_get_dir(move) == BOARD_VERTICAL_DIRECTION;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter letter = move_get_tile(move, tile_idx);
    if (letter == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = vertical ? row_start + tile_idx : row_start;
    const int col = vertical ? col_start : col_start + tile_idx;
    hash ^= rlamc_square_hash(row, col, letter);
  }
  return hash;
}

// The key of a finish from game, whose bag is empty and whose board hashes
// to board_hash (see rlamc_board_hash): its board, the rack of
// the player on turn, which player that is and the scoreless turns. The other
// rack is the tiles unseen by the player on turn. The static plays to the end
// depend on nothing else, and a finish's points are kept apart from the
// scores, so the spread is no part of the key.
static uint64_t rlamc_finish_key(const Game *game, const uint64_t board_hash) {
  const int on_turn = game_get_player_on_turn_index(game);
  uint64_t key = rlamc_mix(board_hash ^ 0x94D049BB133111EBULL);
  const Rack *rack = player_get_rack(game_get_player(game, on_turn));
  for (int letter = 0; letter < rack_get_dist_size(rack); letter++) {
    key = rlamc_mix(key ^ ((uint64_t)letter << 8) ^
                    (uint64_t)rack_get_letter(rack, (MachineLetter)letter));
  }
  key = rlamc_mix(key ^ ((uint64_t)on_turn << 40) ^
                  ((uint64_t)game_get_consecutive_scoreless_turns(game) << 48));
  return key | 1ULL;
}

// Plays game on by static plays to its end, or for at most
// RLAMC_FINISH_MAX_PLIES plies, as the sim's rollouts do (see
// SimArgs.finish_endgame). Returns whether the game ended and, if it did,
// sets *spread to the mover's final spread. A finish from an empty bag is a
// function of rlamc_finish_key, so the points each player scored in one
// that ended are kept in the worker's finish cache, and a finish found there
// is not played: game is then left where the finish began. (Keeping the
// positions a finish passes through as well finds no more.)
static bool rlamc_finish(RlamcWorker *worker, Game *game, const int mover,
                         BestMoveCarry *carry, Equity *spread) {
  const int on_turn = game_get_player_on_turn_index(game);
  const Equity on_turn_score = player_get_score(game_get_player(game, on_turn));
  const Equity off_turn_score =
      player_get_score(game_get_player(game, 1 - on_turn));
  const bool cacheable = bag_get_letters(game_get_bag(game)) == 0;
  uint64_t key = 0;
  uint64_t slot = 0;
  if (cacheable) {
    if (worker->finish_keys == NULL) {
      worker->finish_keys =
          calloc_or_die((size_t)1 << RLAMC_FINISH_CACHE_BITS, sizeof(uint64_t));
      worker->finish_points = malloc_or_die(
          sizeof(Equity) * 2 * ((size_t)1 << RLAMC_FINISH_CACHE_BITS));
    }
    atomic_fetch_add_explicit(&rlamc_total_finishes, 1, memory_order_relaxed);
    key = rlamc_finish_key(game, rlamc_board_hash(game_get_board(game)));
    slot = key & (((uint64_t)1 << RLAMC_FINISH_CACHE_BITS) - 1);
    if (worker->finish_keys[slot] == key) {
      atomic_fetch_add_explicit(&rlamc_total_finish_hits, 1,
                                memory_order_relaxed);
      const Equity on_turn_points = worker->finish_points[2 * slot];
      const Equity off_turn_points = worker->finish_points[2 * slot + 1];
      const Equity on_turn_spread =
          on_turn_score + on_turn_points - off_turn_score - off_turn_points;
      *spread = on_turn == mover ? on_turn_spread : -on_turn_spread;
      return true;
    }
  }
  for (int extra = 0; extra < RLAMC_FINISH_MAX_PLIES && !game_over(game);
       extra++) {
    play_move(rlamc_top_play(worker, game, RLAMC_FINISH_PLY, carry), game,
              NULL);
  }
  if (!game_over(game)) {
    return false;
  }
  if (cacheable) {
    worker->finish_keys[slot] = key;
    worker->finish_points[2 * slot] =
        player_get_score(game_get_player(game, on_turn)) - on_turn_score;
    worker->finish_points[2 * slot + 1] =
        player_get_score(game_get_player(game, 1 - on_turn)) - off_turn_score;
  }
  *spread = player_get_score(game_get_player(game, mover)) -
            player_get_score(game_get_player(game, 1 - mover));
  return true;
}

// Whether, with an empty bag, every ply of a rollout of worker's search is
// the top static play: no deeper level names any of them.
static bool rlamc_horizon_is_static(const RlamcWorker *worker,
                                    const RlamcLevel *args) {
  const RlamcArgs *spec = worker->spec;
  const int inner_level = worker->level + 1;
  if (inner_level >= spec->depth) {
    return true;
  }
  for (int ply = 0; ply < args->horizon_plies; ply++) {
    if (rlamc_level_applies(&spec->levels[inner_level], ply, 0)) {
      return false;
    }
  }
  return true;
}

// The key of the rest of a rollout from game, just after its candidate, when
// the bag is empty and every play left is static (with -sfinish, to the end):
// the finish's key (see rlamc_finish_key), which fixes the plays, with the
// mover's spread and the candidate's leave value, which with them fix the
// rollout's utility.
static uint64_t rlamc_rollout_key(const Game *game, const int mover,
                                  const Equity candidate_leave) {
  const Equity spread = player_get_score(game_get_player(game, mover)) -
                        player_get_score(game_get_player(game, 1 - mover));
  uint64_t key =
      rlamc_mix(rlamc_finish_key(game, rlamc_board_hash(game_get_board(game))) ^
                0xBF58476D1CE4E5B9ULL);
  key = rlamc_mix(key ^ (uint64_t)(int64_t)spread);
  key = rlamc_mix(key ^ ((uint64_t)(int64_t)candidate_leave << 1));
  return key | 1ULL;
}

// One recursive rollout of candidate from worker's game, whose other player's
// rack is drawn again from sample_seed's shuffle, rotated left by rotation
// racks (see bag_rotate). Returns its utility for the player on turn, and
// leaves the game as it found it, apart from the other player's rack and the
// order of the bag.
static double rlamc_rollout(RlamcWorker *worker, const RlamcLevel *args,
                            const RlamcEval *eval, const Move *candidate,
                            const uint64_t sample_seed, const int rotation,
                            BestMoveCarry *carry) {
  Game *game = worker->game;
  const bool finish = eval->finish_endgame && !args->skip_finish;
  const int mover = game_get_player_on_turn_index(game);
  const int other = 1 - mover;
  return_rack_to_bag(game, other);
  game_seed(game, sample_seed);
  if (rotation > 0) {
    bag_rotate(game_get_bag(game), rotation * RACK_SIZE);
  }
  draw_to_full_rack(game, other);

  // The plays of the rollout, and the leave each one kept, for the leftover
  // of the last two.
  Equity leaves[RLAMC_MAX_PLIES + 1];
  int players[RLAMC_MAX_PLIES + 1];
  int num_plays = 0;

  Rack rack_before;
  const Player *mover_player = game_get_player(game, mover);
  rack_copy(&rack_before, player_get_rack(mover_player));
  game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
  play_move(candidate, game, NULL);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  leaves[num_plays] = get_leave_value_for_move(player_get_klv(mover_player),
                                               candidate, &rack_before);
  players[num_plays++] = mover;

  // From an empty bag, the rest of a rollout whose plays are all static is a
  // function of rlamc_rollout_key, and its utility is kept in the rollout
  // cache.
  const bool cacheable = finish && !game_over(game) &&
                         bag_get_letters(game_get_bag(game)) == 0 &&
                         rlamc_horizon_is_static(worker, args);
  uint64_t rollout_key = 0;
  uint64_t rollout_slot = 0;
  if (cacheable) {
    if (worker->rollout_keys == NULL) {
      worker->rollout_keys = calloc_or_die(
          (size_t)1 << RLAMC_ROLLOUT_CACHE_BITS, sizeof(uint64_t));
      worker->rollout_values = malloc_or_die(
          sizeof(double) * ((size_t)1 << RLAMC_ROLLOUT_CACHE_BITS));
    }
    atomic_fetch_add_explicit(&rlamc_total_static_rollouts, 1,
                              memory_order_relaxed);
    rollout_key = rlamc_rollout_key(game, mover, leaves[0]);
    rollout_slot =
        rollout_key & (((uint64_t)1 << RLAMC_ROLLOUT_CACHE_BITS) - 1);
    if (worker->rollout_keys[rollout_slot] == rollout_key) {
      atomic_fetch_add_explicit(&rlamc_total_rollout_hits, 1,
                                memory_order_relaxed);
      const double cached = worker->rollout_values[rollout_slot];
      game_unplay_last_move(game);
      return cached;
    }
  }

  for (int ply = 0; ply < args->horizon_plies; ply++) {
    if (game_over(game)) {
      break;
    }
    const int on_turn = game_get_player_on_turn_index(game);
    const Player *on_turn_player = game_get_player(game, on_turn);
    const Move *play =
        rlamc_ply_play(worker, eval, game, ply, sample_seed, carry);
    rack_copy(&rack_before, player_get_rack(on_turn_player));
    if (ply == args->horizon_plies - 1 && !finish) {
      play_move_no_cross_set_update(play, game, NULL);
    } else {
      play_move(play, game, NULL);
    }
    leaves[num_plays] = get_leave_value_for_move(player_get_klv(on_turn_player),
                                                 play, &rack_before);
    players[num_plays++] = on_turn;
  }
  Equity leftover = 0;
  for (int play_idx = num_plays >= 2 ? num_plays - 2 : 0; play_idx < num_plays;
       play_idx++) {
    leftover +=
        players[play_idx] == mover ? leaves[play_idx] : -leaves[play_idx];
  }
  // As the sim's rollouts do (see SimArgs.finish_endgame): play on to the
  // end when the bag is about empty, and score the result.
  double utility = 0.0;
  Equity final_spread = 0;
  if (finish && !game_over(game) &&
      bag_get_letters(game_get_bag(game)) <= eval->finish_max_bag &&
      rlamc_finish(worker, game, mover, carry, &final_spread)) {
    utility = rlamc_ended_score(eval, final_spread);
  } else {
    utility = rlamc_score(eval, game, mover, leftover);
  }
  if (cacheable) {
    worker->rollout_keys[rollout_slot] = rollout_key;
    worker->rollout_values[rollout_slot] = utility;
  }
  game_unplay_last_move(game);
  return utility;
}

// Whether candidate cand_idx trails the leader by more than z standard errors
// of their paired difference over the rounds played.
static bool rlamc_trails(const RlamcWorker *worker, const int cand_idx,
                         const int leader_idx, const int rounds,
                         const double z) {
  const double *values = worker->values;
  double sum = 0.0;
  double sum_sq = 0.0;
  for (int round = 0; round < rounds; round++) {
    const double diff = values[round * RLAMC_MAX_CANDIDATES + cand_idx] -
                        values[round * RLAMC_MAX_CANDIDATES + leader_idx];
    sum += diff;
    sum_sq += diff * diff;
  }
  const double mean = sum / rounds;
  double variance = (sum_sq - sum * mean) / (rounds - 1);
  if (variance < 0.0) {
    variance = 0.0;
  }
  const double se = sqrt(variance / rounds);
  return mean + z * se < 0.0;
}

// Chooses among the worker's candidates by plain recursive rollouts, from the
// position in the worker's game.
static const Move *rlamc_choose_plain(RlamcWorker *worker,
                                      const RlamcLevel *args,
                                      const RlamcEval *eval,
                                      const uint64_t seed,
                                      BestMoveCarry *carry) {
  const int num_candidates = worker->num_candidates;
  const int rounds_capacity = args->num_samples * RLAMC_MAX_CANDIDATES;
  if (worker->values_capacity < rounds_capacity) {
    free(worker->values);
    worker->values = malloc_or_die(sizeof(double) * (size_t)rounds_capacity);
    worker->values_capacity = rounds_capacity;
  }
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    worker->alive[cand_idx] = true;
    worker->sums[cand_idx] = 0.0;
    worker->counts[cand_idx] = 0;
  }
  int num_alive = num_candidates;
  uint64_t samples = 0;
  for (int round = 0; round < args->num_samples && num_alive > 1; round++) {
    const uint64_t sample_seed = rlamc_mix(seed + (uint64_t)round);
    for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
      if (!worker->alive[cand_idx]) {
        continue;
      }
      const double value =
          rlamc_rollout(worker, args, eval, &worker->candidates[cand_idx],
                        sample_seed, 0, carry);
      worker->values[round * RLAMC_MAX_CANDIDATES + cand_idx] = value;
      worker->sums[cand_idx] += value;
      worker->counts[cand_idx]++;
      samples++;
    }
    if (args->alloc == RLAMC_ALLOC_RACE && round + 1 >= RLAMC_RACE_MIN_ROUNDS) {
      int leader = -1;
      for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
        if (worker->alive[cand_idx] &&
            (leader < 0 || worker->sums[cand_idx] > worker->sums[leader])) {
          leader = cand_idx;
        }
      }
      for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
        if (cand_idx != leader && worker->alive[cand_idx] &&
            rlamc_trails(worker, cand_idx, leader, round + 1, args->race_z)) {
          worker->alive[cand_idx] = false;
          num_alive--;
        }
      }
    }
  }
  atomic_fetch_add_explicit(&rlamc_total_samples, samples,
                            memory_order_relaxed);
  // Every candidate alive at the end has the same count; the others dropped
  // out behind one of them.
  int best = -1;
  double best_value = 0.0;
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    if (!worker->alive[cand_idx]) {
      continue;
    }
    double value = worker->sums[cand_idx] / worker->counts[cand_idx];
    if (cand_idx != 0) {
      value -= args->switch_margin;
    }
    if (best < 0 || value > best_value) {
      best = cand_idx;
      best_value = value;
    }
  }
  if (best != 0) {
    atomic_fetch_add_explicit(&rlamc_total_switched, 1, memory_order_relaxed);
  }
  move_copy(&worker->chosen, &worker->candidates[best]);
  return &worker->chosen;
}

// The tiles a placement takes from the bag's count: the tiles it places. An
// exchange or a pass leaves the bag's count as it is.
static int rlamc_tiles_drawn(const Move *move) {
  return move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE
             ? move_get_tiles_played(move)
             : 0;
}

typedef struct RlamcTilePriority {
  uint64_t priority;
  MachineLetter letter;
} RlamcTilePriority;

static int rlamc_compare_priorities(const void *a, const void *b) {
  const uint64_t pa = ((const RlamcTilePriority *)a)->priority;
  const uint64_t pb = ((const RlamcTilePriority *)b)->priority;
  if (pa < pb) {
    return -1;
  }
  return pa > pb ? 1 : 0;
}

// Fills the replies of the board in the worker's game, whose player on turn
// is the replier and whose bag and racks hold every tile not on the board:
// for each sample, the replier's rack and the other player's are the tiles of
// lowest priority for that sample, and the replier plays its best play.
static void rlamc_fill_replies(RlamcWorker *worker, const RlamcLevel *args,
                               RlamcReply *replies, BestMoveCarry *carry) {
  Game *game = worker->game;
  const int replier = game_get_player_on_turn_index(game);
  const int other = 1 - replier;
  return_rack_to_bag(game, replier);
  return_rack_to_bag(game, other);
  const Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int dist_size = ld_get_size(ld);
  RlamcTilePriority pool[RLAMC_MAX_POOL];
  const Player *replier_player = game_get_player(game, replier);
  for (int sample = 0; sample < args->num_samples; sample++) {
    int pool_size = 0;
    for (int letter = 0; letter < dist_size; letter++) {
      const int count = bag_get_letter(bag, (MachineLetter)letter);
      for (int copy = 0; copy < count && pool_size < RLAMC_MAX_POOL; copy++) {
        pool[pool_size].priority =
            rlamc_mix(worker->sim_seed ^ ((uint64_t)sample << 32) ^
                      ((uint64_t)letter << 12) ^ (uint64_t)copy);
        pool[pool_size].letter = (MachineLetter)letter;
        pool_size++;
      }
    }
    qsort(pool, (size_t)pool_size, sizeof(RlamcTilePriority),
          rlamc_compare_priorities);
    Rack replier_rack;
    Rack other_rack;
    rack_set_dist_size_and_reset(&replier_rack, dist_size);
    rack_set_dist_size_and_reset(&other_rack, dist_size);
    int taken = 0;
    for (; taken < pool_size && taken < RACK_SIZE; taken++) {
      rack_add_letter(&replier_rack, pool[taken].letter);
    }
    for (; taken < pool_size && taken < 2 * RACK_SIZE; taken++) {
      rack_add_letter(&other_rack, pool[taken].letter);
    }
    draw_rack_from_bag(game, replier, &replier_rack);
    draw_rack_from_bag(game, other, &other_rack);
    const Move *reply = rlamc_top_play(worker, game, worker->reply_ply, carry);
    Rack rack_before;
    rack_copy(&rack_before, player_get_rack(replier_player));
    const Equity leave = get_leave_value_for_move(
        player_get_klv(replier_player), reply, &rack_before);
    const int bag_tiles = bag_get_letters(bag);
    int drawn = rlamc_tiles_drawn(reply);
    if (drawn > bag_tiles) {
      drawn = bag_tiles;
    }
    replies[sample].equity = move_get_score(reply) + leave;
    replies[sample].bag_after = (uint8_t)(bag_tiles - drawn);
    replies[sample].replier_rack_after =
        (uint8_t)(rack_get_total_letters(&rack_before) + drawn);
    return_rack_to_bag(game, replier);
    return_rack_to_bag(game, other);
  }
}

// The cached replies to the board key names, which the worker's candidate
// leaves, computed and cached if they are not there yet.
static const RlamcReply *
rlamc_get_replies(RlamcWorker *worker, const RlamcLevel *args, const Game *game,
                  const Move *candidate, const uint64_t key,
                  BestMoveCarry *carry) {
  const uint64_t mask = ((uint64_t)1 << RLAMC_CACHE_BITS) - 1;
  uint64_t slot = key & mask;
  uint64_t free_slot = UINT64_MAX;
  for (int probe = 0; probe < RLAMC_CACHE_PROBES; probe++) {
    const uint64_t probe_slot = (slot + (uint64_t)probe) & mask;
    if (worker->cache_keys[probe_slot] == key) {
      atomic_fetch_add_explicit(&rlamc_total_cache_hits, 1,
                                memory_order_relaxed);
      return &worker->cache_replies[probe_slot * (uint64_t)args->num_samples];
    }
    if (worker->cache_keys[probe_slot] == 0 && free_slot == UINT64_MAX) {
      free_slot = probe_slot;
    }
  }
  atomic_fetch_add_explicit(&rlamc_total_cache_misses, 1, memory_order_relaxed);
  // A full neighborhood overwrites its first slot.
  slot = free_slot != UINT64_MAX ? free_slot : slot;
  if (!worker->game_is_current) {
    game_copy(worker->game, game);
    worker->game_is_current = true;
  }
  Game *worker_game = worker->game;
  game_set_backup_mode(worker_game, BACKUP_MODE_SIMULATION);
  play_move(candidate, worker_game, NULL);
  game_set_backup_mode(worker_game, BACKUP_MODE_OFF);
  RlamcReply *replies =
      &worker->cache_replies[slot * (uint64_t)args->num_samples];
  rlamc_fill_replies(worker, args, replies, carry);
  game_unplay_last_move(worker_game);
  worker->cache_keys[slot] = key;
  return replies;
}

// The value for the player on turn of candidate, which scores score and keeps
// leave, against the cached replies to the board it leaves. spread is the
// player's spread before it.
static double
rlamc_value_from_replies(const RlamcLevel *args, const RlamcEval *eval,
                         const RlamcReply *replies, const Equity spread_after,
                         const Equity leave, const int mover_rack_after) {
  double sum = 0.0;
  for (int sample = 0; sample < args->num_samples; sample++) {
    const RlamcReply *reply = &replies[sample];
    const Equity spread_plus_leftover = spread_after + leave - reply->equity;
    // After the reply, the player is on turn again.
    const double win_pct = (double)win_pct_get(
        eval->win_pcts, (int)lround(equity_to_double(spread_plus_leftover)),
        (unsigned int)reply->bag_after, (unsigned int)mover_rack_after,
        (unsigned int)reply->replier_rack_after);
    sum +=
        sim_utility_blend(win_pct, spread_plus_leftover, eval->utility_w_winpct,
                          eval->utility_w_spread, eval->utility_spread_scale);
  }
  return sum / args->num_samples;
}

// Chooses among the worker's candidates, of the position in game, by shared
// recursive rollouts; a candidate that would empty the bag gets plain ones.
static const Move *rlamc_choose_shared(RlamcWorker *worker,
                                       const RlamcLevel *args,
                                       const RlamcEval *eval, const Game *game,
                                       const uint64_t seed,
                                       BestMoveCarry *carry) {
  if (worker->cache_keys == NULL ||
      worker->cache_samples != args->num_samples) {
    free(worker->cache_keys);
    free(worker->cache_replies);
    const size_t entries = (size_t)1 << RLAMC_CACHE_BITS;
    worker->cache_keys = calloc_or_die(entries, sizeof(uint64_t));
    worker->cache_replies =
        malloc_or_die(sizeof(RlamcReply) * entries * (size_t)args->num_samples);
    worker->cache_samples = args->num_samples;
  }
  const int mover = game_get_player_on_turn_index(game);
  const Player *mover_player = game_get_player(game, mover);
  const Equity spread = player_get_score(mover_player) -
                        player_get_score(game_get_player(game, 1 - mover));
  const int bag_tiles = bag_get_letters(game_get_bag(game));
  const uint64_t board_hash =
      rlamc_board_hash(game_get_board(game)) ^
      rlamc_mix(0x5851F42D4C957F2DULL + (uint64_t)(1 - mover));
  int best = 0;
  double best_value = 0.0;
  uint64_t samples = 0;
  for (int cand_idx = 0; cand_idx < worker->num_candidates; cand_idx++) {
    const Move *candidate = &worker->candidates[cand_idx];
    int drawn = rlamc_tiles_drawn(candidate);
    double value;
    if (bag_tiles - drawn < 1) {
      // The bag empties: the other player's rack is no longer hidden.
      if (!worker->game_is_current) {
        game_copy(worker->game, game);
        worker->game_is_current = true;
      }
      double sum = 0.0;
      for (int sample = 0; sample < args->num_samples; sample++) {
        sum += rlamc_rollout(worker, args, eval, candidate,
                             rlamc_mix(seed + (uint64_t)sample), 0, carry);
      }
      value = sum / args->num_samples;
      samples += (uint64_t)args->num_samples;
    } else {
      const uint64_t key = rlamc_board_hash_after(board_hash, candidate) | 1ULL;
      const RlamcReply *replies =
          rlamc_get_replies(worker, args, game, candidate, key, carry);
      Rack rack;
      rack_copy(&rack, player_get_rack(mover_player));
      const Equity leave = get_leave_value_for_move(
          player_get_klv(mover_player), candidate, &rack);
      // An exchange draws as many tiles as it returns.
      if (move_get_type(candidate) == GAME_EVENT_EXCHANGE) {
        drawn = move_get_tiles_played(candidate);
      }
      const int mover_rack_after = rack_get_total_letters(&rack) + drawn;
      value = rlamc_value_from_replies(
          args, eval, replies, spread + move_get_score(candidate), leave,
          mover_rack_after > RACK_SIZE ? RACK_SIZE : mover_rack_after);
    }
    if (cand_idx != 0) {
      value -= args->switch_margin;
    }
    if (cand_idx == 0 || value > best_value) {
      best = cand_idx;
      best_value = value;
    }
  }
  atomic_fetch_add_explicit(&rlamc_total_samples, samples,
                            memory_order_relaxed);
  if (best != 0) {
    atomic_fetch_add_explicit(&rlamc_total_switched, 1, memory_order_relaxed);
  }
  move_copy(&worker->chosen, &worker->candidates[best]);
  return &worker->chosen;
}

// The LAMC search's surrogate of the evaluated plays (RlamcLevel.lamc_search),
// as Phase 1's (lamc_fit): a prior mean linear in static equity, fit by least
// squares weighted by each mean's precision, and a Gaussian process of the
// residuals whose length scale and signal variance are chosen by marginal
// likelihood. Returns whether a fit succeeded.
// For a paired search: each evaluated play's mean and its variance, in
// worker->gp_values and worker->gp_noise, from the model value = play mean +
// seed effect + noise, fit by alternating means over the seeds that two or
// more plays share. A seed only one play drew counts as a plain sample.
// Returns false, leaving the plain estimates to be used, when too few
// samples share seeds to estimate the noise.
static bool rlamc_paired_estimates(RlamcWorker *worker,
                                   const double rollout_variance) {
  const int num_evaluated = worker->num_evaluated;
  int num_seeds = 0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int samples = worker->play_counts[worker->evaluated[eval_idx]];
    num_seeds = samples > num_seeds ? samples : num_seeds;
  }
  for (int seed_idx = 0; seed_idx < num_seeds; seed_idx++) {
    worker->seed_plays[seed_idx] = 0;
    worker->seed_effects[seed_idx] = 0.0;
  }
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int samples = worker->play_counts[worker->evaluated[eval_idx]];
    for (int seed_idx = 0; seed_idx < samples; seed_idx++) {
      worker->seed_plays[seed_idx]++;
    }
  }
  int shared_samples = 0;
  int shared_seeds = 0;
  for (int seed_idx = 0; seed_idx < num_seeds; seed_idx++) {
    if (worker->seed_plays[seed_idx] >= 2) {
      shared_samples += worker->seed_plays[seed_idx];
      shared_seeds++;
    }
  }
  const int dof = shared_samples - shared_seeds - num_evaluated + 1;
  if (dof < RLAMC_PAIRED_MIN_DOF) {
    return false;
  }
  double *means = worker->gp_values;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int play_idx = worker->evaluated[eval_idx];
    means[eval_idx] =
        worker->play_sums[play_idx] / worker->play_counts[play_idx];
  }
  for (int iteration = 0; iteration < RLAMC_PAIRED_ITERATIONS; iteration++) {
    for (int seed_idx = 0; seed_idx < num_seeds; seed_idx++) {
      worker->seed_effects[seed_idx] = 0.0;
    }
    for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
      const int samples = worker->play_counts[worker->evaluated[eval_idx]];
      const double *values =
          worker->seed_values + (ptrdiff_t)eval_idx * worker->seed_capacity;
      for (int seed_idx = 0; seed_idx < samples; seed_idx++) {
        worker->seed_effects[seed_idx] += values[seed_idx] - means[eval_idx];
      }
    }
    // Effects of shared seeds only, centered over their samples.
    double effect_sum = 0.0;
    for (int seed_idx = 0; seed_idx < num_seeds; seed_idx++) {
      if (worker->seed_plays[seed_idx] >= 2) {
        effect_sum += worker->seed_effects[seed_idx];
        worker->seed_effects[seed_idx] /= worker->seed_plays[seed_idx];
      } else {
        worker->seed_effects[seed_idx] = 0.0;
      }
    }
    const double shift = effect_sum / shared_samples;
    for (int seed_idx = 0; seed_idx < num_seeds; seed_idx++) {
      if (worker->seed_plays[seed_idx] >= 2) {
        worker->seed_effects[seed_idx] -= shift;
      }
    }
    for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
      const int samples = worker->play_counts[worker->evaluated[eval_idx]];
      const double *values =
          worker->seed_values + (ptrdiff_t)eval_idx * worker->seed_capacity;
      double sum = 0.0;
      for (int seed_idx = 0; seed_idx < samples; seed_idx++) {
        sum += values[seed_idx] - worker->seed_effects[seed_idx];
      }
      means[eval_idx] = sum / samples;
    }
  }
  double residual_squares = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int samples = worker->play_counts[worker->evaluated[eval_idx]];
    const double *values =
        worker->seed_values + (ptrdiff_t)eval_idx * worker->seed_capacity;
    for (int seed_idx = 0; seed_idx < samples; seed_idx++) {
      if (worker->seed_plays[seed_idx] >= 2) {
        const double residual =
            values[seed_idx] - means[eval_idx] - worker->seed_effects[seed_idx];
        residual_squares += residual * residual;
      }
    }
  }
  double paired_variance = residual_squares / dof;
  if (paired_variance < RLAMC_MIN_ROLLOUT_VARIANCE) {
    paired_variance = RLAMC_MIN_ROLLOUT_VARIANCE;
  }
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int samples = worker->play_counts[worker->evaluated[eval_idx]];
    double variance_sum = 0.0;
    for (int seed_idx = 0; seed_idx < samples; seed_idx++) {
      variance_sum += worker->seed_plays[seed_idx] >= 2 ? paired_variance
                                                        : rollout_variance;
    }
    worker->gp_noise[eval_idx] = variance_sum / ((double)samples * samples);
  }
  return true;
}

static bool rlamc_fit(RlamcWorker *worker, const RlamcLevel *args,
                      const int num_columns) {
  const int num_evaluated = worker->num_evaluated;
  double pooled_squares = 0.0;
  double pooled_dof = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int play_idx = worker->evaluated[eval_idx];
    const double n = worker->play_counts[play_idx];
    const double mean = worker->play_sums[play_idx] / n;
    if (n > 1) {
      const double variance =
          (worker->play_squares[play_idx] / n - mean * mean) * n / (n - 1);
      pooled_squares += (variance > 0 ? variance : 0.0) * (n - 1);
      pooled_dof += n - 1;
    }
    worker->gp_values[eval_idx] = mean;
    memcpy(worker->gp_points + (ptrdiff_t)eval_idx * num_columns,
           worker->embedding + (ptrdiff_t)play_idx * num_columns,
           sizeof(double) * num_columns);
  }
  double rollout_variance = pooled_dof > 0 ? pooled_squares / pooled_dof
                                           : RLAMC_DEFAULT_ROLLOUT_VARIANCE;
  if (rollout_variance < RLAMC_MIN_ROLLOUT_VARIANCE) {
    rollout_variance = RLAMC_MIN_ROLLOUT_VARIANCE;
  }
  const bool paired =
      args->paired && rlamc_paired_estimates(worker, rollout_variance);
  double weight_sum = 0.0;
  double equity_sum = 0.0;
  double value_sum = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int play_idx = worker->evaluated[eval_idx];
    if (!paired) {
      worker->gp_noise[eval_idx] =
          rollout_variance / (double)worker->play_counts[play_idx];
    }
    const double weight = 1.0 / worker->gp_noise[eval_idx];
    weight_sum += weight;
    equity_sum += weight * worker->static_equity[play_idx];
    value_sum += weight * worker->gp_values[eval_idx];
  }
  const double equity_mean = equity_sum / weight_sum;
  const double value_mean = value_sum / weight_sum;
  double covariance = 0.0;
  double equity_variance = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const double weight = 1.0 / worker->gp_noise[eval_idx];
    const double equity_diff =
        worker->static_equity[worker->evaluated[eval_idx]] - equity_mean;
    covariance +=
        weight * equity_diff * (worker->gp_values[eval_idx] - value_mean);
    equity_variance += weight * equity_diff * equity_diff;
  }
  // Static equity is evidence of a play's value, not against it: the line's
  // slope is at least 0. A negative slope, fit to the noise of a few
  // samples, would rank the plays lowest by static equity best, and the
  // search would sample and choose those. Least squares under that
  // constraint gives a flat line at the weighted mean.
  worker->mean_slope = equity_variance > 0.0 && covariance > 0.0
                           ? covariance / equity_variance
                           : 0.0;
  worker->mean_intercept = value_mean - worker->mean_slope * equity_mean;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    worker->gp_values[eval_idx] -=
        worker->mean_intercept +
        worker->mean_slope * worker->static_equity[worker->evaluated[eval_idx]];
  }
  if (worker->gp_num_dims != num_columns) {
    gp_destroy(worker->gp);
    worker->gp = gp_create(RLAMC_MAX_EVALUATED, num_columns);
    worker->gp_num_dims = num_columns;
  }
  for (int column = 0; column < num_columns; column++) {
    worker->inverse_length_scales[column] = 1.0 / RLAMC_LENGTH_SCALE;
  }
  return gp_fit_by_likelihood(
      worker->gp, GP_KERNEL_MATERN52, 0.0, worker->inverse_length_scales,
      rlamc_length_multiples,
      (int)(sizeof(rlamc_length_multiples) / sizeof(rlamc_length_multiples[0])),
      worker->gp_points, worker->gp_values, worker->gp_noise, num_evaluated);
}

// The surrogate's posterior mean and variance at play play_idx.
static void rlamc_posterior(RlamcWorker *worker, const int num_columns,
                            const int play_idx, double *mean,
                            double *variance) {
  double residual = 0.0;
  gp_predict(worker->gp, worker->embedding + (ptrdiff_t)play_idx * num_columns,
             &residual, variance);
  *mean = worker->mean_intercept +
          worker->mean_slope * worker->static_equity[play_idx] + residual;
}

// Whether the surrogate's best evaluated play, less stop_z standard
// deviations, leads every other play by its mean plus stop_z standard
// deviations.
static bool rlamc_decided(RlamcWorker *worker, const int num_columns,
                          const double stop_z) {
  const int count = move_list_get_count(worker->candidate_list);
  int best = -1;
  double best_mean = -INFINITY;
  for (int eval_idx = 0; eval_idx < worker->num_evaluated; eval_idx++) {
    const int play_idx = worker->evaluated[eval_idx];
    double mean = 0.0;
    rlamc_posterior(worker, num_columns, play_idx, &mean, NULL);
    if (mean > best_mean) {
      best_mean = mean;
      best = play_idx;
    }
  }
  double mean = 0.0;
  double variance = 0.0;
  rlamc_posterior(worker, num_columns, best, &mean, &variance);
  const double floor = mean - stop_z * sqrt(variance > 0.0 ? variance : 0.0);
  // A play's mean plus stop_z deviations is at most its prior mean plus the
  // process's bounds (see rlamc_set_upper_bounds), so plays are visited by
  // prior mean, highest first, until even that is below the floor.
  double mean_bound = 0.0;
  double variance_bound = 0.0;
  gp_get_bounds(worker->gp, &mean_bound, &variance_bound);
  double excess =
      mean_bound + stop_z * sqrt(variance_bound > 0.0 ? variance_bound : 0.0);
  excess += RLAMC_BOUND_SLACK * (1.0 + fabs(excess));
  for (int rank = 0; rank < count; rank++) {
    const int play_idx = worker->equity_order[rank].play_idx;
    if (play_idx == best) {
      continue;
    }
    const double prior = worker->mean_intercept +
                         worker->mean_slope * worker->static_equity[play_idx];
    if (prior + excess < floor) {
      break;
    }
    rlamc_posterior(worker, num_columns, play_idx, &mean, &variance);
    if (mean + stop_z * sqrt(variance > 0.0 ? variance : 0.0) >= floor) {
      return false;
    }
  }
  return true;
}

// Gives each play in pool samples_each more recursive rollouts. A play's n-th
// rollout uses the n-th seed (or, bag-cycled, the n-th rotation), the same
// for every play: common random numbers.
static uint64_t rlamc_sample_pool(RlamcWorker *worker, const RlamcLevel *args,
                                  const RlamcEval *eval, const int *pool,
                                  const int pool_count, const int samples_each,
                                  const uint64_t seed, BestMoveCarry *carry) {
  const int rotations =
      args->bag_cycle ? rlamc_cycle_rotations(worker->game) : 1;
  uint64_t samples = 0;
  for (int pool_idx = 0; pool_idx < pool_count; pool_idx++) {
    const int play_idx = pool[pool_idx];
    if (worker->play_counts[play_idx] == 0 &&
        worker->num_evaluated < RLAMC_MAX_EVALUATED) {
      worker->eval_index[play_idx] = worker->num_evaluated;
      worker->evaluated[worker->num_evaluated++] = play_idx;
    }
    for (int sample = 0; sample < samples_each; sample++) {
      // A bag-cycled search's n-th rollout is rotation n % rotations of
      // shuffle n / rotations.
      const int count = worker->play_counts[play_idx];
      const int shuffle = rotations > 1 ? count / rotations : count;
      const int rotation = rotations > 1 ? count % rotations : 0;
      const double value =
          rlamc_rollout(worker, args, eval,
                        move_list_get_move(worker->candidate_list, play_idx),
                        rlamc_mix(seed + (uint64_t)shuffle), rotation, carry);
      if (args->paired && worker->eval_index[play_idx] >= 0 &&
          worker->play_counts[play_idx] < worker->seed_capacity) {
        worker->seed_values[(ptrdiff_t)worker->eval_index[play_idx] *
                                worker->seed_capacity +
                            worker->play_counts[play_idx]] = value;
      }
      worker->play_sums[play_idx] += value;
      worker->play_squares[play_idx] += value * value;
      worker->play_counts[play_idx]++;
      samples++;
    }
  }
  return samples;
}

// Sets worker->upper_bounds to each unevaluated play's upper confidence
// bound, for the wanted plays of highest bound to be picked from: plays that
// cannot be among them are left at -INFINITY without being predicted. A
// play's bound is at most its prior mean plus the process's bounds on the
// residual mean and variance, so plays are visited by prior mean, highest
// first, until even that is below the wanted-th highest bound found.
static void rlamc_set_upper_bounds(RlamcWorker *worker, const RlamcLevel *args,
                                   const int num_columns, const int count,
                                   const int wanted) {
  for (int play_idx = 0; play_idx < count; play_idx++) {
    worker->upper_bounds[play_idx] = -INFINITY;
  }
  if (wanted <= 0) {
    return;
  }
  double mean_bound = 0.0;
  double variance_bound = 0.0;
  gp_get_bounds(worker->gp, &mean_bound, &variance_bound);
  double excess = mean_bound;
  if (args->ucb_beta > 0.0) {
    excess +=
        args->ucb_beta * sqrt(variance_bound > 0.0 ? variance_bound : 0.0);
  }
  // Room for rounding in sums taken in another order.
  excess += RLAMC_BOUND_SLACK * (1.0 + fabs(excess));
  // The wanted highest bounds so far, highest first.
  double top[RLAMC_MAX_POOL_SIZE];
  int num_top = 0;
  const int kept_top =
      wanted < RLAMC_MAX_POOL_SIZE ? wanted : RLAMC_MAX_POOL_SIZE;
  for (int rank = 0; rank < count; rank++) {
    const int play_idx = worker->equity_order[rank].play_idx;
    if (worker->play_counts[play_idx] > 0) {
      continue;
    }
    const double prior = worker->mean_intercept +
                         worker->mean_slope * worker->static_equity[play_idx];
    if (num_top == kept_top && prior + excess < top[kept_top - 1]) {
      break;
    }
    double mean = 0.0;
    double variance = 0.0;
    rlamc_posterior(worker, num_columns, play_idx, &mean, &variance);
    const double bound =
        mean + args->ucb_beta * sqrt(variance > 0.0 ? variance : 0.0);
    worker->upper_bounds[play_idx] = bound;
    if (num_top < kept_top || bound > top[num_top - 1]) {
      int insert_at = num_top < kept_top ? num_top : kept_top - 1;
      while (insert_at > 0 && top[insert_at - 1] < bound) {
        top[insert_at] = top[insert_at - 1];
        insert_at--;
      }
      top[insert_at] = bound;
      if (num_top < kept_top) {
        num_top++;
      }
    }
  }
}

// A dynamic LAMC search (RlamcLevel.dynamic) from the first pool: budget
// rollouts, step to each pool play at a time, the pool remade after each
// step. Adds the rollouts to *samples and returns whether the last fit
// succeeded.
static bool rlamc_search_dynamic(RlamcWorker *worker, const RlamcLevel *args,
                                 const RlamcEval *eval, int *pool,
                                 int pool_count, const int budget,
                                 const int num_columns, const int count,
                                 const uint64_t seed, BestMoveCarry *carry,
                                 uint64_t *samples) {
  bool fitted = false;
  while (true) {
    const int remaining = budget - (int)*samples;
    if (remaining <= 0 || pool_count == 0) {
      break;
    }
    // The pool is in order of preference: the leader first.
    int step = args->step;
    if (step * pool_count > remaining) {
      pool_count = remaining / step;
      if (pool_count == 0) {
        pool_count = 1;
        step = remaining;
      }
    }
    *samples += rlamc_sample_pool(worker, args, eval, pool, pool_count, step,
                                  seed, carry);
    fitted = rlamc_fit(worker, args, num_columns);
    if (!fitted || (int)*samples >= budget) {
      break;
    }
    if (args->stop_z > 0.0 &&
        rlamc_decided(worker, num_columns, args->stop_z)) {
      atomic_fetch_add_explicit(&rlamc_total_stopped, 1, memory_order_relaxed);
      break;
    }
    // The leader by the surrogate's mean, then the other plays of highest
    // upper confidence bound, at most the room left for new ones among them.
    for (int play_idx = 0; play_idx < count; play_idx++) {
      worker->picked[play_idx] = false;
    }
    int leader = -1;
    double leader_mean = -INFINITY;
    for (int eval_idx = 0; eval_idx < worker->num_evaluated; eval_idx++) {
      double mean = 0.0;
      rlamc_posterior(worker, num_columns, worker->evaluated[eval_idx], &mean,
                      NULL);
      if (mean > leader_mean) {
        leader_mean = mean;
        leader = worker->evaluated[eval_idx];
      }
    }
    pool_count = 0;
    worker->picked[leader] = true;
    pool[pool_count++] = leader;
    int room = RLAMC_MAX_EVALUATED - worker->num_evaluated;
    if (room > args->pool_size - 1) {
      room = args->pool_size - 1;
    }
    rlamc_set_upper_bounds(worker, args, num_columns, count, room);
    for (int eval_idx = 0; eval_idx < worker->num_evaluated; eval_idx++) {
      const int play_idx = worker->evaluated[eval_idx];
      if (play_idx == leader) {
        continue;
      }
      double mean = 0.0;
      double variance = 0.0;
      rlamc_posterior(worker, num_columns, play_idx, &mean, &variance);
      worker->upper_bounds[play_idx] =
          mean + args->ucb_beta * sqrt(variance > 0.0 ? variance : 0.0);
    }
    int new_plays = 0;
    while (pool_count < args->pool_size) {
      int best = -1;
      double best_bound = -INFINITY;
      for (int play_idx = 0; play_idx < count; play_idx++) {
        if (worker->picked[play_idx] ||
            (worker->play_counts[play_idx] == 0 && new_plays >= room)) {
          continue;
        }
        if (worker->upper_bounds[play_idx] > best_bound) {
          best_bound = worker->upper_bounds[play_idx];
          best = play_idx;
        }
      }
      if (best < 0) {
        break;
      }
      if (worker->play_counts[best] == 0) {
        new_plays++;
      }
      worker->picked[best] = true;
      pool[pool_count++] = best;
    }
  }
  return fitted;
}

// Chooses by a LAMC search over every generated play of the position in
// game (RlamcLevel.lamc_search): a first pool of the best static plays and
// plays spread over the play embedding, then rounds in which the surrogate's
// best plays are kept and the rest of the pool is the unevaluated plays with
// the highest upper confidence bounds.
static const Move *rlamc_choose_lamc(RlamcWorker *worker,
                                     const RlamcLevel *args,
                                     const RlamcEval *eval, const Game *game,
                                     const uint64_t seed,
                                     BestMoveCarry *carry) {
  const MoveList *plays = worker->candidate_list;
  const int count = move_list_get_count(plays);
  if (count <= 1) {
    move_copy(&worker->chosen, move_list_get_move(plays, 0));
    return &worker->chosen;
  }
  game_copy(worker->game, game);
  worker->game_is_current = true;
  const int num_columns = rlamc_embed(worker, game);
  if (num_columns > RLAMC_MAX_COLUMNS) {
    log_fatal("the play embedding has more columns than RLAMC search holds");
  }
  for (int play_idx = 0; play_idx < count; play_idx++) {
    worker->play_sums[play_idx] = 0.0;
    worker->play_squares[play_idx] = 0.0;
    worker->play_counts[play_idx] = 0;
    worker->eval_index[play_idx] = -1;
  }
  const int budget = args->rounds * args->pool_size * args->num_samples;
  if (args->paired) {
    // A play draws at most num_samples seeds a round, or with a dynamic pool,
    // at most the whole budget.
    const int seeds = args->dynamic ? budget : args->rounds * args->num_samples;
    if (worker->seed_capacity < seeds) {
      free(worker->seed_values);
      free(worker->seed_effects);
      free(worker->seed_plays);
      worker->seed_capacity = seeds;
      worker->seed_values = malloc_or_die(
          sizeof(double) * (size_t)RLAMC_MAX_EVALUATED * (size_t)seeds);
      worker->seed_effects = malloc_or_die(sizeof(double) * (size_t)seeds);
      worker->seed_plays = malloc_or_die(sizeof(int) * (size_t)seeds);
    }
  }
  worker->num_evaluated = 0;
  for (int play_idx = 0; play_idx < count; play_idx++) {
    worker->equity_order[play_idx].equity = worker->static_equity[play_idx];
    worker->equity_order[play_idx].play_idx = play_idx;
  }
  qsort(worker->equity_order, (size_t)count, sizeof(RlamcEquityRank),
        rlamc_equity_rank_compare);
  int pool[RLAMC_MAX_POOL_SIZE];
  int pool_count = rlamc_farthest_points(
      worker, num_columns, args->pool_size < count ? args->pool_size : count,
      args->static_seeds);
  memcpy(pool, worker->pool, sizeof(int) * (size_t)pool_count);
  uint64_t samples = 0;
  bool fitted = false;
  if (args->dynamic) {
    fitted = rlamc_search_dynamic(worker, args, eval, pool, pool_count, budget,
                                  num_columns, count, seed, carry, &samples);
  }
  for (int round = 0; round < args->rounds && !args->dynamic; round++) {
    samples += rlamc_sample_pool(worker, args, eval, pool, pool_count,
                                 args->num_samples, seed, carry);
    fitted = rlamc_fit(worker, args, num_columns);
    if (round == args->rounds - 1 || !fitted) {
      break;
    }
    if (args->stop_z > 0.0 &&
        rlamc_decided(worker, num_columns, args->stop_z)) {
      atomic_fetch_add_explicit(&rlamc_total_stopped, 1, memory_order_relaxed);
      break;
    }
    // Keep the best by the surrogate's mean.
    for (int play_idx = 0; play_idx < count; play_idx++) {
      worker->picked[play_idx] = false;
    }
    pool_count = 0;
    double eval_means[RLAMC_MAX_EVALUATED];
    for (int eval_idx = 0; eval_idx < worker->num_evaluated; eval_idx++) {
      rlamc_posterior(worker, num_columns, worker->evaluated[eval_idx],
                      &eval_means[eval_idx], NULL);
    }
    for (int kept = 0; kept < args->keep && kept < worker->num_evaluated;
         kept++) {
      int best = -1;
      double best_mean = -INFINITY;
      for (int eval_idx = 0; eval_idx < worker->num_evaluated; eval_idx++) {
        const int play_idx = worker->evaluated[eval_idx];
        if (worker->picked[play_idx]) {
          continue;
        }
        const double mean = eval_means[eval_idx];
        if (mean > best_mean) {
          best_mean = mean;
          best = play_idx;
        }
      }
      worker->picked[best] = true;
      pool[pool_count++] = best;
    }
    // The rest: the unevaluated plays of highest upper confidence bound.
    int wanted = 0;
    while (pool_count + wanted < args->pool_size &&
           worker->num_evaluated + pool_count + wanted - args->keep <
               RLAMC_MAX_EVALUATED) {
      wanted++;
    }
    rlamc_set_upper_bounds(worker, args, num_columns, count, wanted);
    while (pool_count < args->pool_size &&
           worker->num_evaluated + pool_count - args->keep <
               RLAMC_MAX_EVALUATED) {
      int best = -1;
      double best_bound = -INFINITY;
      for (int play_idx = 0; play_idx < count; play_idx++) {
        if (!worker->picked[play_idx] &&
            worker->upper_bounds[play_idx] > best_bound) {
          best_bound = worker->upper_bounds[play_idx];
          best = play_idx;
        }
      }
      if (best < 0) {
        break;
      }
      worker->picked[best] = true;
      pool[pool_count++] = best;
    }
  }
  atomic_fetch_add_explicit(&rlamc_total_samples, samples,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&rlamc_total_candidates,
                            (uint64_t)worker->num_evaluated,
                            memory_order_relaxed);
  // The best evaluated play by the surrogate's mean, or by its own mean
  // without a fit.
  int best = worker->evaluated[0];
  double best_value = -INFINITY;
  for (int eval_idx = 0; eval_idx < worker->num_evaluated; eval_idx++) {
    const int play_idx = worker->evaluated[eval_idx];
    double value = 0.0;
    if (fitted) {
      rlamc_posterior(worker, num_columns, play_idx, &value, NULL);
    } else {
      value = worker->play_sums[play_idx] / worker->play_counts[play_idx];
    }
    if (play_idx != 0) {
      value -= args->switch_margin;
    }
    if (value > best_value) {
      best_value = value;
      best = play_idx;
    }
  }
  if (best != 0) {
    atomic_fetch_add_explicit(&rlamc_total_switched, 1, memory_order_relaxed);
  }
  move_copy(&worker->chosen, move_list_get_move(plays, best));
  return &worker->chosen;
}

// The key of the decision the player on turn in game makes: its board, its
// rack, which player it is, the spread and the scoreless turns. The unseen
// tiles it draws other racks from follow from the board and its rack.
static uint64_t rlamc_decision_key(const Game *game) {
  const int mover = game_get_player_on_turn_index(game);
  uint64_t key = rlamc_board_hash(game_get_board(game));
  const Rack *rack = player_get_rack(game_get_player(game, mover));
  for (int letter = 0; letter < rack_get_dist_size(rack); letter++) {
    key = rlamc_mix(key ^ ((uint64_t)letter << 8) ^
                    (uint64_t)rack_get_letter(rack, (MachineLetter)letter));
  }
  const Equity spread = player_get_score(game_get_player(game, mover)) -
                        player_get_score(game_get_player(game, 1 - mover));
  key = rlamc_mix(key ^ (uint64_t)(int64_t)spread);
  key = rlamc_mix(key ^ ((uint64_t)mover << 40) ^
                  ((uint64_t)game_get_consecutive_scoreless_turns(game) << 48));
  return key | 1ULL;
}

static const Move *rlamc_choose_move_uncached(RlamcWorker *worker,
                                              const RlamcLevel *args,
                                              const RlamcEval *eval,
                                              const Game *game, uint64_t seed,
                                              BestMoveCarry *carry);

const Move *rlamc_choose_move(RlamcWorker *worker, const RlamcArgs *args,
                              const RlamcEval *eval, const Game *game,
                              const uint64_t seed, BestMoveCarry *carry) {
  worker->spec = args;
  return rlamc_choose_level(worker, eval, game, seed, carry);
}

// The play of a search of the worker's level, from game.
static const Move *rlamc_choose_level(RlamcWorker *worker,
                                      const RlamcEval *eval, const Game *game,
                                      const uint64_t seed,
                                      BestMoveCarry *carry) {
  const RlamcLevel *args = &worker->spec->levels[worker->level];
  if (!args->decision_cache) {
    return rlamc_choose_move_uncached(worker, args, eval, game, seed, carry);
  }
  if (worker->decision_keys == NULL) {
    worker->decision_keys =
        calloc_or_die((size_t)1 << RLAMC_CACHE_BITS, sizeof(uint64_t));
    worker->decision_moves =
        malloc_or_die(sizeof(Move) * ((size_t)1 << RLAMC_CACHE_BITS));
  }
  // The decision is a function of its key alone, its recursive rollouts' seeds
  // included, so a cached one is the one the search would make again.
  const uint64_t key = rlamc_decision_key(game);
  const uint64_t slot = key & (((uint64_t)1 << RLAMC_CACHE_BITS) - 1);
  if (worker->decision_keys[slot] == key) {
    atomic_fetch_add_explicit(&rlamc_total_decision_hits, 1,
                              memory_order_relaxed);
    move_copy(&worker->chosen, &worker->decision_moves[slot]);
    return &worker->chosen;
  }
  const Move *chosen = rlamc_choose_move_uncached(
      worker, args, eval, game, rlamc_mix(key ^ worker->sim_seed), carry);
  worker->decision_keys[slot] = key;
  move_copy(&worker->decision_moves[slot], chosen);
  return chosen;
}

static const Move *
rlamc_choose_move_uncached(RlamcWorker *worker, const RlamcLevel *args,
                           const RlamcEval *eval, const Game *game,
                           const uint64_t seed, BestMoveCarry *carry) {
  worker->game_is_current = false;
  rlamc_generate_candidates(worker, args, game);
  atomic_fetch_add_explicit(&rlamc_total_searches, 1, memory_order_relaxed);
  if (worker->level > 0) {
    atomic_fetch_add_explicit(&rlamc_total_inner_searches, 1,
                              memory_order_relaxed);
  }
  if (args->lamc_search) {
    return rlamc_choose_lamc(worker, args, eval, game, seed, carry);
  }
  atomic_fetch_add_explicit(&rlamc_total_candidates,
                            (uint64_t)worker->num_candidates,
                            memory_order_relaxed);
  if (worker->num_candidates <= 1) {
    move_copy(&worker->chosen, &worker->candidates[0]);
    return &worker->chosen;
  }
  if (args->share) {
    return rlamc_choose_shared(worker, args, eval, game, seed, carry);
  }
  game_copy(worker->game, game);
  worker->game_is_current = true;
  return rlamc_choose_plain(worker, args, eval, seed, carry);
}

static bool rlamc_parse_int(const char *value, const int min, const int max,
                            int *out) {
  ErrorStack *error_stack = error_stack_create();
  const int parsed = string_to_int(value, error_stack);
  const bool ok =
      error_stack_is_empty(error_stack) && parsed >= min && parsed <= max;
  error_stack_destroy(error_stack);
  if (ok) {
    *out = parsed;
  }
  return ok;
}

static bool rlamc_parse_double(const char *value, double *out) {
  ErrorStack *error_stack = error_stack_create();
  const double parsed = string_to_double(value, error_stack);
  const bool ok = error_stack_is_empty(error_stack);
  error_stack_destroy(error_stack);
  if (ok) {
    *out = parsed;
  }
  return ok;
}

static bool rlamc_parse_plies(const char *value, uint32_t *mask) {
  StringSplitter *plies = split_string(value, '+', true);
  const int count = string_splitter_get_number_of_items(plies);
  uint32_t parsed_mask = 0;
  bool ok = count > 0;
  for (int item_idx = 0; ok && item_idx < count; item_idx++) {
    int ply = 0;
    ok = rlamc_parse_int(string_splitter_get_item(plies, item_idx), 0,
                         RLAMC_MAX_PLY_INDEX, &ply);
    if (ok) {
      parsed_mask |= 1U << ply;
    }
  }
  string_splitter_destroy(plies);
  if (ok) {
    *mask = parsed_mask;
  }
  return ok;
}

static bool rlamc_parse_setting(const char *key, const char *value,
                                RlamcArgs *spec, RlamcLevel *args) {
  if (strings_iequal(key, "plies")) {
    return rlamc_parse_plies(value, &args->plies_mask);
  }
  if (strings_iequal(key, "k")) {
    return rlamc_parse_int(value, 1, RLAMC_MAX_CANDIDATES,
                           &args->num_candidates);
  }
  if (strings_iequal(key, "m")) {
    return rlamc_parse_int(value, 1, RLAMC_MAX_SAMPLES, &args->num_samples);
  }
  if (strings_iequal(key, "h")) {
    return rlamc_parse_int(value, 0, RLAMC_MAX_PLIES, &args->horizon_plies);
  }
  if (strings_iequal(key, "margin")) {
    return rlamc_parse_int(value, 0, 1000, &args->margin);
  }
  if (strings_iequal(key, "maxbag")) {
    return rlamc_parse_int(value, 0, 1000, &args->max_bag);
  }
  if (strings_iequal(key, "minbag")) {
    return rlamc_parse_int(value, 0, 1000, &args->min_bag);
  }
  if (strings_iequal(key, "alloc")) {
    if (strings_iequal(value, "uniform")) {
      args->alloc = RLAMC_ALLOC_UNIFORM;
      return true;
    }
    if (strings_iequal(value, "race")) {
      args->alloc = RLAMC_ALLOC_RACE;
      return true;
    }
    return false;
  }
  if (strings_iequal(key, "z")) {
    return rlamc_parse_double(value, &args->race_z);
  }
  if (strings_iequal(key, "switch")) {
    return rlamc_parse_double(value, &args->switch_margin);
  }
  if (strings_iequal(key, "finalists")) {
    return rlamc_parse_int(value, 0, 1000, &spec->finalists);
  }
  if (strings_iequal(key, "cv")) {
    return rlamc_parse_int(value, 0, RLAMC_MAX_CONTROL_SAMPLES,
                           &spec->control_samples);
  }
  if (strings_iequal(key, "stage")) {
    return rlamc_parse_double(value, &spec->stage_share) &&
           spec->stage_share > 0.0 && spec->stage_share < 1.0;
  }
  if (strings_iequal(key, "lamc")) {
    int lamc = 0;
    if (!rlamc_parse_int(value, 0, 1, &lamc)) {
      return false;
    }
    args->lamc_search = lamc == 1;
    return true;
  }
  if (strings_iequal(key, "pool")) {
    return rlamc_parse_int(value, 2, RLAMC_MAX_POOL_SIZE, &args->pool_size);
  }
  if (strings_iequal(key, "keep")) {
    return rlamc_parse_int(value, 1, RLAMC_MAX_POOL_SIZE, &args->keep);
  }
  if (strings_iequal(key, "rounds")) {
    return rlamc_parse_int(value, 1, RLAMC_MAX_ROUNDS, &args->rounds);
  }
  if (strings_iequal(key, "beta")) {
    return rlamc_parse_double(value, &args->ucb_beta);
  }
  if (strings_iequal(key, "stop")) {
    return rlamc_parse_double(value, &args->stop_z) && args->stop_z >= 0.0;
  }
  if (strings_iequal(key, "dyn")) {
    int dynamic = 0;
    if (!rlamc_parse_int(value, 0, 1, &dynamic)) {
      return false;
    }
    args->dynamic = dynamic == 1;
    return true;
  }
  if (strings_iequal(key, "step")) {
    return rlamc_parse_int(value, 1, RLAMC_MAX_SAMPLES, &args->step);
  }
  if (strings_iequal(key, "cycle")) {
    int bag_cycle = 0;
    if (!rlamc_parse_int(value, 0, 1, &bag_cycle)) {
      return false;
    }
    args->bag_cycle = bag_cycle == 1;
    return true;
  }
  if (strings_iequal(key, "fin")) {
    int finish = 1;
    if (!rlamc_parse_int(value, 0, 1, &finish)) {
      return false;
    }
    args->skip_finish = finish == 0;
    return true;
  }
  if (strings_iequal(key, "pair")) {
    int paired = 0;
    if (!rlamc_parse_int(value, 0, 1, &paired)) {
      return false;
    }
    args->paired = paired == 1;
    return true;
  }
  if (strings_iequal(key, "universe")) {
    return rlamc_parse_int(value, 0, RLAMC_MAX_UNIVERSE, &args->universe);
  }
  if (strings_iequal(key, "seeds")) {
    return rlamc_parse_int(value, 1, RLAMC_MAX_CANDIDATES, &args->static_seeds);
  }
  if (strings_iequal(key, "dcache")) {
    int cache = 0;
    if (!rlamc_parse_int(value, 0, 1, &cache)) {
      return false;
    }
    args->decision_cache = cache == 1;
    return true;
  }
  if (strings_iequal(key, "share")) {
    int share = 0;
    if (!rlamc_parse_int(value, 0, 1, &share)) {
      return false;
    }
    args->share = share == 1;
    return true;
  }
  return false;
}

// Parses one level's comma-separated settings into level. Returns false,
// with an error on error_stack, for a bad one.
static bool rlamc_parse_level(const char *settings_spec, RlamcArgs *spec,
                              RlamcLevel *level, ErrorStack *error_stack) {
  StringSplitter *settings = split_string(settings_spec, ',', true);
  const int count = string_splitter_get_number_of_items(settings);
  bool ok = true;
  for (int item_idx = 0; ok && item_idx < count; item_idx++) {
    const char *setting = string_splitter_get_item(settings, item_idx);
    StringSplitter *key_value = split_string(setting, '=', false);
    ok = string_splitter_get_number_of_items(key_value) == 2 &&
         rlamc_parse_setting(string_splitter_get_item(key_value, 0),
                             string_splitter_get_item(key_value, 1), spec,
                             level);
    string_splitter_destroy(key_value);
    if (!ok) {
      error_stack_push(
          error_stack, ERROR_STATUS_RLAMC_BAD_SPEC,
          get_formatted_string("invalid RLAMC setting '%s'", setting));
    }
  }
  string_splitter_destroy(settings);
  return ok;
}

void rlamc_args_parse(const char *spec, RlamcArgs *args,
                      ErrorStack *error_stack) {
  RlamcArgs parsed;
  rlamc_args_set_defaults(&parsed);
  if (strings_iequal(spec, "off") || strings_iequal(spec, "false")) {
    *args = parsed;
    return;
  }
  parsed.enabled = true;
  if (strings_iequal(spec, "on") || strings_iequal(spec, "true")) {
    *args = parsed;
    return;
  }
  StringSplitter *levels = split_string(spec, '/', false);
  const int count = string_splitter_get_number_of_items(levels);
  if (count > RLAMC_MAX_DEPTH) {
    error_stack_push(error_stack, ERROR_STATUS_RLAMC_BAD_SPEC,
                     get_formatted_string("RLAMC has at most %d levels, not %d",
                                          RLAMC_MAX_DEPTH, count));
    string_splitter_destroy(levels);
    return;
  }
  parsed.depth = count;
  for (int level_idx = 0; level_idx < count; level_idx++) {
    if (level_idx > 0) {
      parsed.levels[level_idx] = parsed.levels[level_idx - 1];
    }
    if (!rlamc_parse_level(string_splitter_get_item(levels, level_idx), &parsed,
                           &parsed.levels[level_idx], error_stack)) {
      string_splitter_destroy(levels);
      return;
    }
    // A shared search's recursive rollouts play only static replies.
    if (level_idx > 0 && parsed.levels[level_idx - 1].share) {
      error_stack_push(error_stack, ERROR_STATUS_RLAMC_BAD_SPEC,
                       string_duplicate("only the deepest RLAMC level may "
                                        "share its rollouts"));
      string_splitter_destroy(levels);
      return;
    }
  }
  string_splitter_destroy(levels);
  *args = parsed;
}
