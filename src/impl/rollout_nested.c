#include "rollout_nested.h"

#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/lamc_defs.h"
#include "../def/move_defs.h"
#include "../def/play_feature_defs.h"
#include "../ent/bag.h"
#include "../ent/best_move_carry.h"
#include "../ent/board.h"
#include "../ent/cross_set_cache.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/nested_args.h"
#include "../ent/play_feature.h"
#include "../ent/player.h"
#include "../ent/rack.h"
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Rounds every candidate gets before a race may drop any.
  NESTED_RACE_MIN_ROUNDS = 2,
  // The shared cache's entries, a power of 2, and the most probes a lookup
  // makes before it gives up and computes the entry again.
  NESTED_CACHE_BITS = 14,
  NESTED_CACHE_PROBES = 8,
  // The most tiles a pool holds.
  NESTED_MAX_POOL = 1024,
  // The most static plies a nested rollout plays to finish a game, and the
  // ply it tells the census they are (past any the census serves).
  NESTED_FINISH_MAX_PLIES = 40,
  NESTED_FINISH_PLY = 1000,
};

// A feature column whose standard deviation is below this is constant.
#define NESTED_MIN_COLUMN_STDEV 1e-9
// The LAMC search's variance floors and the rollout variance it assumes
// before any play has two samples (as Phase 1's, in lamc.c).
#define NESTED_MIN_SIGNAL_VARIANCE 1e-6
#define NESTED_MIN_ROLLOUT_VARIANCE 1e-4
#define NESTED_DEFAULT_ROLLOUT_VARIANCE 0.1
// The LAMC search's length scale, in standard deviations of each column, and
// the multiples of it and of the signal variance each fit chooses among.
#define NESTED_LENGTH_SCALE 1.5

// One cached reply: the replying player's best play to a board, from one
// sample's racks.
typedef struct NestedReply {
  // The reply's score plus the value of the leave it keeps.
  Equity equity;
  // After the reply: tiles in the bag, and on the replier's rack.
  uint8_t bag_after;
  uint8_t replier_rack_after;
} NestedReply;

static const double nested_length_multiples[] = {0.5, 1.0, 2.0};
static const double nested_signal_multiples[] = {0.1, 0.3, 1.0};

// Standardizes count values, stride apart: a constant column becomes zeros.
static void nested_standardize(double *values, const int stride,
                               const int count) {
  double sum = 0.0;
  double squares = 0.0;
  for (int idx = 0; idx < count; idx++) {
    sum += values[idx * stride];
    squares += values[idx * stride] * values[idx * stride];
  }
  const double mean = sum / count;
  const double variance = squares / count - mean * mean;
  const double stdev = variance > 0.0 ? sqrt(variance) : 0.0;
  for (int idx = 0; idx < count; idx++) {
    values[idx * stride] = stdev < NESTED_MIN_COLUMN_STDEV
                               ? 0.0
                               : (values[idx * stride] - mean) / stdev;
  }
}

struct NestedWorker {
  // The level of search the worker makes (an index into NestedArgs.levels),
  // the settings of its current search, and the worker of the level below
  // it, made when first needed.
  int level;
  const NestedArgs *spec;
  NestedWorker *inner;
  CrossSetCache *cross_set_cache;
  // The copy of the rollout's game the nested rollouts play on.
  Game *game;
  // The candidates of the current search, best by static equity first.
  MoveList *candidate_list;
  int candidate_capacity;
  Move candidates[NESTED_MAX_CANDIDATES];
  int num_candidates;
  // For the static plies of nested rollouts.
  MoveList *reply_list;
  // Per candidate: whether it is still in the race, and its utility sums.
  bool alive[NESTED_MAX_CANDIDATES];
  double sums[NESTED_MAX_CANDIDATES];
  int counts[NESTED_MAX_CANDIDATES];
  // Per candidate and round: the utilities, for paired differences.
  double *values;
  int values_capacity;
  Move chosen;
  // The shared cache: keys, and num_samples replies per entry.
  uint64_t sim_seed;
  uint64_t *cache_keys;
  NestedReply *cache_replies;
  int cache_samples;
  // Whether the worker's game holds the search's position.
  bool game_is_current;
  // The census that steers the sim's rollouts, if one does, the thread's
  // worker of it, and the rollout ply of the replies the search plays.
  CensusRolloutWorker *census_worker;
  const CensusRollout *census;
  int reply_ply;
  // The decision cache (NestedLevel.decision_cache): keys, and the play each
  // decision chose.
  uint64_t *decision_keys;
  Move *decision_moves;
  // For diverse candidates (NestedLevel.universe): the play features, and the
  // embedding of the generated plays with its scratch.
  bool features[NUMBER_OF_PLAY_FEATURES];
  bool features_ready;
  PlayFeatureCtx feature_ctx;
  double *embedding;
  int embedding_capacity;
  double *min_distance;
  bool *picked;
  // For the LAMC search (NestedLevel.lamc_search), per generated play: its
  // standardized static equity, its nested rollouts' sums and count, and
  // its upper confidence bound; the pool, the evaluated plays, and the
  // surrogate.
  int play_capacity;
  double *static_equity;
  double *play_sums;
  double *play_squares;
  int *play_counts;
  double *upper_bounds;
  int pool[NESTED_MAX_UNIVERSE];
  int evaluated[NESTED_MAX_EVALUATED];
  int num_evaluated;
  double gp_points[NESTED_MAX_EVALUATED * NESTED_MAX_COLUMNS];
  double gp_values[NESTED_MAX_EVALUATED];
  double gp_noise[NESTED_MAX_EVALUATED];
  double fit_inverse_length_scales[NESTED_MAX_COLUMNS];
  double mean_intercept;
  double mean_slope;
  GaussianProcess *gp;
  int gp_num_dims;
};

