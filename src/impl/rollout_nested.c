#include "rollout_nested.h"

#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/move_defs.h"
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
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "census_rollout.h"
#include "gameplay.h"
#include "move_gen.h"
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

// One cached reply: the replying player's best play to a board, from one
// sample's racks.
typedef struct NestedReply {
  // The reply's score plus the value of the leave it keeps.
  Equity equity;
  // After the reply: tiles in the bag, and on the replier's rack.
  uint8_t bag_after;
  uint8_t replier_rack_after;
} NestedReply;

struct NestedWorker {
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
};

static _Atomic uint64_t nested_total_searches;
static _Atomic uint64_t nested_total_samples;
static _Atomic uint64_t nested_total_candidates;
static _Atomic uint64_t nested_total_switched;
static _Atomic uint64_t nested_total_cache_hits;
static _Atomic uint64_t nested_total_cache_misses;

void nested_totals_reset(void) {
  atomic_store(&nested_total_searches, 0);
  atomic_store(&nested_total_samples, 0);
  atomic_store(&nested_total_candidates, 0);
  atomic_store(&nested_total_switched, 0);
  atomic_store(&nested_total_cache_hits, 0);
  atomic_store(&nested_total_cache_misses, 0);
}

NestedTotals nested_get_totals(void) {
  NestedTotals totals = {
      .searches = atomic_load(&nested_total_searches),
      .samples = atomic_load(&nested_total_samples),
      .candidates = atomic_load(&nested_total_candidates),
      .switched = atomic_load(&nested_total_switched),
      .cache_hits = atomic_load(&nested_total_cache_hits),
      .cache_misses = atomic_load(&nested_total_cache_misses),
  };
  return totals;
}

NestedWorker *nested_worker_create(const Game *game,
                                   CrossSetCache *cross_set_cache) {
  NestedWorker *worker = malloc_or_die(sizeof(NestedWorker));
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
  return worker;
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
  if (worker->cache_keys != NULL) {
    memset(worker->cache_keys, 0,
           sizeof(uint64_t) * ((size_t)1 << NESTED_CACHE_BITS));
  }
}

void nested_worker_destroy(NestedWorker *worker) {
  if (!worker) {
    return;
  }
  game_destroy(worker->game);
  move_list_destroy(worker->candidate_list);
  move_list_destroy(worker->reply_list);
  free(worker->values);
  free(worker->cache_keys);
  free(worker->cache_replies);
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
static void nested_generate_candidates(NestedWorker *worker,
                                       const NestedArgs *args,
                                       const Game *game) {
  const int capacity = args->num_candidates;
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

// One nested rollout of candidate from worker's game, whose other player's
// rack is drawn again from sample_seed's shuffle. Returns its utility for the
// player on turn, and leaves the game as it found it, apart from the other
// player's rack and the order of the bag.
static double nested_rollout(NestedWorker *worker, const NestedArgs *args,
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
        nested_top_play(worker, game, worker->reply_ply + ply, carry);
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
                                       const NestedArgs *args,
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
static void nested_fill_replies(NestedWorker *worker, const NestedArgs *args,
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
nested_get_replies(NestedWorker *worker, const NestedArgs *args,
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
nested_value_from_replies(const NestedArgs *args, const NestedEval *eval,
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
                                        const NestedArgs *args,
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

const Move *nested_choose_move(NestedWorker *worker, const NestedArgs *args,
                               const NestedEval *eval, const Game *game,
                               const uint64_t seed, BestMoveCarry *carry) {
  worker->game_is_current = false;
  nested_generate_candidates(worker, args, game);
  atomic_fetch_add_explicit(&nested_total_searches, 1, memory_order_relaxed);
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
                                 NestedArgs *args) {
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
    return nested_parse_int(value, 0, 1000, &args->finalists);
  }
  if (strings_iequal(key, "stage")) {
    return nested_parse_double(value, &args->stage_share) &&
           args->stage_share > 0.0 && args->stage_share < 1.0;
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

void nested_args_parse(const char *spec, NestedArgs *args,
                       ErrorStack *error_stack) {
  NestedArgs parsed;
  nested_args_set_defaults(&parsed);
  if (strings_iequal(spec, "off") || strings_iequal(spec, "false")) {
    *args = parsed;
    return;
  }
  parsed.enabled = true;
  if (!strings_iequal(spec, "on") && !strings_iequal(spec, "true")) {
    StringSplitter *settings = split_string(spec, ',', true);
    const int count = string_splitter_get_number_of_items(settings);
    for (int item_idx = 0; item_idx < count; item_idx++) {
      const char *setting = string_splitter_get_item(settings, item_idx);
      StringSplitter *key_value = split_string(setting, '=', false);
      const bool ok =
          string_splitter_get_number_of_items(key_value) == 2 &&
          nested_parse_setting(string_splitter_get_item(key_value, 0),
                               string_splitter_get_item(key_value, 1), &parsed);
      string_splitter_destroy(key_value);
      if (!ok) {
        error_stack_push(error_stack, ERROR_STATUS_NESTED_BAD_SPEC,
                         get_formatted_string(
                             "invalid nested rollout setting '%s'", setting));
        string_splitter_destroy(settings);
        return;
      }
    }
    string_splitter_destroy(settings);
  }
  *args = parsed;
}