static _Atomic uint64_t nested_total_searches;
static _Atomic uint64_t nested_total_inner_searches;
static _Atomic uint64_t nested_total_samples;
static _Atomic uint64_t nested_total_candidates;
static _Atomic uint64_t nested_total_switched;
static _Atomic uint64_t nested_total_cache_hits;
static _Atomic uint64_t nested_total_cache_misses;
static _Atomic uint64_t nested_total_decision_hits;

void nested_totals_reset(void) {
  atomic_store(&nested_total_searches, 0);
  atomic_store(&nested_total_inner_searches, 0);
  atomic_store(&nested_total_samples, 0);
  atomic_store(&nested_total_candidates, 0);
  atomic_store(&nested_total_switched, 0);
  atomic_store(&nested_total_cache_hits, 0);
  atomic_store(&nested_total_cache_misses, 0);
  atomic_store(&nested_total_decision_hits, 0);
}

NestedTotals nested_get_totals(void) {
  NestedTotals totals = {
      .searches = atomic_load(&nested_total_searches),
      .inner_searches = atomic_load(&nested_total_inner_searches),
      .samples = atomic_load(&nested_total_samples),
      .candidates = atomic_load(&nested_total_candidates),
      .switched = atomic_load(&nested_total_switched),
      .cache_hits = atomic_load(&nested_total_cache_hits),
      .cache_misses = atomic_load(&nested_total_cache_misses),
      .decision_hits = atomic_load(&nested_total_decision_hits),
  };
  return totals;
}

static NestedWorker *nested_worker_create_level(const Game *game,
                                                CrossSetCache *cross_set_cache,
                                                const int level) {
  NestedWorker *worker = malloc_or_die(sizeof(NestedWorker));
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
  worker->num_evaluated = 0;
  worker->gp = NULL;
  worker->gp_num_dims = 0;
  return worker;
}

NestedWorker *nested_worker_create(const Game *game,
                                   CrossSetCache *cross_set_cache) {
  return nested_worker_create_level(game, cross_set_cache, 0);
}

void nested_worker_set_census(NestedWorker *worker,
                              CensusRolloutWorker *census_worker,
                              const CensusRollout *census,
                              const int reply_ply) {
  worker->census_worker = census_worker;
  worker->census = census;
  worker->reply_ply = reply_ply;
}

// The top static play of the player on turn in game, steered by the census
// when the sim has one: the same play, found faster.
static const Move *nested_top_play(NestedWorker *worker, Game *game,
                                   const int ply, BestMoveCarry *carry) {
  if (worker->census != NULL) {
    return census_rollout_get_best_move(worker->census_worker, worker->census,
                                        game, worker->reply_list, ply);
  }
  return get_top_equity_move_with_carry(game, worker->reply_list, carry);
}

void nested_worker_reset(NestedWorker *worker, const uint64_t sim_seed) {
  worker->sim_seed = sim_seed;
  if (worker->inner != NULL) {
    nested_worker_reset(worker->inner, sim_seed);
  }
  if (worker->cache_keys != NULL) {
    memset(worker->cache_keys, 0,
           sizeof(uint64_t) * ((size_t)1 << NESTED_CACHE_BITS));
  }
  if (worker->decision_keys != NULL) {
    memset(worker->decision_keys, 0,
           sizeof(uint64_t) * ((size_t)1 << NESTED_CACHE_BITS));
  }
}

void nested_worker_destroy(NestedWorker *worker) {
  if (!worker) {
    return;
  }
  nested_worker_destroy(worker->inner);
  game_destroy(worker->game);
  move_list_destroy(worker->candidate_list);
  move_list_destroy(worker->reply_list);
  free(worker->values);
  free(worker->cache_keys);
  free(worker->cache_replies);
  free(worker->decision_keys);
  free(worker->decision_moves);
  free(worker->embedding);
  free(worker->min_distance);
  free(worker->picked);
  free(worker->static_equity);
  free(worker->play_sums);
  free(worker->play_squares);
  free(worker->play_counts);
  free(worker->upper_bounds);
  gp_destroy(worker->gp);
  free(worker);
}

static uint64_t nested_mix(uint64_t value) {
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
static int nested_embed(NestedWorker *worker, const Game *game) {
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
        nested_standardize(embedding + column + offset, num_columns, count);
      }
    }
    column += feature_columns;
  }
  play_feature_extract(PLAY_FEATURE_EQUITY, &worker->feature_ctx, plays, 1,
                       worker->static_equity);
  nested_standardize(worker->static_equity, 1, count);
  return num_columns;
}

static double nested_distance_squared(const double *a, const double *b,
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
static int nested_farthest_points(NestedWorker *worker, const int num_columns,
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
    const double *chosen_row = embedding + chosen * num_columns;
    for (int play_idx = 0; play_idx < count; play_idx++) {
      const double distance = nested_distance_squared(
          embedding + play_idx * num_columns, chosen_row, num_columns);
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
static void nested_pick_diverse(NestedWorker *worker, const NestedLevel *args,
                                const Game *game) {
  const int num_columns = nested_embed(worker, game);
  const int picks = nested_farthest_points(
      worker, num_columns, args->num_candidates, args->static_seeds);
  worker->num_candidates = 0;
  for (int pick = 0; pick < picks; pick++) {
    move_copy(&worker->candidates[worker->num_candidates++],
              move_list_get_move(worker->candidate_list, worker->pool[pick]));
  }
}

// Generates the candidates of the player on turn in game: up to
// num_candidates plays by static equity, within margin points of the best,
// or with a universe, a diverse pick from that many (nested_pick_diverse).
static void nested_generate_candidates(NestedWorker *worker,
                                       const NestedLevel *args,
                                       const Game *game) {
  int capacity = args->num_candidates;
  if (args->lamc_search) {
    capacity = args->universe > 0 ? args->universe : NESTED_MAX_UNIVERSE;
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
    // nested_choose_lamc searches the plays as generated.
    return;
  }
  if (args->universe > 0) {
    nested_pick_diverse(worker, args, game);
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

// The utility for player mover of the nested rollout's last state, as the sim
// scores a rollout's horizon (see sim_rollout).
static double nested_score(const NestedEval *eval, const Game *game,
                           const int mover, const Equity leftover) {
  const Equity spread = player_get_score(game_get_player(game, mover)) -
                        player_get_score(game_get_player(game, 1 - mover));
  double win_pct;
  if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
    if (spread == 0) {
      win_pct = 0.5;
    } else {
      win_pct = spread > 0 ? 1.0 : 0.0;
    }
  } else {
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
    win_pct = (double)win_pct_get(
        eval->win_pcts, spread_plus_leftover, (unsigned int)bag_tiles,
        (unsigned int)on_turn_rack, (unsigned int)off_turn_rack);
    if (on_turn != mover) {
      win_pct = 1.0 - win_pct;
    }
  }
  return sim_utility_blend(win_pct, spread + leftover, eval->utility_w_winpct,
                           eval->utility_w_spread, eval->utility_spread_scale);
}

static const Move *nested_choose_level(NestedWorker *worker,
                                       const NestedEval *eval, const Game *game,
                                       uint64_t seed, BestMoveCarry *carry);

// The play at ply of a nested rollout of worker's search: a search of the
// next level if there is one and it names ply, else the top static play.
static const Move *nested_ply_play(NestedWorker *worker, const NestedEval *eval,
                                   Game *game, const int ply,
                                   const uint64_t sample_seed,
                                   BestMoveCarry *carry) {
  const NestedArgs *spec = worker->spec;
  const int inner_level = worker->level + 1;
  if (inner_level < spec->depth &&
      nested_level_applies(&spec->levels[inner_level], ply,
                           bag_get_letters(game_get_bag(game)))) {
    if (worker->inner == NULL) {
      worker->inner = nested_worker_create_level(game, worker->cross_set_cache,
                                                 inner_level);
      worker->inner->sim_seed = worker->sim_seed;
    }
    NestedWorker *inner = worker->inner;
    inner->spec = spec;
    inner->census_worker = worker->census_worker;
    inner->census = worker->census;
    inner->reply_ply = worker->reply_ply + ply + 1;
    return nested_choose_level(
        inner, eval, game,
        nested_mix(sample_seed ^ ((uint64_t)(ply + 1) * 0xA24BAED4963EE407ULL)),
        carry);
  }
  return nested_top_play(worker, game, worker->reply_ply + ply, carry);
}

// One nested rollout of candidate from worker's game, whose other player's
// rack is drawn again from sample_seed's shuffle. Returns its utility for the
// player on turn, and leaves the game as it found it, apart from the other
// player's rack and the order of the bag.
static double nested_rollout(NestedWorker *worker, const NestedLevel *args,
                             const NestedEval *eval, const Move *candidate,
                             const uint64_t sample_seed, BestMoveCarry *carry) {
  Game *game = worker->game;
  const int mover = game_get_player_on_turn_index(game);
  const int other = 1 - mover;
  return_rack_to_bag(game, other);
  game_seed(game, sample_seed);
  draw_to_full_rack(game, other);

  // The plays of the rollout, and the leave each one kept, for the leftover
  // of the last two.
  Equity leaves[NESTED_MAX_PLIES + 1];
  int players[NESTED_MAX_PLIES + 1];
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

  for (int ply = 0; ply < args->nested_plies; ply++) {
    if (game_over(game)) {
      break;
    }
    const int on_turn = game_get_player_on_turn_index(game);
    const Player *on_turn_player = game_get_player(game, on_turn);
    const Move *play =
        nested_ply_play(worker, eval, game, ply, sample_seed, carry);
    rack_copy(&rack_before, player_get_rack(on_turn_player));
    if (ply == args->nested_plies - 1 && !eval->finish_endgame) {
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
  if (eval->finish_endgame && !game_over(game) &&
      bag_get_letters(game_get_bag(game)) <= eval->finish_max_bag) {
    for (int extra = 0; extra < NESTED_FINISH_MAX_PLIES && !game_over(game);
         extra++) {
      play_move(nested_top_play(worker, game, NESTED_FINISH_PLY, carry), game,
                NULL);
    }
    if (game_over(game)) {
      leftover = 0;
    }
  }
  const double utility = nested_score(eval, game, mover, leftover);
  game_unplay_last_move(game);
  return utility;
}

// Whether candidate cand_idx trails the leader by more than z standard errors
// of their paired difference over the rounds played.
static bool nested_trails(const NestedWorker *worker, const int cand_idx,
                          const int leader_idx, const int rounds,
                          const double z) {
  const double *values = worker->values;
  double sum = 0.0;
  double sum_sq = 0.0;
  for (int round = 0; round < rounds; round++) {
    const double diff = values[round * NESTED_MAX_CANDIDATES + cand_idx] -
                        values[round * NESTED_MAX_CANDIDATES + leader_idx];
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

// Chooses among the worker's candidates by plain nested rollouts, from the
// position in the worker's game.
static const Move *nested_choose_plain(NestedWorker *worker,
                                       const NestedLevel *args,
                                       const NestedEval *eval,
                                       const uint64_t seed,
                                       BestMoveCarry *carry) {
  const int num_candidates = worker->num_candidates;
  const int rounds_capacity = args->num_samples * NESTED_MAX_CANDIDATES;
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
    const uint64_t sample_seed = nested_mix(seed + (uint64_t)round);
    for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
      if (!worker->alive[cand_idx]) {
        continue;
      }
      const double value =
          nested_rollout(worker, args, eval, &worker->candidates[cand_idx],
                         sample_seed, carry);
      worker->values[round * NESTED_MAX_CANDIDATES + cand_idx] = value;
      worker->sums[cand_idx] += value;
      worker->counts[cand_idx]++;
      samples++;
    }
    if (args->alloc == NESTED_ALLOC_RACE &&
        round + 1 >= NESTED_RACE_MIN_ROUNDS) {
      int leader = -1;
      for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
        if (worker->alive[cand_idx] &&
            (leader < 0 || worker->sums[cand_idx] > worker->sums[leader])) {
          leader = cand_idx;
        }
      }
      for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
        if (cand_idx != leader && worker->alive[cand_idx] &&
            nested_trails(worker, cand_idx, leader, round + 1, args->race_z)) {
          worker->alive[cand_idx] = false;
          num_alive--;
        }
      }
    }
  }
  atomic_fetch_add_explicit(&nested_total_samples, samples,
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
    atomic_fetch_add_explicit(&nested_total_switched, 1, memory_order_relaxed);
  }
  move_copy(&worker->chosen, &worker->candidates[best]);
  return &worker->chosen;
}

// A hash of a square holding a letter.
static uint64_t nested_square_hash(const int row, const int col,
                                   const MachineLetter letter) {
  return nested_mix(0xC2B2AE3D27D4EB4FULL ^
                    ((uint64_t)((row * BOARD_DIM + col) * 256 + letter)));
}

static uint64_t nested_board_hash(const Board *board) {
  uint64_t hash = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter letter = board_get_letter(board, row, col);
      if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
        hash ^= nested_square_hash(row, col, letter);
      }
    }
  }
  return hash;
}

// The hash of the board move leaves, from the hash of the board it is
// played on. Exchanges and passes leave the board as it is.
static uint64_t nested_board_hash_after(const uint64_t board_hash,
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
    hash ^= nested_square_hash(row, col, letter);
  }
  return hash;
}

// The tiles a placement takes from the bag's count: the tiles it places. An
// exchange or a pass leaves the bag's count as it is.
static int nested_tiles_drawn(const Move *move) {
  return move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE
             ? move_get_tiles_played(move)
             : 0;
}

typedef struct NestedTilePriority {
  uint64_t priority;
  MachineLetter letter;
} NestedTilePriority;

static int nested_compare_priorities(const void *a, const void *b) {
  const uint64_t pa = ((const NestedTilePriority *)a)->priority;
  const uint64_t pb = ((const NestedTilePriority *)b)->priority;
  if (pa < pb) {
    return -1;
  }
  return pa > pb ? 1 : 0;
}

// Fills the replies of the board in the worker's game, whose player on turn
// is the replier and whose bag and racks hold every tile not on the board:
// for each sample, the replier's rack and the other player's are the tiles of
// lowest priority for that sample, and the replier plays its best play.
static void nested_fill_replies(NestedWorker *worker, const NestedLevel *args,
                                NestedReply *replies, BestMoveCarry *carry) {
  Game *game = worker->game;
  const int replier = game_get_player_on_turn_index(game);
  const int other = 1 - replier;
  return_rack_to_bag(game, replier);
  return_rack_to_bag(game, other);
  const Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int dist_size = ld_get_size(ld);
  NestedTilePriority pool[NESTED_MAX_POOL];
  const Player *replier_player = game_get_player(game, replier);
  for (int sample = 0; sample < args->num_samples; sample++) {
    int pool_size = 0;
    for (int letter = 0; letter < dist_size; letter++) {
      const int count = bag_get_letter(bag, (MachineLetter)letter);
      for (int copy = 0; copy < count && pool_size < NESTED_MAX_POOL; copy++) {
        pool[pool_size].priority =
            nested_mix(worker->sim_seed ^ ((uint64_t)sample << 32) ^
                       ((uint64_t)letter << 12) ^ (uint64_t)copy);
        pool[pool_size].letter = (MachineLetter)letter;
        pool_size++;
      }
    }
    qsort(pool, (size_t)pool_size, sizeof(NestedTilePriority),
          nested_compare_priorities);
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
    const Move *reply = nested_top_play(worker, game, worker->reply_ply, carry);
    Rack rack_before;
    rack_copy(&rack_before, player_get_rack(replier_player));
    const Equity leave = get_leave_value_for_move(
        player_get_klv(replier_player), reply, &rack_before);
    const int bag_tiles = bag_get_letters(bag);
    int drawn = nested_tiles_drawn(reply);
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
static const NestedReply *
nested_get_replies(NestedWorker *worker, const NestedLevel *args,
                   const Game *game, const Move *candidate, const uint64_t key,
                   BestMoveCarry *carry) {
  const uint64_t mask = ((uint64_t)1 << NESTED_CACHE_BITS) - 1;
  uint64_t slot = key & mask;
  uint64_t free_slot = UINT64_MAX;
  for (int probe = 0; probe < NESTED_CACHE_PROBES; probe++) {
    const uint64_t probe_slot = (slot + (uint64_t)probe) & mask;
    if (worker->cache_keys[probe_slot] == key) {
      atomic_fetch_add_explicit(&nested_total_cache_hits, 1,
                                memory_order_relaxed);
      return &worker->cache_replies[probe_slot * (uint64_t)args->num_samples];
    }
    if (worker->cache_keys[probe_slot] == 0 && free_slot == UINT64_MAX) {
      free_slot = probe_slot;
    }
  }
  atomic_fetch_add_explicit(&nested_total_cache_misses, 1,
                            memory_order_relaxed);
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
  NestedReply *replies =
      &worker->cache_replies[slot * (uint64_t)args->num_samples];
  nested_fill_replies(worker, args, replies, carry);
  game_unplay_last_move(worker_game);
  worker->cache_keys[slot] = key;
  return replies;
}

// The value for the player on turn of candidate, which scores score and keeps
// leave, against the cached replies to the board it leaves. spread is the
// player's spread before it.
static double
nested_value_from_replies(const NestedLevel *args, const NestedEval *eval,
                          const NestedReply *replies, const Equity spread_after,
                          const Equity leave, const int mover_rack_after) {
  double sum = 0.0;
  for (int sample = 0; sample < args->num_samples; sample++) {
    const NestedReply *reply = &replies[sample];
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
// nested rollouts; a candidate that would empty the bag gets plain ones.
static const Move *nested_choose_shared(NestedWorker *worker,
                                        const NestedLevel *args,
                                        const NestedEval *eval,
                                        const Game *game, const uint64_t seed,
                                        BestMoveCarry *carry) {
  if (worker->cache_keys == NULL ||
      worker->cache_samples != args->num_samples) {
    free(worker->cache_keys);
    free(worker->cache_replies);
    const size_t entries = (size_t)1 << NESTED_CACHE_BITS;
    worker->cache_keys = calloc_or_die(entries, sizeof(uint64_t));
    worker->cache_replies = malloc_or_die(sizeof(NestedReply) * entries *
                                          (size_t)args->num_samples);
    worker->cache_samples = args->num_samples;
  }
  const int mover = game_get_player_on_turn_index(game);
  const Player *mover_player = game_get_player(game, mover);
  const Equity spread = player_get_score(mover_player) -
                        player_get_score(game_get_player(game, 1 - mover));
  const int bag_tiles = bag_get_letters(game_get_bag(game));
  const uint64_t board_hash =
      nested_board_hash(game_get_board(game)) ^
      nested_mix(0x5851F42D4C957F2DULL + (uint64_t)(1 - mover));
  int best = 0;
  double best_value = 0.0;
  uint64_t samples = 0;
  for (int cand_idx = 0; cand_idx < worker->num_candidates; cand_idx++) {
    const Move *candidate = &worker->candidates[cand_idx];
    int drawn = nested_tiles_drawn(candidate);
    double value;
    if (bag_tiles - drawn < 1) {
      // The bag empties: the other player's rack is no longer hidden.
      if (!worker->game_is_current) {
        game_copy(worker->game, game);
        worker->game_is_current = true;
      }
      double sum = 0.0;
      for (int sample = 0; sample < args->num_samples; sample++) {
        sum += nested_rollout(worker, args, eval, candidate,
                              nested_mix(seed + (uint64_t)sample), carry);
      }
      value = sum / args->num_samples;
      samples += (uint64_t)args->num_samples;
    } else {
      const uint64_t key =
          nested_board_hash_after(board_hash, candidate) | 1ULL;
      const NestedReply *replies =
          nested_get_replies(worker, args, game, candidate, key, carry);
      Rack rack;
      rack_copy(&rack, player_get_rack(mover_player));
      const Equity leave = get_leave_value_for_move(
          player_get_klv(mover_player), candidate, &rack);
      // An exchange draws as many tiles as it returns.
      if (move_get_type(candidate) == GAME_EVENT_EXCHANGE) {
        drawn = move_get_tiles_played(candidate);
      }
      const int mover_rack_after = rack_get_total_letters(&rack) + drawn;
      value = nested_value_from_replies(
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
  atomic_fetch_add_explicit(&nested_total_samples, samples,
                            memory_order_relaxed);
  if (best != 0) {
    atomic_fetch_add_explicit(&nested_total_switched, 1, memory_order_relaxed);
  }
  move_copy(&worker->chosen, &worker->candidates[best]);
  return &worker->chosen;
}

// The LAMC search's surrogate of the evaluated plays (NestedLevel.lamc_search),
// as Phase 1's (lamc_fit): a prior mean linear in static equity, fit by least
// squares weighted by each mean's precision, and a Gaussian process of the
// residuals whose length scale and signal variance are chosen by marginal
// likelihood. Returns whether a fit succeeded.
static bool nested_fit(NestedWorker *worker, const int num_columns) {
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
                                           : NESTED_DEFAULT_ROLLOUT_VARIANCE;
  if (rollout_variance < NESTED_MIN_ROLLOUT_VARIANCE) {
    rollout_variance = NESTED_MIN_ROLLOUT_VARIANCE;
  }
  double noise_sum = 0.0;
  double weight_sum = 0.0;
  double equity_sum = 0.0;
  double value_sum = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    const int play_idx = worker->evaluated[eval_idx];
    worker->gp_noise[eval_idx] =
        rollout_variance / (double)worker->play_counts[play_idx];
    noise_sum += worker->gp_noise[eval_idx];
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
  worker->mean_slope =
      equity_variance > 0.0 ? covariance / equity_variance : 0.0;
  worker->mean_intercept = value_mean - worker->mean_slope * equity_mean;
  double value_squares = 0.0;
  for (int eval_idx = 0; eval_idx < num_evaluated; eval_idx++) {
    worker->gp_values[eval_idx] -=
        worker->mean_intercept +
        worker->mean_slope * worker->static_equity[worker->evaluated[eval_idx]];
    value_squares += worker->gp_values[eval_idx] * worker->gp_values[eval_idx];
  }
  double signal_variance =
      value_squares / num_evaluated - noise_sum / num_evaluated;
  if (signal_variance < NESTED_MIN_SIGNAL_VARIANCE) {
    signal_variance = NESTED_MIN_SIGNAL_VARIANCE;
  }
  if (worker->gp_num_dims != num_columns) {
    gp_destroy(worker->gp);
    worker->gp = gp_create(NESTED_MAX_EVALUATED, num_columns);
    worker->gp_num_dims = num_columns;
  }
  GaussianProcessParams params = {
      .kernel = GP_KERNEL_MATERN52,
      .inverse_length_scales = worker->fit_inverse_length_scales,
      .signal_variance = signal_variance,
      .prior_mean = 0.0,
  };
  const int num_length = (int)(sizeof(nested_length_multiples) /
                               sizeof(nested_length_multiples[0]));
  const int num_signal = (int)(sizeof(nested_signal_multiples) /
                               sizeof(nested_signal_multiples[0]));
  int best_length = -1;
  int best_signal = -1;
  double best_likelihood = -INFINITY;
  for (int signal_idx = 0; signal_idx < num_signal; signal_idx++) {
    params.signal_variance =
        signal_variance * nested_signal_multiples[signal_idx];
    for (int length_idx = 0; length_idx < num_length; length_idx++) {
      for (int column = 0; column < num_columns; column++) {
        worker->fit_inverse_length_scales[column] =
            1.0 / (NESTED_LENGTH_SCALE * nested_length_multiples[length_idx]);
      }
      if (gp_fit(worker->gp, &params, worker->gp_points, worker->gp_values,
                 worker->gp_noise, num_evaluated) &&
          gp_get_log_marginal_likelihood(worker->gp) > best_likelihood) {
        best_likelihood = gp_get_log_marginal_likelihood(worker->gp);
        best_length = length_idx;
        best_signal = signal_idx;
      }
    }
  }
  if (best_length < 0) {
    return false;
  }
  params.signal_variance =
      signal_variance * nested_signal_multiples[best_signal];
  for (int column = 0; column < num_columns; column++) {
    worker->fit_inverse_length_scales[column] =
        1.0 / (NESTED_LENGTH_SCALE * nested_length_multiples[best_length]);
  }
  return gp_fit(worker->gp, &params, worker->gp_points, worker->gp_values,
                worker->gp_noise, num_evaluated);
}

// The surrogate's posterior mean and variance at play play_idx.
static void nested_posterior(NestedWorker *worker, const int num_columns,
                             const int play_idx, double *mean,
                             double *variance) {
  double residual = 0.0;
  gp_predict(worker->gp, worker->embedding + (ptrdiff_t)play_idx * num_columns,
             &residual, variance);
  *mean = worker->mean_intercept +
          worker->mean_slope * worker->static_equity[play_idx] + residual;
}

// Gives each play in pool num_samples more nested rollouts. A play's n-th
// rollout uses the n-th seed, the same for every play: common random numbers.
static uint64_t nested_sample_pool(NestedWorker *worker,
                                   const NestedLevel *args,
                                   const NestedEval *eval, const int *pool,
                                   const int pool_count, const uint64_t seed,
                                   BestMoveCarry *carry) {
  uint64_t samples = 0;
  for (int pool_idx = 0; pool_idx < pool_count; pool_idx++) {
    const int play_idx = pool[pool_idx];
    if (worker->play_counts[play_idx] == 0 &&
        worker->num_evaluated < NESTED_MAX_EVALUATED) {
      worker->evaluated[worker->num_evaluated++] = play_idx;
    }
    for (int sample = 0; sample < args->num_samples; sample++) {
      const double value = nested_rollout(
          worker, args, eval,
          move_list_get_move(worker->candidate_list, play_idx),
          nested_mix(seed + (uint64_t)worker->play_counts[play_idx]), carry);
      worker->play_sums[play_idx] += value;
      worker->play_squares[play_idx] += value * value;
      worker->play_counts[play_idx]++;
      samples++;
    }
  }
  return samples;
}

// Chooses by a LAMC search over every generated play of the position in
// game (NestedLevel.lamc_search): a first pool of the best static plays and
// plays spread over the play embedding, then rounds in which the surrogate's
// best plays are kept and the rest of the pool is the unevaluated plays with
// the highest upper confidence bounds.
static const Move *nested_choose_lamc(NestedWorker *worker,
                                      const NestedLevel *args,
                                      const NestedEval *eval, const Game *game,
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
  const int num_columns = nested_embed(worker, game);
  if (num_columns > NESTED_MAX_COLUMNS) {
    log_fatal("the play embedding has more columns than nested search holds");
  }
  for (int play_idx = 0; play_idx < count; play_idx++) {
    worker->play_sums[play_idx] = 0.0;
    worker->play_squares[play_idx] = 0.0;
    worker->play_counts[play_idx] = 0;
  }
  worker->num_evaluated = 0;
  int pool[NESTED_MAX_POOL_SIZE];
  int pool_count = nested_farthest_points(
      worker, num_columns, args->pool_size < count ? args->pool_size : count,
      args->static_seeds);
  memcpy(pool, worker->pool, sizeof(int) * (size_t)pool_count);
  uint64_t samples = 0;
  bool fitted = false;
  for (int round = 0; round < args->rounds; round++) {
    samples +=
        nested_sample_pool(worker, args, eval, pool, pool_count, seed, carry);
    fitted = nested_fit(worker, num_columns);
    if (round == args->rounds - 1 || !fitted) {
      break;
    }
    // Keep the best by the surrogate's mean.
    for (int play_idx = 0; play_idx < count; play_idx++) {
      worker->picked[play_idx] = false;
    }
    pool_count = 0;
    for (int kept = 0; kept < args->keep && kept < worker->num_evaluated;
         kept++) {
      int best = -1;
      double best_mean = -INFINITY;
      for (int eval_idx = 0; eval_idx < worker->num_evaluated; eval_idx++) {
        const int play_idx = worker->evaluated[eval_idx];
        if (worker->picked[play_idx]) {
          continue;
        }
        double mean = 0.0;
        nested_posterior(worker, num_columns, play_idx, &mean, NULL);
        if (mean > best_mean) {
          best_mean = mean;
          best = play_idx;
        }
      }
      worker->picked[best] = true;
      pool[pool_count++] = best;
    }
    // The rest: the unevaluated plays of highest upper confidence bound.
    for (int play_idx = 0; play_idx < count; play_idx++) {
      if (worker->play_counts[play_idx] > 0) {
        worker->upper_bounds[play_idx] = -INFINITY;
        continue;
      }
      double mean = 0.0;
      double variance = 0.0;
      nested_posterior(worker, num_columns, play_idx, &mean, &variance);
      worker->upper_bounds[play_idx] =
          mean + args->ucb_beta * sqrt(variance > 0.0 ? variance : 0.0);
    }
    while (pool_count < args->pool_size &&
           worker->num_evaluated + pool_count - args->keep <
               NESTED_MAX_EVALUATED) {
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
  atomic_fetch_add_explicit(&nested_total_samples, samples,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&nested_total_candidates,
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
      nested_posterior(worker, num_columns, play_idx, &value, NULL);
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
    atomic_fetch_add_explicit(&nested_total_switched, 1, memory_order_relaxed);
  }
  move_copy(&worker->chosen, move_list_get_move(plays, best));
  return &worker->chosen;
}

// The key of the decision the player on turn in game makes: its board, its
// rack, which player it is, the spread and the scoreless turns. The unseen
// tiles it draws other racks from follow from the board and its rack.
static uint64_t nested_decision_key(const Game *game) {
  const int mover = game_get_player_on_turn_index(game);
  uint64_t key = nested_board_hash(game_get_board(game));
  const Rack *rack = player_get_rack(game_get_player(game, mover));
  for (int letter = 0; letter < rack_get_dist_size(rack); letter++) {
    key = nested_mix(key ^ ((uint64_t)letter << 8) ^
                     (uint64_t)rack_get_letter(rack, (MachineLetter)letter));
  }
  const Equity spread = player_get_score(game_get_player(game, mover)) -
                        player_get_score(game_get_player(game, 1 - mover));
  key = nested_mix(key ^ (uint64_t)(int64_t)spread);
  key =
      nested_mix(key ^ ((uint64_t)mover << 40) ^
                 ((uint64_t)game_get_consecutive_scoreless_turns(game) << 48));
  return key | 1ULL;
}

static const Move *
nested_choose_move_uncached(NestedWorker *worker, const NestedLevel *args,
                            const NestedEval *eval, const Game *game,
                            const uint64_t seed, BestMoveCarry *carry);

const Move *nested_choose_move(NestedWorker *worker, const NestedArgs *args,
                               const NestedEval *eval, const Game *game,
                               const uint64_t seed, BestMoveCarry *carry) {
  worker->spec = args;
  return nested_choose_level(worker, eval, game, seed, carry);
}

// The play of a search of the worker's level, from game.
static const Move *nested_choose_level(NestedWorker *worker,
                                       const NestedEval *eval, const Game *game,
                                       const uint64_t seed,
                                       BestMoveCarry *carry) {
  const NestedLevel *args = &worker->spec->levels[worker->level];
  if (!args->decision_cache) {
    return nested_choose_move_uncached(worker, args, eval, game, seed, carry);
  }
  if (worker->decision_keys == NULL) {
    worker->decision_keys =
        calloc_or_die((size_t)1 << NESTED_CACHE_BITS, sizeof(uint64_t));
    worker->decision_moves =
        malloc_or_die(sizeof(Move) * ((size_t)1 << NESTED_CACHE_BITS));
  }
  // The decision is a function of its key alone, its nested rollouts' seeds
  // included, so a cached one is the one the search would make again.
  const uint64_t key = nested_decision_key(game);
  const uint64_t slot = key & (((uint64_t)1 << NESTED_CACHE_BITS) - 1);
  if (worker->decision_keys[slot] == key) {
    atomic_fetch_add_explicit(&nested_total_decision_hits, 1,
                              memory_order_relaxed);
    move_copy(&worker->chosen, &worker->decision_moves[slot]);
    return &worker->chosen;
  }
  const Move *chosen = nested_choose_move_uncached(
      worker, args, eval, game, nested_mix(key ^ worker->sim_seed), carry);
  worker->decision_keys[slot] = key;
  move_copy(&worker->decision_moves[slot], chosen);
  return chosen;
}

static const Move *
nested_choose_move_uncached(NestedWorker *worker, const NestedLevel *args,
                            const NestedEval *eval, const Game *game,
                            const uint64_t seed, BestMoveCarry *carry) {
  worker->game_is_current = false;
  nested_generate_candidates(worker, args, game);
  atomic_fetch_add_explicit(&nested_total_searches, 1, memory_order_relaxed);
  if (worker->level > 0) {
    atomic_fetch_add_explicit(&nested_total_inner_searches, 1,
                              memory_order_relaxed);
  }
  if (args->lamc_search) {
    return nested_choose_lamc(worker, args, eval, game, seed, carry);
  }
  atomic_fetch_add_explicit(&nested_total_candidates,
                            (uint64_t)worker->num_candidates,
                            memory_order_relaxed);
  if (worker->num_candidates <= 1) {
    move_copy(&worker->chosen, &worker->candidates[0]);
    return &worker->chosen;
  }
  if (args->share) {
    return nested_choose_shared(worker, args, eval, game, seed, carry);
  }
  game_copy(worker->game, game);
  worker->game_is_current = true;
  return nested_choose_plain(worker, args, eval, seed, carry);
}

static bool nested_parse_int(const char *value, const int min, const int max,
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

static bool nested_parse_double(const char *value, double *out) {
  ErrorStack *error_stack = error_stack_create();
  const double parsed = string_to_double(value, error_stack);
  const bool ok = error_stack_is_empty(error_stack);
  error_stack_destroy(error_stack);
  if (ok) {
    *out = parsed;
  }
  return ok;
}

static bool nested_parse_plies(const char *value, uint32_t *mask) {
  StringSplitter *plies = split_string(value, '+', true);
  const int count = string_splitter_get_number_of_items(plies);
  uint32_t parsed_mask = 0;
  bool ok = count > 0;
  for (int item_idx = 0; ok && item_idx < count; item_idx++) {
    int ply = 0;
    ok = nested_parse_int(string_splitter_get_item(plies, item_idx), 0,
                          NESTED_MAX_PLY_INDEX, &ply);
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

static bool nested_parse_setting(const char *key, const char *value,
                                 NestedArgs *spec, NestedLevel *args) {
  if (strings_iequal(key, "plies")) {
    return nested_parse_plies(value, &args->plies_mask);
  }
  if (strings_iequal(key, "k")) {
    return nested_parse_int(value, 1, NESTED_MAX_CANDIDATES,
                            &args->num_candidates);
  }
  if (strings_iequal(key, "m")) {
    return nested_parse_int(value, 1, NESTED_MAX_SAMPLES, &args->num_samples);
  }
  if (strings_iequal(key, "h")) {
    return nested_parse_int(value, 0, NESTED_MAX_PLIES, &args->nested_plies);
  }
  if (strings_iequal(key, "margin")) {
    return nested_parse_int(value, 0, 1000, &args->margin);
  }
  if (strings_iequal(key, "maxbag")) {
    return nested_parse_int(value, 0, 1000, &args->max_bag);
  }
  if (strings_iequal(key, "minbag")) {
    return nested_parse_int(value, 0, 1000, &args->min_bag);
  }
  if (strings_iequal(key, "alloc")) {
    if (strings_iequal(value, "uniform")) {
      args->alloc = NESTED_ALLOC_UNIFORM;
      return true;
    }
    if (strings_iequal(value, "race")) {
      args->alloc = NESTED_ALLOC_RACE;
      return true;
    }
    return false;
  }
  if (strings_iequal(key, "z")) {
    return nested_parse_double(value, &args->race_z);
  }
  if (strings_iequal(key, "switch")) {
    return nested_parse_double(value, &args->switch_margin);
  }
  if (strings_iequal(key, "finalists")) {
    return nested_parse_int(value, 0, 1000, &spec->finalists);
  }
  if (strings_iequal(key, "stage")) {
    return nested_parse_double(value, &spec->stage_share) &&
           spec->stage_share > 0.0 && spec->stage_share < 1.0;
  }
  if (strings_iequal(key, "lamc")) {
    int lamc = 0;
    if (!nested_parse_int(value, 0, 1, &lamc)) {
      return false;
    }
    args->lamc_search = lamc == 1;
    return true;
  }
  if (strings_iequal(key, "pool")) {
    return nested_parse_int(value, 2, NESTED_MAX_POOL_SIZE, &args->pool_size);
  }
  if (strings_iequal(key, "keep")) {
    return nested_parse_int(value, 1, NESTED_MAX_POOL_SIZE, &args->keep);
  }
  if (strings_iequal(key, "rounds")) {
    return nested_parse_int(value, 1, NESTED_MAX_ROUNDS, &args->rounds);
  }
  if (strings_iequal(key, "beta")) {
    return nested_parse_double(value, &args->ucb_beta);
  }
  if (strings_iequal(key, "universe")) {
    return nested_parse_int(value, 0, NESTED_MAX_UNIVERSE, &args->universe);
  }
  if (strings_iequal(key, "seeds")) {
    return nested_parse_int(value, 1, NESTED_MAX_CANDIDATES,
                            &args->static_seeds);
  }
  if (strings_iequal(key, "dcache")) {
    int cache = 0;
    if (!nested_parse_int(value, 0, 1, &cache)) {
      return false;
    }
    args->decision_cache = cache == 1;
    return true;
  }
  if (strings_iequal(key, "share")) {
    int share = 0;
    if (!nested_parse_int(value, 0, 1, &share)) {
      return false;
    }
    args->share = share == 1;
    return true;
  }
  return false;
}

// Parses one level's comma-separated settings into level. Returns false,
// with an error on error_stack, for a bad one.
static bool nested_parse_level(const char *settings_spec, NestedArgs *spec,
                               NestedLevel *level, ErrorStack *error_stack) {
  StringSplitter *settings = split_string(settings_spec, ',', true);
  const int count = string_splitter_get_number_of_items(settings);
  bool ok = true;
  for (int item_idx = 0; ok && item_idx < count; item_idx++) {
    const char *setting = string_splitter_get_item(settings, item_idx);
    StringSplitter *key_value = split_string(setting, '=', false);
    ok = string_splitter_get_number_of_items(key_value) == 2 &&
         nested_parse_setting(string_splitter_get_item(key_value, 0),
                              string_splitter_get_item(key_value, 1), spec,
                              level);
    string_splitter_destroy(key_value);
    if (!ok) {
      error_stack_push(
          error_stack, ERROR_STATUS_NESTED_BAD_SPEC,
          get_formatted_string("invalid nested rollout setting '%s'", setting));
    }
  }
  string_splitter_destroy(settings);
  return ok;
}

void nested_args_parse(const char *spec, NestedArgs *args,
                       ErrorStack *error_stack) {
  NestedArgs parsed;
  nested_args_set_defaults(&parsed);
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
  if (count > NESTED_MAX_DEPTH) {
    error_stack_push(
        error_stack, ERROR_STATUS_NESTED_BAD_SPEC,
        get_formatted_string("nested rollouts have at most %d levels, not %d",
                             NESTED_MAX_DEPTH, count));
    string_splitter_destroy(levels);
    return;
  }
  parsed.depth = count;
  for (int level_idx = 0; level_idx < count; level_idx++) {
    if (level_idx > 0) {
      parsed.levels[level_idx] = parsed.levels[level_idx - 1];
    }
    if (!nested_parse_level(string_splitter_get_item(levels, level_idx),
                            &parsed, &parsed.levels[level_idx], error_stack)) {
      string_splitter_destroy(levels);
      return;
    }
    // A shared search's nested rollouts play only static replies.
    if (level_idx > 0 && parsed.levels[level_idx - 1].share) {
      error_stack_push(error_stack, ERROR_STATUS_NESTED_BAD_SPEC,
                       string_duplicate("only the deepest nested level may "
                                        "share its nested rollouts"));
      string_splitter_destroy(levels);
      return;
    }
  }
  string_splitter_destroy(levels);
  *args = parsed;
}
