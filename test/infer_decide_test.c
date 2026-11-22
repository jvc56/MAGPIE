// Decision-level comparison of search arms at a real per-move budget.
//
// infdecide:GAMES:MOVE_MS:THREADS:SEED:ARMS:PLAYOUTS
//
// Self-play games in which every move is chosen by the control search (the
// engine's default sim, no inference, MOVE_MS per move on THREADS threads).
// On every turn where inference is possible the same position is also
// searched by each extra arm in ARMS:
//   s  the same search with softmax inference (temperature 8)
//   o  softmax inference (temperature 8) with the alias sampler seeded as
//      before the sampler fix; s uses the fixed sampler
//   x  the control search with four times the iteration budget (meant for
//      INFDEC_ITERATIONS runs: the value of more search)
//   l  the exact kept leave after a placement, with the refill hidden
//   p  the same search told the opponent's actual rack (perfect information)
//   c  the control search again with another seed (negative control)
//   t  the top static-equity move (positive control, free)
//   a  softmax inference at temperature 3; b at temperature 16
//   h  hard-margin inference, margin 5
//   v, k, j  feature oracles: the search samples the opponent's kept tiles
//      from the draw-count prior restricted to leaves with the true vowel
//      count (v), a leave value within 2 points of the true one (k), or both
//      (j). After an exchange these arms run without inference.
//   1, 2, 4  the search told that many tiles of the opponent's actual rack,
//      chosen at random (partial information)
// Where an arm picks a different move from the control, both moves are scored
// against the opponent's actual rack two ways:
//   playout oracle: PLAYOUTS full games to the end with static play for both
//     sides, the unseen bag reshuffled each time, same shuffles for each move;
//   sim oracle: an equal-sample (round robin) sim of just those moves with
//     the opponent's rack known, through the engine's own rollout evaluator.
// The control's move is always the one played. The game stops when the bag
// empties: the opponent's rack is then known to everyone.

#include "infer_decide_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/cpthread_defs.h"
#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/bag.h"
#include "../src/ent/encoded_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
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
  DECIDE_MAX_ARMS = 12,
  DECIDE_MAX_TURNS = 200,
  DECIDE_PLAYOUT_MAX_TURNS = 120,
  DECIDE_MOVE_STR = 96,
  DECIDE_ORACLE_SIM_ROLLOUTS_PER_MOVE = 30000,
  DECIDE_RANK_CANDIDATES = 400,
  SFIT_TOP_PLAYS = 10,
  SFIT_MAX_SAME = 5,
  SFIT_MEMORY_GRID = 5,
  SFIT_MAX_HISTORY = 6,
  SFIT_CACHE_BITS = 20,
  SFIT_MAX_PLAYS = 16,
  SFIT_MOVE_CAPACITY = 2000,
  DECIDE_SIM_LOG_PLAYS = 5,
  DECIDE_SIM_LOG_CAPACITY = 512,
  DECIDE_MORE_SEARCH_FACTOR = 4,
  STEER_TOP_PLAYS = 10,
  STEER_MAX_PLAYS = 11,
  STEER_MAX_RACKS = 4000,
  STEER_MIN_FIT = 8,
  STEER_MIN_PAIR_COUNT = 2,
  STEER_PLIES = 2,
  STEER_GRID = 6,
  DECIDE_DEFAULT_EXCHANGE_BAG = 30,
  DECIDE_RANK_LINE = 4096,
};

typedef struct DecideArm {
  char code;
  bool ran;
  bool simulated;
  Move move;
  char move_str[DECIDE_MOVE_STR];
  uint64_t iterations;
  double seconds;
  SimCtx *sim_ctx;
  // Index into the distinct-move table of the position.
  int distinct_index;
} DecideArm;

typedef struct PlayoutJob {
  const Game *base;
  const Move *moves;
  int num_moves;
  int mover;
  uint64_t seed_base;
  int first;
  int last;
  // [move][playout]
  float *win;
  float *spread;
  int playouts;
} PlayoutJob;

static double decide_win(double spread) {
  if (spread > 0) {
    return 1;
  }
  if (spread < 0) {
    return 0;
  }
  return 0.5;
}

static void *decide_playout_worker(void *context) {
  PlayoutJob *job = context;
  Game *work = game_duplicate(job->base);
  MoveList *best = move_list_create(1);
  for (int playout = job->first; playout < job->last; playout++) {
    for (int move_idx = 0; move_idx < job->num_moves; move_idx++) {
      game_copy(work, job->base);
      // Reshuffles the unseen bag; the opponent keeps its actual rack.
      game_seed(work, job->seed_base + (uint64_t)playout);
      play_move(&job->moves[move_idx], work, NULL);
      for (int turn = 0; !game_over(work) && turn < DECIDE_PLAYOUT_MAX_TURNS;
           turn++) {
        const Move *reply = get_top_equity_move(work, best);
        play_move(reply, work, NULL);
      }
      const double spread = equity_to_double(
          player_get_score(game_get_player(work, job->mover)) -
          player_get_score(game_get_player(work, 1 - job->mover)));
      const size_t slot =
          ((size_t)move_idx * (size_t)job->playouts) + (size_t)playout;
      job->spread[slot] = (float)spread;
      job->win[slot] = (float)decide_win(spread);
    }
  }
  move_list_destroy(best);
  game_destroy(work);
  return NULL;
}

static int sfit_env_int(const char *name, int fallback) {
  const char *text = getenv(name);
  return text ? atoi(text) : fallback;
}

// Sim-steered weights. For a sample of the racks the mover might have held,
// a short rollout of their best static plays and of the observed play gives
// the observed play's loss by simulation. A ridge regression of that loss on
// the static loss, the tiles kept and the pairs of tiles kept is then applied
// by the inference to every rack (InferenceLossModel).
typedef struct SteerRack {
  Rack leave;
  bool usable;
  double static_loss;
  double sim_loss;
} SteerRack;

typedef struct SteerJob {
  const Game *base;
  const Move *observed;
  const Rack *played;
  int mover;
  int rollouts;
  uint64_t seed;
  int first;
  int last;
  SteerRack *racks;
} SteerJob;

static bool steer_same_move(const Move *first, const Move *second) {
  if (move_get_type(first) != move_get_type(second) ||
      move_get_score(first) != move_get_score(second) ||
      move_get_row_start(first) != move_get_row_start(second) ||
      move_get_col_start(first) != move_get_col_start(second) ||
      move_get_dir(first) != move_get_dir(second) ||
      move_get_tiles_length(first) != move_get_tiles_length(second)) {
    return false;
  }
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(first); tile_idx++) {
    if (move_get_tile(first, tile_idx) != move_get_tile(second, tile_idx)) {
      return false;
    }
  }
  return true;
}

static void *steer_worker(void *context) {
  SteerJob *job = context;
  Game *work = game_duplicate(job->base);
  Game *roll = game_duplicate(job->base);
  MoveList *moves = move_list_create(SFIT_MOVE_CAPACITY);
  MoveList *best = move_list_create(1);
  Move candidates[STEER_MAX_PLAYS];
  const int other = 1 - job->mover;
  for (int rack_idx = job->first; rack_idx < job->last; rack_idx++) {
    SteerRack *out = &job->racks[rack_idx];
    out->usable = false;
    game_copy(work, job->base);
    game_seed(work, job->seed + ((uint64_t)rack_idx * UINT64_C(1000003)));
    set_random_rack(work, job->mover, job->played);
    rack_copy(&out->leave, player_get_rack(game_get_player(work, job->mover)));
    rack_subtract_using_floor_zero(&out->leave, job->played);
    const MoveGenArgs gen = {.game = work,
                             .move_list = moves,
                             .move_record_type = MOVE_RECORD_ALL,
                             .move_sort_type = MOVE_SORT_EQUITY,
                             .target_equity = EQUITY_MAX_VALUE};
    generate_moves(&gen);
    move_list_sort_moves(moves);
    const int count = move_list_get_count(moves);
    int observed_rank = -1;
    for (int idx = 0; idx < count && observed_rank < 0; idx++) {
      if (steer_same_move(move_list_get_move(moves, idx), job->observed)) {
        observed_rank = idx;
      }
    }
    if (observed_rank < 0) {
      continue;
    }
    out->static_loss =
        equity_to_double(move_get_equity(move_list_get_move(moves, 0))) -
        equity_to_double(
            move_get_equity(move_list_get_move(moves, observed_rank)));
    int num_candidates = 0;
    int observed_candidate = -1;
    for (int idx = 0; idx < count && idx < STEER_TOP_PLAYS; idx++) {
      if (idx == observed_rank) {
        observed_candidate = num_candidates;
      }
      move_copy(&candidates[num_candidates++], move_list_get_move(moves, idx));
    }
    const int num_top = num_candidates;
    if (observed_candidate < 0) {
      observed_candidate = num_candidates;
      move_copy(&candidates[num_candidates++],
                move_list_get_move(moves, observed_rank));
    }
    double total[STEER_MAX_PLAYS] = {0};
    for (int rollout = 0; rollout < job->rollouts; rollout++) {
      for (int idx = 0; idx < num_candidates; idx++) {
        game_copy(roll, work);
        // The same draws for every candidate; the observer's rack is unknown
        // to the mover, so it is redrawn.
        game_seed(roll, job->seed + ((uint64_t)rack_idx * UINT64_C(7919)) +
                            ((uint64_t)rollout * UINT64_C(104729)) + 1);
        set_random_rack(roll, other, NULL);
        play_move(&candidates[idx], roll, NULL);
        for (int ply = 0; ply < STEER_PLIES && !game_over(roll); ply++) {
          play_move(get_top_equity_move(roll, best), roll, NULL);
        }
        const Player *mover_player = game_get_player(roll, job->mover);
        const Player *other_player = game_get_player(roll, other);
        double value = equity_to_double(player_get_score(mover_player) -
                                        player_get_score(other_player));
        if (!game_over(roll)) {
          value +=
              equity_to_double(
                  klv_get_leave_value(player_get_klv(mover_player),
                                      player_get_rack(mover_player))) -
              equity_to_double(klv_get_leave_value(
                  player_get_klv(other_player), player_get_rack(other_player)));
        }
        total[idx] += value;
      }
    }
    double top = total[0];
    for (int idx = 1; idx < num_top; idx++) {
      if (total[idx] > top) {
        top = total[idx];
      }
    }
    out->sim_loss =
        fmax(0.0, (top - total[observed_candidate]) / (double)job->rollouts);
    out->usable = true;
  }
  move_list_destroy(best);
  move_list_destroy(moves);
  game_destroy(roll);
  game_destroy(work);
  return NULL;
}

// Solves (matrix) x = rhs in place for a symmetric positive definite matrix
// of size dim, by Cholesky. Returns false if it is not positive definite.
static bool steer_solve(double *matrix, double *rhs, int dim) {
  for (int col = 0; col < dim; col++) {
    double diagonal = matrix[((size_t)col * dim) + col];
    for (int prior = 0; prior < col; prior++) {
      const double entry = matrix[((size_t)col * dim) + prior];
      diagonal -= entry * entry;
    }
    if (diagonal <= 0) {
      return false;
    }
    diagonal = sqrt(diagonal);
    matrix[((size_t)col * dim) + col] = diagonal;
    for (int row = col + 1; row < dim; row++) {
      double entry = matrix[((size_t)row * dim) + col];
      for (int prior = 0; prior < col; prior++) {
        entry -= matrix[((size_t)row * dim) + prior] *
                 matrix[((size_t)col * dim) + prior];
      }
      matrix[((size_t)row * dim) + col] = entry / diagonal;
    }
  }
  for (int row = 0; row < dim; row++) {
    double value = rhs[row];
    for (int prior = 0; prior < row; prior++) {
      value -= matrix[((size_t)row * dim) + prior] * rhs[prior];
    }
    rhs[row] = value / matrix[((size_t)row * dim) + row];
  }
  for (int row = dim - 1; row >= 0; row--) {
    double value = rhs[row];
    for (int later = row + 1; later < dim; later++) {
      value -= matrix[((size_t)later * dim) + row] * rhs[later];
    }
    rhs[row] = value / matrix[((size_t)row * dim) + row];
  }
  return true;
}

// The columns of a leave's feature row: 0 intercept, 1 static loss, then a
// count for each letter, then an indicator for each pair in pair_column.
static int steer_features(const Rack *leave, double static_loss, int ld_size,
                          int pair_column[MAX_ALPHABET_SIZE][MAX_ALPHABET_SIZE],
                          int *columns, double *values) {
  int num = 0;
  columns[num] = 0;
  values[num++] = 1;
  columns[num] = 1;
  values[num++] = static_loss;
  for (int first = 0; first < ld_size; first++) {
    const int first_count = rack_get_letter(leave, first);
    if (first_count == 0) {
      continue;
    }
    columns[num] = 2 + first;
    values[num++] = first_count;
    for (int second = first; second < ld_size; second++) {
      const bool present = second == first ? first_count > 1
                                           : rack_get_letter(leave, second) > 0;
      if (present && pair_column[first][second] >= 0) {
        columns[num] = pair_column[first][second];
        values[num++] = 1;
      }
    }
  }
  return num;
}

// Fits `model` for the mover's play `observed` in `base` (the position before
// it, mover on turn). Returns the number of racks fitted on, 0 when there
// were too few to fit and the model must not be used.
static int steer_fit(const Game *base, int mover, const Move *observed,
                     const Rack *played, int threads, int num_racks,
                     int rollouts, uint64_t seed, InferenceLossModel *model,
                     double *mean_static, double *mean_sim) {
  assert(num_racks <= STEER_MAX_RACKS);
  const int ld_size = ld_get_size(game_get_ld(base));
  SteerRack *racks = malloc_or_die((size_t)num_racks * sizeof(SteerRack));
  SteerJob *jobs = malloc_or_die((size_t)threads * sizeof(SteerJob));
  cpthread_t *workers = malloc_or_die((size_t)threads * sizeof(cpthread_t));
  for (int worker = 0; worker < threads; worker++) {
    jobs[worker] = (SteerJob){
        .base = base,
        .observed = observed,
        .played = played,
        .mover = mover,
        .rollouts = rollouts,
        .seed = seed,
        .first = (int)(((int64_t)num_racks * worker) / threads),
        .last = (int)(((int64_t)num_racks * (worker + 1)) / threads),
        .racks = racks,
    };
    cpthread_create(&workers[worker], steer_worker, &jobs[worker]);
  }
  for (int worker = 0; worker < threads; worker++) {
    cpthread_join(workers[worker]);
  }
  free(workers);
  free(jobs);
  const double clip = 40;
  int pair_count[MAX_ALPHABET_SIZE][MAX_ALPHABET_SIZE];
  int pair_column[MAX_ALPHABET_SIZE][MAX_ALPHABET_SIZE];
  memset(pair_count, 0, sizeof(pair_count));
  int num_fit = 0;
  for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    SteerRack *rack = &racks[rack_idx];
    rack->usable = rack->usable && rack->static_loss <= clip;
    if (!rack->usable) {
      continue;
    }
    num_fit++;
    for (int first = 0; first < ld_size; first++) {
      const int first_count = rack_get_letter(&rack->leave, first);
      if (first_count == 0) {
        continue;
      }
      pair_count[first][first] += first_count > 1;
      for (int second = first + 1; second < ld_size; second++) {
        pair_count[first][second] += rack_get_letter(&rack->leave, second) > 0;
      }
    }
  }
  if (num_fit < STEER_MIN_FIT) {
    free(racks);
    return 0;
  }
  int dim = 2 + ld_size;
  for (int first = 0; first < ld_size; first++) {
    for (int second = 0; second < ld_size; second++) {
      pair_column[first][second] = -1;
      if (second >= first &&
          pair_count[first][second] >= STEER_MIN_PAIR_COUNT) {
        pair_column[first][second] = dim++;
      }
    }
  }
  double *matrix = calloc_or_die((size_t)dim * dim, sizeof(double));
  double *rhs = calloc_or_die((size_t)dim, sizeof(double));
  int columns[2 + RACK_SIZE + (RACK_SIZE * RACK_SIZE)];
  double values[2 + RACK_SIZE + (RACK_SIZE * RACK_SIZE)];
  double static_sum = 0;
  double sim_sum = 0;
  for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    const SteerRack *rack = &racks[rack_idx];
    if (!rack->usable) {
      continue;
    }
    const double target = fmin(rack->sim_loss, clip);
    static_sum += rack->static_loss;
    sim_sum += target;
    const int num = steer_features(&rack->leave, rack->static_loss, ld_size,
                                   pair_column, columns, values);
    for (int left = 0; left < num; left++) {
      rhs[columns[left]] += values[left] * target;
      for (int right = 0; right < num; right++) {
        matrix[((size_t)columns[left] * dim) + columns[right]] +=
            values[left] * values[right];
      }
    }
  }
  for (int col = 0; col < dim; col++) {
    double penalty = 30;
    if (col == 0) {
      penalty = 1e-6;
    } else if (col == 1) {
      penalty = 1e-3;
    } else if (col < 2 + ld_size) {
      penalty = 10;
    }
    matrix[((size_t)col * dim) + col] += penalty;
  }
  const bool solved = steer_solve(matrix, rhs, dim);
  if (solved) {
    memset(model, 0, sizeof(*model));
    model->intercept = rhs[0];
    model->slope = rhs[1];
    model->clip = clip;
    for (int ml = 0; ml < ld_size; ml++) {
      model->letter[ml] = rhs[2 + ml];
      for (int second = ml; second < ld_size; second++) {
        if (pair_column[ml][second] >= 0) {
          model->pair[ml][second] = rhs[pair_column[ml][second]];
        }
      }
    }
    *mean_static = static_sum / num_fit;
    *mean_sim = sim_sum / num_fit;
  }
  free(rhs);
  free(matrix);
  free(racks);
  return solved ? num_fit : 0;
}

// Copies `keep` tiles of `actual`, chosen uniformly without replacement.
static void decide_partial_rack(const Rack *actual, int keep, uint64_t seed,
                                Rack *out) {
  MachineLetter tiles[RACK_SIZE];
  int count = 0;
  const int dist_size = rack_get_dist_size(actual);
  for (int ml = 0; ml < dist_size; ml++) {
    for (int copy = 0; copy < (int)rack_get_letter(actual, (MachineLetter)ml);
         copy++) {
      if (count < RACK_SIZE) {
        tiles[count++] = (MachineLetter)ml;
      }
    }
  }
  rack_reset(out);
  uint64_t state = seed;
  for (int idx = 0; idx < keep && idx < count; idx++) {
    // splitmix64
    state += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    mixed = (mixed ^ (mixed >> 27)) * UINT64_C(0x94D049BB133111EB);
    mixed ^= mixed >> 31;
    const int pick = idx + (int)(mixed % (uint64_t)(count - idx));
    const MachineLetter chosen = tiles[pick];
    tiles[pick] = tiles[idx];
    tiles[idx] = chosen;
    rack_add_letter(out, chosen);
  }
}

static void decide_move_string(const Game *game, const Move *move, char *dest) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          true);
  const char *text = string_builder_peek(sb);
  size_t out = 0;
  for (size_t in = 0; text[in] != '\0' && out + 1 < DECIDE_MOVE_STR; in++) {
    dest[out++] = text[in] == ' ' ? '_' : text[in];
  }
  dest[out] = '\0';
  string_builder_destroy(sb);
}

static void decide_rack_string(const Game *game, const Rack *rack, char *dest,
                               size_t dest_size) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, rack, game_get_ld(game), false);
  const char *text = string_builder_peek(sb);
  snprintf(dest, dest_size, "%s", text[0] == '\0' ? "-" : text);
  string_builder_destroy(sb);
}

// Arm 'g': the placement temperature follows the bag before the observed
// move, interpolated in log temperature between knots and held flat outside
// them. INFDEC_SCHEDULE overrides the knots as "bag:T,bag:T,..." (ascending).
static double decide_schedule_temperature(int bag) {
  static int bags[16];
  static double temps[16];
  static int knots = 0;
  if (knots == 0) {
    const char *text = getenv("INFDEC_SCHEDULE");
    if (text == NULL || text[0] == '\0') {
      text = "4:12,10:8,17:5,25:3,38:3,53:2,75:1.5";
    }
    while (knots < 16 &&
           sscanf(text, "%d:%lf", &bags[knots], &temps[knots]) == 2) {
      knots++;
      text = strchr(text, ',');
      if (text == NULL) {
        break;
      }
      text++;
    }
    assert(knots > 0);
  }
  if (bag <= bags[0]) {
    return temps[0];
  }
  for (int idx = 1; idx < knots; idx++) {
    if (bag <= bags[idx]) {
      const double weight =
          (double)(bag - bags[idx - 1]) / (bags[idx] - bags[idx - 1]);
      return exp((1 - weight) * log(temps[idx - 1]) + weight * log(temps[idx]));
    }
  }
  return temps[knots - 1];
}

// Logs what an arm's inference believed about the opponent's kept tiles
// next to the truth: the weight on the true leave and the posterior means of
// a few leave features.
static void decide_log_shape(const InferenceResults *results, const Game *game,
                             int target, const Rack *truth, uint64_t seed,
                             int game_idx, int turn, char arm_code, int bag,
                             bool exchange) {
  const AliasMethod *am = inference_results_get_alias_method(results);
  if (am == NULL || am->num_items == 0 || rack_is_empty(truth)) {
    return;
  }
  const LetterDistribution *ld = game_get_ld(game);
  const KLV *klv = player_get_klv(game_get_player(game, target));
  const MachineLetter s_letter = ld_hl_to_ml(ld, "S");
  Rack rack;
  rack_set_dist_size_and_reset(&rack, rack_get_dist_size(truth));
  double true_weight = 0;
  double blanks = 0;
  double esses = 0;
  double any_blank = 0;
  double any_s = 0;
  double vowels = 0;
  double leave_value = 0;
  double leave_value_sq = 0;
  int true_vowels = 0;
  for (int ml = 0; ml < rack_get_dist_size(truth); ml++) {
    if (ld_get_is_vowel(ld, (MachineLetter)ml)) {
      true_vowels += rack_get_letter(truth, ml);
    }
  }
  for (uint32_t idx = 0; idx < am->num_items; idx++) {
    const AliasMethodItem *item = &am->items[idx];
    for (int part = 0; part < 2; part++) {
      const double mass =
          (part == 0 ? item->probability : 1 - item->probability) /
          am->num_items;
      if (mass <= 0) {
        continue;
      }
      rack_decode(part == 0 ? &item->rack : &am->items[item->alias].rack,
                  &rack);
      if (racks_are_equal(&rack, truth)) {
        true_weight += mass;
      }
      const int rack_blanks = rack_get_letter(&rack, BLANK_MACHINE_LETTER);
      const int rack_esses = rack_get_letter(&rack, s_letter);
      blanks += mass * rack_blanks;
      esses += mass * rack_esses;
      any_blank += rack_blanks > 0 ? mass : 0;
      any_s += rack_esses > 0 ? mass : 0;
      for (int ml = 0; ml < rack_get_dist_size(&rack); ml++) {
        if (ld_get_is_vowel(ld, (MachineLetter)ml)) {
          vowels += mass * rack_get_letter(&rack, ml);
        }
      }
      const double value = equity_to_double(klv_get_leave_value(klv, &rack));
      leave_value += mass * value;
      leave_value_sq += mass * value * value;
    }
  }
  printf("INFDEC_SHAPE seed=%llu game=%d turn=%d arm=%c bag=%d exch=%d "
         "kept=%d items=%u p_true=%.6g blank=%.4f/%d s=%.4f/%d "
         "any_blank=%.4f any_s=%.4f vowels=%.3f/%d klv=%.3f/%.3f "
         "klv_sd=%.3f\n",
         (unsigned long long)seed, game_idx, turn, arm_code, bag, exchange,
         rack_get_total_letters(truth), am->num_items, true_weight, blanks,
         rack_get_letter(truth, BLANK_MACHINE_LETTER), esses,
         rack_get_letter(truth, s_letter), any_blank, any_s, vowels,
         true_vowels, leave_value,
         equity_to_double(klv_get_leave_value(klv, truth)),
         sqrt(fmax(0.0, leave_value_sq - (leave_value * leave_value))));
}

void infer_decide_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  assert(string_splitter_get_number_of_items(fields) == 6);
  ErrorStack *errors = error_stack_create();
  const int games = string_to_int(string_splitter_get_item(fields, 0), errors);
  const int move_ms =
      string_to_int(string_splitter_get_item(fields, 1), errors);
  const int threads =
      string_to_int(string_splitter_get_item(fields, 2), errors);
  const uint64_t seed =
      string_to_uint64(string_splitter_get_item(fields, 3), errors);
  char arm_codes[DECIDE_MAX_ARMS + 1];
  assert(strlen(string_splitter_get_item(fields, 4)) <= DECIDE_MAX_ARMS);
  snprintf(arm_codes, sizeof(arm_codes), "%s",
           string_splitter_get_item(fields, 4));
  const int playouts =
      string_to_int(string_splitter_get_item(fields, 5), errors);
  assert(error_stack_is_empty(errors) && games > 0 && move_ms > 0 &&
         threads > 0 && playouts > 1);
  string_splitter_destroy(fields);
  error_stack_destroy(errors);
  const int num_extra = (int)strlen(arm_codes);
  for (int idx = 0; idx < num_extra; idx++) {
    assert(strchr("spctabh124vkjloxgmry", arm_codes[idx]) != NULL);
  }
  const double move_seconds = move_ms / 1000.0;
  const int steer_racks = sfit_env_int("INFDEC_STEER_RACKS", 150);
  const int steer_rollouts = sfit_env_int("INFDEC_STEER_ROLLOUTS", 20);
  const char *steer_scale_text = getenv("INFDEC_STEER_TSCALE");
  const double steer_temperature_scale =
      steer_scale_text ? atof(steer_scale_text) : 0.5;
  const char *equal_time_text = getenv("INFDEC_EQUAL_TIME_FACTOR");
  const double equal_time_factor =
      equal_time_text ? atof(equal_time_text) : 1.0;
  InferenceLossModel steer_model;
  // INFDEC_ITERATIONS=N gives every search exactly N rollouts: no stopping
  // rule and no clock, so the arms differ only in what they know.
  // INFDEC_EXCH_BAG is the most tiles in the bag at which an exchange is
  // inferred from (30 by default, a time guard for short clocks).
  const char *iterations_text = getenv("INFDEC_ITERATIONS");
  const uint64_t fixed_iterations =
      iterations_text ? strtoull(iterations_text, NULL, 10) : 0;
  // INFDEC_EXCH_TEMP is the softmax temperature used after an exchange by
  // the temperature-8 arms (s and o); unset, they use 8 there too.
  const char *exchange_temp_text = getenv("INFDEC_EXCH_TEMP");
  const int exchange_temperature =
      exchange_temp_text ? (int)strtol(exchange_temp_text, NULL, 10) : 8;
  const char *exchange_bag_text = getenv("INFDEC_EXCH_BAG");
  const int exchange_bag = exchange_bag_text
                               ? (int)strtol(exchange_bag_text, NULL, 10)
                               : DECIDE_DEFAULT_EXCHANGE_BAG;

  // Engine defaults for the search (candidates, plies, stopping rule, PAT),
  // with the tables on. Apply overrides as a separate set command so that
  // an option already in the defaults (such as -minp) can be overridden.
  const char *extra_settings = getenv("INFDEC_SET");
  char *settings = get_formatted_string(
      "set -lex CSW24 -leaves CSW24 -wmp true -rit true -ritmmap true -wit "
      "true -s1 equity -s2 equity -r1 all -r2 all -pat auto -threads %d "
      "-numplays 100 -minp 500 -savesettings false -autosavegcg false",
      threads);
  Config *config = config_create_or_die(settings);
  if (extra_settings && extra_settings[0]) {
    char *overrides = get_formatted_string("set %s", extra_settings);
    load_and_exec_config_or_die(config, overrides);
    free(overrides);
  }
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex CSW24;");
  errors = error_stack_create();
  config_load_win_pcts(config, errors);
  assert(error_stack_is_empty(errors) && config_get_win_pcts(config) != NULL);
  Game *game = config_get_game(config);
  for (int seat = 0; seat < 2; seat++) {
    const Player *player = game_get_player(game, seat);
    if (player_get_pat(player) == NULL || player_get_pat_disabled(player)) {
      log_fatal("infdecide: PAT not active for player %d", seat);
    }
  }
  const int num_plays = config_get_num_plays(config);
  printf("INFDEC_SETTINGS games=%d move_ms=%d threads=%d seed=%llu arms=c0%s "
         "playouts=%d plies=%d candidates=%d oracle_sim_rollouts=%d "
         "temperature=8 margin=10000 lex=CSW24 fixed_iterations=%llu "
         "exchange_bag=%d exchange_temperature=%d settings=\"%s\" "
         "overrides=\"%s\"\n",
         games, move_ms, threads, (unsigned long long)seed, arm_codes, playouts,
         config_get_plies(config), num_plays,
         DECIDE_ORACLE_SIM_ROLLOUTS_PER_MOVE,
         (unsigned long long)fixed_iterations, exchange_bag,
         exchange_temperature, settings, extra_settings ? extra_settings : "");
  free(settings);
  printf("INFDEC_SCHEDULE env=\"%s\" T(2)=%.2f T(7)=%.2f T(14)=%.2f T(20)=%.2f "
         "T(30)=%.2f T(45)=%.2f T(60)=%.2f T(86)=%.2f\n",
         getenv("INFDEC_SCHEDULE") ? getenv("INFDEC_SCHEDULE") : "",
         decide_schedule_temperature(2), decide_schedule_temperature(7),
         decide_schedule_temperature(14), decide_schedule_temperature(20),
         decide_schedule_temperature(30), decide_schedule_temperature(45),
         decide_schedule_temperature(60), decide_schedule_temperature(86));
  fflush(stdout);

  const LetterDistribution *ld = game_get_ld(game);
  Rack played;
  Rack known;
  Rack observer;
  Rack opp_actual;
  Rack no_rack;
  Rack partial;
  Rack true_leave;
  rack_set_dist_size_and_reset(&played, ld_get_size(ld));
  rack_set_dist_size_and_reset(&known, ld_get_size(ld));
  rack_set_dist_size_and_reset(&observer, ld_get_size(ld));
  rack_set_dist_size_and_reset(&opp_actual, ld_get_size(ld));
  rack_set_dist_size_and_reset(&no_rack, ld_get_size(ld));
  rack_set_dist_size_and_reset(&partial, ld_get_size(ld));
  rack_set_dist_size_and_reset(&true_leave, ld_get_size(ld));
  MoveList *moves = move_list_create(num_plays);
  MoveList *static_best = move_list_create(1);
  MoveList *oracle_moves = move_list_create(DECIDE_MAX_ARMS + 1);
  SimResults *results = sim_results_create(0);
  SimResults *oracle_results = sim_results_create(0);
  InferenceResults *inferences = inference_results_create(NULL);
  ThreadControl *control = config_get_thread_control(config);
  // Arm 0 is the control; the rest follow arm_codes.
  DecideArm arms[DECIDE_MAX_ARMS + 1] = {0};
  const int num_arms = num_extra + 1;
  arms[0].code = '0';
  for (int idx = 0; idx < num_extra; idx++) {
    arms[idx + 1].code = arm_codes[idx];
  }
  float *playout_win =
      malloc_or_die(sizeof(float) * (size_t)num_arms * (size_t)playouts);
  float *playout_spread =
      malloc_or_die(sizeof(float) * (size_t)num_arms * (size_t)playouts);
  PlayoutJob *jobs = malloc_or_die(sizeof(PlayoutJob) * (size_t)threads);
  cpthread_t *workers = malloc_or_die(sizeof(cpthread_t) * (size_t)threads);

  // Arm m: schedule temperature plus carry-over of each player's inferred
  // leave from turn to turn. One pair of tables per inferred player.
  const uint32_t num_klv_leaves =
      klv_get_number_of_leaves(player_get_klv(game_get_player(game, 0)));
  InferenceCarryoverSlot *carry_table[2][2];
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    for (int table_idx = 0; table_idx < 2; table_idx++) {
      carry_table[player_idx][table_idx] =
          calloc_or_die(num_klv_leaves, sizeof(InferenceCarryoverSlot));
    }
  }
  int carry_current[2] = {0, 0};
  int carry_kept[2] = {0, 0};
  int carry_history[2] = {0, 0};
  double carry_floor[2] = {0, 0};

  for (int game_idx = 0; game_idx < games; game_idx++) {
    carry_history[0] = 0;
    carry_history[1] = 0;
    const uint64_t game_seed_value = seed + (uint64_t)game_idx;
    game_reset(game);
    game_seed(game, game_seed_value);
    game_set_starting_player_index(game, game_idx % 2);
    draw_starting_racks(game);
    Game *previous = game_duplicate(game);
    Move previous_move = {0};
    for (int turn = 0; !game_over(game) && turn < DECIDE_MAX_TURNS; turn++) {
      const int bag_tiles = bag_get_letters(game_get_bag(game));
      if (bag_tiles == 0) {
        break;
      }
      const int seat = game_get_player_on_turn_index(game);
      const bool eligible =
          turn > 0 && move_get_type(&previous_move) != GAME_EVENT_PASS &&
          (move_get_type(&previous_move) != GAME_EVENT_EXCHANGE ||
           bag_get_letters(game_get_bag(previous)) <= exchange_bag);
      rack_copy(&opp_actual, player_get_rack(game_get_player(game, 1 - seat)));
      rack_copy(&observer, player_get_rack(game_get_player(game, seat)));
      rack_reset(&played);
      if (move_get_type(&previous_move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        for (int tile_idx = 0; tile_idx < move_get_tiles_length(&previous_move);
             tile_idx++) {
          const MachineLetter ml = move_get_tile(&previous_move, tile_idx);
          if (ml != PLAYED_THROUGH_MARKER) {
            rack_add_letter(&played,
                            get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
          }
        }
      }
      const uint64_t sim_seed = (game_seed_value * UINT64_C(1000003)) +
                                ((uint64_t)turn * UINT64_C(7919));
      if (!eligible && turn > 0 &&
          move_get_type(&previous_move) != GAME_EVENT_PASS) {
        // No inference this turn: the chain for that player starts again.
        // After a pass their rack, and so the table, is unchanged.
        carry_history[1 - seat] = 0;
      }
      int candidates = 0;
      for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
        DecideArm *arm = &arms[arm_idx];
        arm->ran = arm_idx == 0 || eligible;
        arm->simulated = false;
        arm->iterations = 0;
        arm->seconds = 0;
        arm->distinct_index = -1;
        if (!arm->ran) {
          continue;
        }
        Timer timer;
        ctimer_start(&timer);
        if (arm->code == 't') {
          move_copy(&arm->move, get_top_equity_move(game, static_best));
          arm->seconds = ctimer_elapsed_seconds(&timer);
          decide_move_string(game, &arm->move, arm->move_str);
          continue;
        }
        if (arm->code == 'l') {
          // With no kept tiles to reveal (after a bingo, an exchange or a
          // pass) this arm is the control: copy its move so that search
          // noise is not counted as an effect of the arm.
          bool informative =
              move_get_type(&previous_move) == GAME_EVENT_TILE_PLACEMENT_MOVE;
          if (informative) {
            rack_copy(&true_leave,
                      player_get_rack(game_get_player(previous, 1 - seat)));
            rack_subtract_using_floor_zero(&true_leave, &played);
            informative = !rack_is_empty(&true_leave);
          }
          if (!informative) {
            move_copy(&arm->move, &arms[0].move);
            arm->seconds = 0;
            decide_move_string(game, &arm->move, arm->move_str);
            continue;
          }
        }
        const bool feature_oracle = strchr("vkj", arm->code) != NULL;
        const bool use_inference =
            strchr("sabhogmry", arm->code) != NULL ||
            (feature_oracle &&
             move_get_type(&previous_move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
        Rack *arm_known_rack = &no_rack;
        if (arm->code == 'p') {
          arm_known_rack = &opp_actual;
        } else if (arm->code == 'l' && move_get_type(&previous_move) ==
                                           GAME_EVENT_TILE_PLACEMENT_MOVE) {
          rack_copy(&true_leave,
                    player_get_rack(game_get_player(previous, 1 - seat)));
          rack_subtract_using_floor_zero(&true_leave, &played);
          arm_known_rack = &true_leave;
        } else if (arm->code >= '1' && arm->code <= '6') {
          decide_partial_rack(&opp_actual, arm->code - '0',
                              sim_seed + UINT64_C(424243), &partial);
          arm_known_rack = &partial;
        }
        SimArgs args;
        config_fill_sim_args(config, arm_known_rack, &played, &observer, &known,
                             &args);
        args.move_list = moves;
        args.num_plays = num_plays;
        args.game = game;
        args.inference_results = inferences;
        args.seed = arm->code == 'c' ? sim_seed + UINT64_C(7777777) : sim_seed;
        args.bai_options.time_limit_seconds = move_seconds;
        if (fixed_iterations > 0) {
          args.bai_options.time_limit_seconds = 0;
          args.bai_options.threshold = BAI_THRESHOLD_NONE;
          args.bai_options.sample_limit = fixed_iterations;
        }
        if (arm->code == 'x') {
          args.bai_options.sample_limit *= DECIDE_MORE_SEARCH_FACTOR;
        }
        if (arm->code == 'y') {
          // Arm y: the schedule with more search, to match arm r's time.
          args.bai_options.sample_limit =
              (uint64_t)((double)args.bai_options.sample_limit *
                         equal_time_factor);
        }
        args.use_inference = use_inference;
        args.legacy_alias_seed = arm->code == 'o';
        if (use_inference) {
          infer_args_fill(&args.inference_args, 0,
                          int_to_equity(arm->code == 'h' ? 5 : 10000), NULL,
                          previous, threads, 0, 0, control, false, true,
                          1 - seat, move_get_score(&previous_move),
                          move_get_type(&previous_move) == GAME_EVENT_EXCHANGE
                              ? move_get_tiles_played(&previous_move)
                              : 0,
                          &played, &known, &observer);
          int arm_temperature =
              move_get_type(&previous_move) == GAME_EVENT_EXCHANGE
                  ? exchange_temperature
                  : 8;
          if (arm->code == 'a') {
            arm_temperature = 3;
          } else if (arm->code == 'b') {
            arm_temperature = 16;
          } else if (arm->code == 'h') {
            arm_temperature = 0;
          }
          args.inference_args.temperature = int_to_equity(arm_temperature);
          if ((arm->code == 'g' || arm->code == 'm' || arm->code == 'r' ||
               arm->code == 'y') &&
              move_get_type(&previous_move) != GAME_EVENT_EXCHANGE) {
            args.inference_args.temperature =
                double_to_equity(decide_schedule_temperature(
                    bag_get_letters(game_get_bag(previous))));
          }
          if (arm->code == 'r' &&
              move_get_type(&previous_move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
              move_get_tiles_played(&previous_move) < (RACK_SIZE) &&
              bag_get_letters(game_get_bag(previous)) > 0) {
            // Arm r: the schedule, with the loss of each rack steered by a
            // mini-sim of a sample of racks.
            Timer steer_timer;
            ctimer_start(&steer_timer);
            double mean_static = 0;
            double mean_sim = 0;
            const int fitted = steer_fit(
                previous, 1 - seat, &previous_move, &played, threads,
                steer_racks, steer_rollouts, sim_seed + UINT64_C(31337),
                &steer_model, &mean_static, &mean_sim);
            if (fitted > 0) {
              args.inference_args.loss_model = &steer_model;
              args.inference_args.temperature =
                  double_to_equity(steer_temperature_scale *
                                   decide_schedule_temperature(bag_get_letters(
                                       game_get_bag(previous))));
            }
            printf("INFDEC_STEER seed=%llu game=%d turn=%d bag=%d racks=%d "
                   "fitted=%d seconds=%.3f mean_static=%.3f mean_sim=%.3f "
                   "intercept=%.3f slope=%.3f\n",
                   (unsigned long long)seed, game_idx, turn,
                   bag_get_letters(game_get_bag(previous)), steer_racks, fitted,
                   ctimer_elapsed_seconds(&steer_timer), mean_static, mean_sim,
                   fitted > 0 ? steer_model.intercept : 0.0,
                   fitted > 0 ? steer_model.slope : 0.0);
          }
          if (feature_oracle) {
            rack_copy(&true_leave,
                      player_get_rack(game_get_player(previous, 1 - seat)));
            rack_subtract_using_floor_zero(&true_leave, &played);
            args.inference_args.oracle_leave = &true_leave;
            args.inference_args.oracle_tolerance = int_to_equity(2);
            args.inference_args.oracle_feature =
                arm->code == 'v' ? 1 : (arm->code == 'k' ? 2 : 3);
          }
          args.inference_args.target_move = &previous_move;
          if (arm->code == 'm') {
            const int target = 1 - seat;
            InferenceCarryoverSlot *table_out =
                carry_table[target][1 - carry_current[target]];
            memset(table_out, 0,
                   (size_t)num_klv_leaves * sizeof(InferenceCarryoverSlot));
            if (carry_history[target] > 0) {
              args.inference_args.carry_in =
                  carry_table[target][carry_current[target]];
              args.inference_args.carry_in_kept = carry_kept[target];
              args.inference_args.carry_in_floor = carry_floor[target];
            }
            args.inference_args.carry_out = table_out;
          }
        }
        thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
        const Move *move =
            get_top_simming_move(game, moves, &args, &arm->sim_ctx, results,
                                 &arm->simulated, errors);
        if (arm->code == 'm' && use_inference) {
          const int target = 1 - seat;
          const InferenceCarryoverSlot *table_out =
              carry_table[target][1 - carry_current[target]];
          float table_max = 0;
          for (uint32_t leave_idx = 0; leave_idx < num_klv_leaves;
               leave_idx++) {
            const uint32_t bits = atomic_load(&table_out[leave_idx]);
            float value;
            memcpy(&value, &bits, sizeof(value));
            if (value > table_max) {
              table_max = value;
            }
          }
          const int kept_now = rack_get_total_letters(player_get_rack(
                                   game_get_player(previous, target))) -
                               move_get_tiles_played(&previous_move);
          printf("INFDEC_CARRY seed=%llu game=%d turn=%d target=%d used=%d "
                 "kept_in=%d kept_out=%d table_max=%.4g\n",
                 (unsigned long long)seed, game_idx, turn, target,
                 carry_history[target], carry_kept[target], kept_now,
                 (double)table_max);
          if (table_max > 0 && kept_now > 0 && kept_now < (RACK_SIZE)) {
            carry_current[target] = 1 - carry_current[target];
            carry_kept[target] = kept_now;
            carry_floor[target] = 1e-3 * (double)table_max;
            carry_history[target]++;
          } else {
            carry_history[target] = 0;
          }
        }
        if (!error_stack_is_empty(errors) || move == NULL) {
          error_stack_print_and_reset(errors);
          log_fatal("infdecide failed: seed=%llu game=%d turn=%d arm=%c",
                    (unsigned long long)seed, game_idx, turn, arm->code);
        }
        move_copy(&arm->move, move);
        arm->seconds = ctimer_elapsed_seconds(&timer);
        if (arm->simulated && use_inference && !feature_oracle) {
          // The tiles the opponent really kept after their last move.
          Rack kept_truth;
          rack_copy(&kept_truth,
                    player_get_rack(game_get_player(previous, 1 - seat)));
          for (int tile_idx = 0;
               tile_idx < move_get_tiles_length(&previous_move); tile_idx++) {
            const MachineLetter ml = move_get_tile(&previous_move, tile_idx);
            // In an exchange every tile counts: a blank has the same value
            // as the marker for a square played through.
            if (move_get_type(&previous_move) == GAME_EVENT_EXCHANGE ||
                ml != PLAYED_THROUGH_MARKER) {
              const MachineLetter tile =
                  get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml;
              if (rack_get_letter(&kept_truth, tile) > 0) {
                rack_take_letter(&kept_truth, tile);
              }
            }
          }
          decide_log_shape(
              inferences, game, 1 - seat, &kept_truth, seed, game_idx, turn,
              arm->code, bag_get_letters(game_get_bag(previous)),
              move_get_type(&previous_move) == GAME_EVENT_EXCHANGE);
        }
        if (arm->simulated) {
          arm->iterations = sim_results_get_iteration_count(results);
          // The search's own estimates of its best plays, for modelling its
          // sampling error afterwards.
          const int sim_plays = sim_results_get_number_of_plays(results);
          bool logged[DECIDE_SIM_LOG_CAPACITY] = {false};
          printf("INFDEC_SIM seed=%llu game=%d turn=%d arm=%c plays=%d",
                 (unsigned long long)seed, game_idx, turn, arm->code,
                 sim_plays);
          for (int slot = 0; slot < DECIDE_SIM_LOG_PLAYS; slot++) {
            int best_idx = -1;
            double best_utility = 0;
            for (int play_idx = 0;
                 play_idx < sim_plays && play_idx < DECIDE_SIM_LOG_CAPACITY;
                 play_idx++) {
              const SimmedPlay *candidate =
                  sim_results_get_simmed_play(results, play_idx);
              const double utility =
                  stat_get_mean(simmed_play_get_utility_stat(candidate));
              if (!logged[play_idx] &&
                  simmed_play_get_num_rollouts(candidate) > 0 &&
                  (best_idx < 0 || utility > best_utility)) {
                best_idx = play_idx;
                best_utility = utility;
              }
            }
            if (best_idx < 0) {
              break;
            }
            logged[best_idx] = true;
            const SimmedPlay *top =
                sim_results_get_simmed_play(results, best_idx);
            char top_str[DECIDE_MOVE_STR];
            decide_move_string(game, simmed_play_get_move(top), top_str);
            const Stat *utility_stat = simmed_play_get_utility_stat(top);
            const Stat *win_stat = simmed_play_get_win_pct_stat(top);
            const Stat *equity_stat = simmed_play_get_equity_stat(top);
            printf(" %s,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%.4f,%.4f", top_str,
                   (unsigned long long)simmed_play_get_num_rollouts(top),
                   (unsigned long long)stat_get_num_samples(utility_stat),
                   stat_get_mean(utility_stat), stat_get_sem(utility_stat),
                   stat_get_mean(win_stat), stat_get_sem(win_stat),
                   stat_get_mean(equity_stat), stat_get_sem(equity_stat));
          }
          printf("\n");
        }
        if (arm_idx == 0) {
          candidates = move_list_get_count(moves);
        }
        decide_move_string(game, &arm->move, arm->move_str);
      }

      // Distinct moves, the control's first.
      Move distinct[DECIDE_MAX_ARMS + 1];
      char distinct_str[DECIDE_MAX_ARMS + 1][DECIDE_MOVE_STR];
      int num_distinct = 0;
      for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
        DecideArm *arm = &arms[arm_idx];
        if (!arm->ran) {
          continue;
        }
        for (int idx = 0; idx < num_distinct; idx++) {
          if (strcmp(distinct_str[idx], arm->move_str) == 0) {
            arm->distinct_index = idx;
            break;
          }
        }
        if (arm->distinct_index < 0) {
          arm->distinct_index = num_distinct;
          move_copy(&distinct[num_distinct], &arm->move);
          snprintf(distinct_str[num_distinct], DECIDE_MOVE_STR, "%s",
                   arm->move_str);
          num_distinct++;
        }
      }

      char rack_str[32];
      char opp_str[32];
      decide_rack_string(game, &observer, rack_str, sizeof(rack_str));
      decide_rack_string(game, &opp_actual, opp_str, sizeof(opp_str));
      printf("INFDEC_POS seed=%llu game=%d turn=%d seat=%d bag=%d eligible=%d "
             "candidates=%d rack=%s opp_rack=%s distinct=%d",
             (unsigned long long)seed, game_idx, turn, seat, bag_tiles,
             eligible, candidates, rack_str, opp_str, num_distinct);
      for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
        const DecideArm *arm = &arms[arm_idx];
        if (arm->ran) {
          printf(" %c_move=%s %c_iterations=%llu %c_seconds=%.3f", arm->code,
                 arm->move_str, arm->code, (unsigned long long)arm->iterations,
                 arm->code, arm->seconds);
        }
      }
      printf("\n");
      fflush(stdout);

      double sim_win[DECIDE_MAX_ARMS + 1] = {0};
      double sim_utility[DECIDE_MAX_ARMS + 1] = {0};
      double sim_equity[DECIDE_MAX_ARMS + 1] = {0};
      uint64_t sim_rollouts[DECIDE_MAX_ARMS + 1] = {0};
      double oracle_seconds = 0;
      if (num_distinct > 1) {
        Timer oracle_timer;
        ctimer_start(&oracle_timer);
        // Playout oracle.
        const uint64_t playout_seed = (sim_seed * UINT64_C(31)) + 17;
        for (int worker = 0; worker < threads; worker++) {
          jobs[worker] = (PlayoutJob){
              .base = game,
              .moves = distinct,
              .num_moves = num_distinct,
              .mover = seat,
              .seed_base = playout_seed,
              .first = (int)(((int64_t)playouts * worker) / threads),
              .last = (int)(((int64_t)playouts * (worker + 1)) / threads),
              .win = playout_win,
              .spread = playout_spread,
              .playouts = playouts,
          };
          cpthread_create(&workers[worker], decide_playout_worker,
                          &jobs[worker]);
        }
        for (int worker = 0; worker < threads; worker++) {
          cpthread_join(workers[worker]);
        }
        // Sim oracle: equal samples of just these moves, opponent rack known.
        move_list_reset(oracle_moves);
        for (int idx = 0; idx < num_distinct; idx++) {
          move_list_add_move(oracle_moves, &distinct[idx]);
        }
        SimArgs args;
        config_fill_sim_args(config, &opp_actual, &played, &observer, &known,
                             &args);
        args.move_list = oracle_moves;
        args.num_plays = num_distinct;
        args.game = game;
        args.inference_results = inferences;
        args.use_inference = false;
        args.seed = sim_seed + UINT64_C(424243);
        args.bai_options.threshold = BAI_THRESHOLD_NONE;
        args.bai_options.sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN;
        args.bai_options.sample_limit =
            (uint64_t)DECIDE_ORACLE_SIM_ROLLOUTS_PER_MOVE *
            (uint64_t)num_distinct;
        args.bai_options.time_limit_seconds = 120;
        thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
        SimCtx *oracle_ctx = NULL;
        simulate(&args, &oracle_ctx, oracle_results, errors);
        if (!error_stack_is_empty(errors)) {
          error_stack_print_and_reset(errors);
          log_fatal("infdecide oracle sim failed: seed=%llu game=%d turn=%d",
                    (unsigned long long)seed, game_idx, turn);
        }
        const int oracle_plays =
            sim_results_get_number_of_plays(oracle_results);
        for (int play_idx = 0; play_idx < oracle_plays; play_idx++) {
          const SimmedPlay *simmed =
              sim_results_get_simmed_play(oracle_results, play_idx);
          char simmed_str[DECIDE_MOVE_STR];
          decide_move_string(game, simmed_play_get_move(simmed), simmed_str);
          for (int idx = 0; idx < num_distinct; idx++) {
            if (strcmp(distinct_str[idx], simmed_str) == 0) {
              sim_win[idx] =
                  stat_get_mean(simmed_play_get_win_pct_stat(simmed));
              sim_utility[idx] =
                  stat_get_mean(simmed_play_get_utility_stat(simmed));
              sim_equity[idx] =
                  stat_get_mean(simmed_play_get_equity_stat(simmed));
              sim_rollouts[idx] = simmed_play_get_num_rollouts(simmed);
            }
          }
        }
        sim_ctx_destroy(oracle_ctx);
        oracle_seconds = ctimer_elapsed_seconds(&oracle_timer);
      }

      for (int arm_idx = 1; arm_idx < num_arms; arm_idx++) {
        const DecideArm *arm = &arms[arm_idx];
        if (!arm->ran) {
          continue;
        }
        const int idx = arm->distinct_index;
        if (idx == 0) {
          printf("INFDEC_ARM seed=%llu game=%d turn=%d seat=%d bag=%d arm=%c "
                 "same=1\n",
                 (unsigned long long)seed, game_idx, turn, seat, bag_tiles,
                 arm->code);
          continue;
        }
        double win_sum = 0;
        double win_sq = 0;
        double spread_sum = 0;
        double spread_sq = 0;
        double ctl_win = 0;
        double ctl_spread = 0;
        for (int playout = 0; playout < playouts; playout++) {
          const size_t arm_slot =
              ((size_t)idx * (size_t)playouts) + (size_t)playout;
          const double dwin =
              (double)playout_win[arm_slot] - (double)playout_win[playout];
          const double dspread = (double)playout_spread[arm_slot] -
                                 (double)playout_spread[playout];
          win_sum += dwin;
          win_sq += dwin * dwin;
          spread_sum += dspread;
          spread_sq += dspread * dspread;
          ctl_win += playout_win[playout];
          ctl_spread += playout_spread[playout];
        }
        const double count = playouts;
        const double dwin_mean = win_sum / count;
        const double dspread_mean = spread_sum / count;
        const double dwin_se = sqrt(
            fmax(0, (win_sq / count) - (dwin_mean * dwin_mean)) / (count - 1));
        const double dspread_se =
            sqrt(fmax(0, (spread_sq / count) - (dspread_mean * dspread_mean)) /
                 (count - 1));
        printf("INFDEC_ARM seed=%llu game=%d turn=%d seat=%d bag=%d arm=%c "
               "same=0 move=%s ctl_move=%s playouts=%d ctl_win=%.6f "
               "ctl_spread=%.4f dwin=%.6f dwin_se=%.6f dspread=%.4f "
               "dspread_se=%.4f sim_dwin=%.6f sim_dutility=%.6f "
               "sim_dequity=%.4f sim_rollouts=%llu sim_ctl_rollouts=%llu "
               "oracle_seconds=%.3f\n",
               (unsigned long long)seed, game_idx, turn, seat, bag_tiles,
               arm->code, arm->move_str, arms[0].move_str, playouts,
               ctl_win / count, ctl_spread / count, dwin_mean, dwin_se,
               dspread_mean, dspread_se, sim_win[idx] - sim_win[0],
               sim_utility[idx] - sim_utility[0],
               sim_equity[idx] - sim_equity[0],
               (unsigned long long)sim_rollouts[idx],
               (unsigned long long)sim_rollouts[0], oracle_seconds);
      }
      if (fflush(stdout) != 0) {
        log_fatal("infdecide output flush failed");
      }

      game_copy(previous, game);
      move_copy(&previous_move, &arms[0].move);
      play_move(&arms[0].move, game, NULL);
    }
    printf("INFDEC_GAME seed=%llu game=%d\n", (unsigned long long)seed,
           game_idx);
    fflush(stdout);
    game_destroy(previous);
  }

  for (int arm_idx = 0; arm_idx < num_arms; arm_idx++) {
    sim_ctx_destroy(arms[arm_idx].sim_ctx);
  }
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    for (int table_idx = 0; table_idx < 2; table_idx++) {
      free(carry_table[player_idx][table_idx]);
    }
  }
  free(workers);
  free(jobs);
  free(playout_spread);
  free(playout_win);
  inference_results_destroy(inferences);
  sim_results_destroy(oracle_results);
  sim_results_destroy(results);
  move_list_destroy(oracle_moves);
  move_list_destroy(static_best);
  move_list_destroy(moves);
  error_stack_destroy(errors);
  config_destroy(config);
}

// infrank: reads "CGP<TAB>move move ..." lines (moves in the INFDEC_POS
// format) and prints each move's 1-based rank among the position's static
// candidates, generated as the harness generates them. 0 means not found.
void infer_decide_rank_stdin(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -rit true -ritmmap true -wit "
      "true -s1 equity -s2 equity -r1 all -r2 all -pat auto -threads 1 "
      "-numplays 100 -savesettings false -autosavegcg false");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex CSW24;");
  ErrorStack *errors = error_stack_create();
  Game *game = config_get_game(config);
  MoveList *moves = move_list_create(DECIDE_RANK_CANDIDATES);
  char line[DECIDE_RANK_LINE];
  char move_str[DECIDE_MOVE_STR];
  while (fgets(line, sizeof(line), stdin)) {
    char *wanted = strchr(line, '\t');
    assert(wanted != NULL);
    *wanted++ = '\0';
    wanted[strcspn(wanted, "\r\n")] = '\0';
    game_load_cgp(game, line, errors);
    if (!error_stack_is_empty(errors)) {
      error_stack_print_and_reset(errors);
      log_fatal("infrank: bad cgp: %s", line);
    }
    const MoveGenArgs gen = {.game = game,
                             .move_list = moves,
                             .move_record_type = MOVE_RECORD_ALL,
                             .move_sort_type = MOVE_SORT_EQUITY,
                             .target_equity = EQUITY_MAX_VALUE};
    generate_moves(&gen);
    move_list_sort_moves(moves);
    const int count = move_list_get_count(moves);
    printf("RANK");
    for (char *token = strtok(wanted, " "); token != NULL;
         token = strtok(NULL, " ")) {
      int rank = 0;
      for (int idx = 0; idx < count && rank == 0; idx++) {
        decide_move_string(game, move_list_get_move(moves, idx), move_str);
        if (strcmp(move_str, token) == 0) {
          rank = idx + 1;
        }
      }
      printf(" %d", rank);
    }
    printf(" %d\n", count);
    fflush(stdout);
  }
  move_list_destroy(moves);
  error_stack_destroy(errors);
  config_destroy(config);
}

// inftfit: replays logged control games ("game turn rack move" lines on
// stdin, in log order; the seed comes from INFTFIT_SEED) and, before each
// move, scores how well inference at each temperature predicts the leave the
// mover really kept. Prints the posterior probability of the true leave.
static double tfit_true_probability(const InferenceResults *results,
                                    const Rack *truth) {
  const AliasMethod *am = inference_results_get_alias_method(results);
  Rack rack;
  rack_set_dist_size_and_reset(&rack, rack_get_dist_size(truth));
  double total = 0;
  for (uint32_t idx = 0; idx < am->num_items; idx++) {
    const AliasMethodItem *item = &am->items[idx];
    rack_decode(&item->rack, &rack);
    if (racks_are_equal(&rack, truth)) {
      total += item->probability / am->num_items;
    }
    rack_decode(&am->items[item->alias].rack, &rack);
    if (racks_are_equal(&rack, truth)) {
      total += (1 - item->probability) / am->num_items;
    }
  }
  return total;
}

void infer_decide_tfit_stdin(void) {
  // Negative: hard margin of that many points. Positive: temperature.
  static const double grid[] = {100000, 0.5, 1,  1.5, 2,  3,  4,  5,
                                6,      8,   10, 12,  16, 24, 32, -5};
  enum { GRID = sizeof(grid) / sizeof(grid[0]) };
  const char *seed_text = getenv("INFTFIT_SEED");
  assert(seed_text != NULL);
  const uint64_t seed = strtoull(seed_text, NULL, 10);
  const char *threads_text = getenv("INFTFIT_THREADS");
  const int threads = threads_text ? atoi(threads_text) : 16;
  // INFTFIT_CARRY=1: instead of the temperature grid, compare inference
  // with and without carry-over from the mover's earlier turns.
  const bool carry_mode = getenv("INFTFIT_CARRY") != NULL;
  // INFTFIT_POOL_FLOOR: the carry-over floor is 1e-3 of the largest weight
  // among the kept leaves the mover can still hold, as in play, instead of
  // the table's largest. INFTFIT_PRUNE=<fraction>: entries below that fraction
  // of the table's largest weight are dropped when it is recorded. Either one
  // adds the two maxima and the entry counts to each line.
  const bool carry_pool_floor = getenv("INFTFIT_POOL_FLOOR") != NULL;
  // INFTFIT_FLOOR=<fraction>: the floor as a fraction of the table's largest
  // weight (default 1e-3).
  const char *carry_floor_text = getenv("INFTFIT_FLOOR");
  const double carry_floor_fraction =
      carry_floor_text ? atof(carry_floor_text) : 1e-3;
  const char *carry_prune_text = getenv("INFTFIT_PRUNE");
  const double carry_prune = carry_prune_text ? atof(carry_prune_text) : 0.0;
  const bool carry_extra = carry_pool_floor || carry_prune_text != NULL;
  double carry_pool_max[2] = {0, 0};
  double carry_table_max[2] = {0, 0};
  const char *carry_temp_text = getenv("INFTFIT_CARRY_T");
  // INFTFIT_NO_TABLES: run on a machine without the rack and word info
  // tables, which only speed move generation up.
  char *settings = get_formatted_string(
      "set -lex CSW24 -leaves CSW24 -wmp true %s -s1 equity -s2 equity -r1 "
      "all -r2 all -pat auto -threads %d -numplays 100 -minp 500 "
      "-savesettings false -autosavegcg false",
      getenv("INFTFIT_NO_TABLES") != NULL ? "-rit false -wit false"
                                          : "-rit true -ritmmap true -wit true",
      threads);
  Config *config = config_create_or_die(settings);
  free(settings);
  load_and_exec_config_or_die(config, "set -plies 4");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex CSW24;");
  ErrorStack *errors = error_stack_create();
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  ThreadControl *control = config_get_thread_control(config);
  printf(
      "TFIT_SETTINGS seed=%llu peg0=%d peg1=%d grid=", (unsigned long long)seed,
      player_get_peg_adjust(game_get_player(game, 0)) != NULL,
      player_get_peg_adjust(game_get_player(game, 1)) != NULL);
  for (int idx = 0; idx < GRID; idx++) {
    printf("%s%g", idx ? "," : "", grid[idx]);
  }
  printf("\n");
  MoveList *moves = move_list_create(DECIDE_RANK_CANDIDATES);
  InferenceResults *inferences = inference_results_create(NULL);
  const uint32_t num_klv_leaves =
      klv_get_number_of_leaves(player_get_klv(game_get_player(game, 0)));
  InferenceCarryoverSlot *carry_table[2][2];
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    for (int table_idx = 0; table_idx < 2; table_idx++) {
      carry_table[player_idx][table_idx] =
          calloc_or_die(num_klv_leaves, sizeof(InferenceCarryoverSlot));
    }
  }
  int carry_current[2] = {0, 0};
  int carry_kept[2] = {0, 0};
  int carry_history[2] = {0, 0};
  double carry_floor[2] = {0, 0};
  Rack played, known, observer, truth;
  rack_set_dist_size_and_reset(&played, ld_get_size(ld));
  rack_set_dist_size_and_reset(&known, ld_get_size(ld));
  char line[DECIDE_RANK_LINE];
  char move_str[DECIDE_MOVE_STR];
  char rack_str[DECIDE_MOVE_STR];
  int bad_game = -1;
  while (fgets(line, sizeof(line), stdin)) {
    int game_idx;
    int turn;
    char want_rack[64];
    char want_move[DECIDE_MOVE_STR];
    if (sscanf(line, "%d %d %63s %95s", &game_idx, &turn, want_rack,
               want_move) != 4) {
      log_fatal("inftfit: bad line: %s", line);
    }
    if (game_idx == bad_game) {
      continue;
    }
    if (turn == 0) {
      game_reset(game);
      game_seed(game, seed + (uint64_t)game_idx);
      game_set_starting_player_index(game, game_idx % 2);
      draw_starting_racks(game);
      carry_history[0] = 0;
      carry_history[1] = 0;
    }
    const int seat = game_get_player_on_turn_index(game);
    const Rack *mover = player_get_rack(game_get_player(game, seat));
    decide_rack_string(game, mover, rack_str, sizeof(rack_str));
    if (strcmp(rack_str, want_rack) != 0) {
      printf("TFIT_BAD game=%d turn=%d rack=%s want=%s\n", game_idx, turn,
             rack_str, want_rack);
      bad_game = game_idx;
      continue;
    }
    const MoveGenArgs gen = {.game = game,
                             .move_list = moves,
                             .move_record_type = MOVE_RECORD_ALL,
                             .move_sort_type = MOVE_SORT_EQUITY,
                             .target_equity = EQUITY_MAX_VALUE};
    generate_moves(&gen);
    move_list_sort_moves(moves);
    const Move *found = NULL;
    int rank = 0;
    for (int idx = 0; idx < move_list_get_count(moves) && !found; idx++) {
      decide_move_string(game, move_list_get_move(moves, idx), move_str);
      if (strcmp(move_str, want_move) == 0) {
        found = move_list_get_move(moves, idx);
        rank = idx + 1;
      }
    }
    if (!found) {
      printf("TFIT_BAD game=%d turn=%d move=%s\n", game_idx, turn, want_move);
      bad_game = game_idx;
      continue;
    }
    Move move;
    move_copy(&move, found);
    const double best_equity =
        equity_to_double(move_get_equity(move_list_get_move(moves, 0)));
    const double loss = best_equity - equity_to_double(move_get_equity(&move));
    const bool exchange = move_get_type(&move) == GAME_EVENT_EXCHANGE;
    const int bag_tiles = bag_get_letters(game_get_bag(game));
    rack_reset(&played);
    rack_copy(&truth, mover);
    if (move_get_type(&move) != GAME_EVENT_PASS) {
      Rack used;
      rack_set_dist_size_and_reset(&used, ld_get_size(ld));
      for (int idx = 0; idx < move_get_tiles_length(&move); idx++) {
        const MachineLetter ml = move_get_tile(&move, idx);
        // In an exchange every tile counts: a blank has the same value as
        // the marker for a square played through.
        if (exchange || ml != PLAYED_THROUGH_MARKER) {
          rack_add_letter(&used,
                          get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
        }
      }
      rack_subtract_using_floor_zero(&truth, &used);
      if (!exchange) {
        rack_copy(&played, &used);
      }
    }
    if (carry_mode) {
      if (move_get_type(&move) != GAME_EVENT_PASS && !rack_is_empty(&truth) &&
          bag_tiles > 0) {
        rack_copy(&observer, player_get_rack(game_get_player(game, 1 - seat)));
        const double carry_temperature =
            carry_temp_text ? atof(carry_temp_text)
                            : (exchange ? 3.0
                                        : decide_schedule_temperature(bag_tiles));
        InferenceCarryoverSlot *table_in =
            carry_table[seat][carry_current[seat]];
        InferenceCarryoverSlot *table_out =
            carry_table[seat][1 - carry_current[seat]];
        memset(table_out, 0,
               (size_t)num_klv_leaves * sizeof(InferenceCarryoverSlot));
        double probability[2];
        for (int with_carry = 0; with_carry < 2; with_carry++) {
          InferenceArgs args;
          infer_args_fill(&args, 0, int_to_equity(10000), NULL, game, threads,
                          0, 0, control, false, true, seat,
                          move_get_score(&move),
                          exchange ? move_get_tiles_played(&move) : 0, &played,
                          &known, &observer);
          args.temperature = double_to_equity(carry_temperature);
          args.target_move = &move;
          if (with_carry) {
            if (carry_history[seat] > 0) {
              args.carry_in = table_in;
              args.carry_in_kept = carry_kept[seat];
              args.carry_in_floor = carry_floor[seat];
              if (carry_extra) {
                // What the mover can hold: the bag and their rack.
                const KLV *seat_klv =
                    player_get_klv(game_get_player(game, seat));
                InferenceCarryoverTable *sparse =
                    inference_carryover_table_create_from_slots(
                        table_in, seat_klv, carry_kept[seat]);
                Rack pool;
                rack_copy(&pool, player_get_rack(game_get_player(game, seat)));
                for (int ml = 0; ml < rack_get_dist_size(&pool); ml++) {
                  rack_add_letters(&pool, ml,
                                   bag_get_letter(game_get_bag(game), ml));
                }
                carry_table_max[seat] =
                    sparse ? (double)inference_carryover_table_get_max(sparse)
                           : 0;
                carry_pool_max[seat] =
                    sparse ? (double)inference_carryover_table_get_max_within(
                                 sparse, &pool)
                           : 0;
                inference_carryover_table_destroy(sparse);
                if (carry_pool_floor) {
                  args.carry_in_floor =
                      carry_floor_fraction * carry_pool_max[seat];
                }
              }
            }
            args.carry_out = table_out;
          }
          thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
          infer_without_ctx(&args, inferences, errors);
          if (!error_stack_is_empty(errors)) {
            error_stack_print_and_reset(errors);
            log_fatal("inftfit: inference failed game=%d turn=%d", game_idx,
                      turn);
          }
          probability[with_carry] = tfit_true_probability(inferences, &truth);
        }
        printf("TCARRY game=%d turn=%d bag=%d tiles=%d exch=%d leave=%d "
               "hist=%d kept=%d p0=%.6g p1=%.6g",
               game_idx, turn, bag_tiles, move_get_tiles_played(&move),
               exchange, rack_get_total_letters(&truth), carry_history[seat],
               carry_kept[seat], probability[0], probability[1]);
        float table_max = 0;
        for (uint32_t leave_idx = 0; leave_idx < num_klv_leaves; leave_idx++) {
          const uint32_t bits = atomic_load(&table_out[leave_idx]);
          float value;
          memcpy(&value, &bits, sizeof(value));
          if (value > table_max) {
            table_max = value;
          }
        }
        uint32_t entries_kept = 0;
        uint32_t entries_dropped = 0;
        for (uint32_t leave_idx = 0; carry_extra && leave_idx < num_klv_leaves;
             leave_idx++) {
          const uint32_t bits = atomic_load(&table_out[leave_idx]);
          float value;
          memcpy(&value, &bits, sizeof(value));
          if (value <= 0) {
            continue;
          }
          if ((double)value < carry_prune * (double)table_max) {
            atomic_store(&table_out[leave_idx], 0);
            entries_dropped++;
          } else {
            entries_kept++;
          }
        }
        if (carry_extra) {
          printf(" tmax=%.6g pmax=%.6g entries=%u dropped=%u",
                 carry_history[seat] > 0 ? carry_table_max[seat] : 0.0,
                 carry_history[seat] > 0 ? carry_pool_max[seat] : 0.0,
                 entries_kept, entries_dropped);
        }
        printf("\n");
        fflush(stdout);
        if (table_max > 0 && rack_get_total_letters(&truth) < (RACK_SIZE)) {
          carry_current[seat] = 1 - carry_current[seat];
          carry_kept[seat] = rack_get_total_letters(&truth);
          carry_floor[seat] = carry_floor_fraction * (double)table_max;
          carry_history[seat]++;
        } else {
          carry_history[seat] = 0;
        }
      } else if (move_get_type(&move) != GAME_EVENT_PASS) {
        // A bingo or an empty bag: nothing kept to carry.
        carry_history[seat] = 0;
      }
    } else if (move_get_type(&move) != GAME_EVENT_PASS &&
               !rack_is_empty(&truth) && bag_tiles > 0) {
      rack_copy(&observer, player_get_rack(game_get_player(game, 1 - seat)));
      printf("TFIT game=%d turn=%d bag=%d tiles=%d exch=%d leave=%d rank=%d "
             "loss=%.3f p=",
             game_idx, turn, bag_tiles, move_get_tiles_played(&move), exchange,
             rack_get_total_letters(&truth), rank, loss);
      for (int idx = 0; idx < GRID; idx++) {
        InferenceArgs args;
        infer_args_fill(
            &args, 0,
            grid[idx] < 0 ? double_to_equity(-grid[idx]) : int_to_equity(10000),
            NULL, game, threads, 0, 0, control, false, true, seat,
            move_get_score(&move), exchange ? move_get_tiles_played(&move) : 0,
            &played, &known, &observer);
        args.temperature = grid[idx] < 0 ? 0 : double_to_equity(grid[idx]);
        args.target_move = &move;
        thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
        infer_without_ctx(&args, inferences, errors);
        if (!error_stack_is_empty(errors)) {
          error_stack_print_and_reset(errors);
          log_fatal("inftfit: inference failed game=%d turn=%d", game_idx,
                    turn);
        }
        printf("%s%.6g", idx ? "," : "",
               tfit_true_probability(inferences, &truth));
      }
      printf("\n");
      fflush(stdout);
    }
    play_move(&move, game, NULL);
  }
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    for (int table_idx = 0; table_idx < 2; table_idx++) {
      free(carry_table[player_idx][table_idx]);
    }
  }
  inference_results_destroy(inferences);
  move_list_destroy(moves);
  error_stack_destroy(errors);
  config_destroy(config);
}

// True when the move places exactly the tiles in used.
static bool sfit_same_tiles(const Move *move, const Rack *used) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
      move_get_tiles_played(move) != rack_get_total_letters(used)) {
    return false;
  }
  Rack tiles;
  rack_set_dist_size_and_reset(&tiles, rack_get_dist_size(used));
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&tiles, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }
  return racks_are_equal(&tiles, used);
}

// One earlier turn of a player: the position before the move, the tiles it
// played, and what the static evaluator made of the move actually chosen.
typedef struct SfitTurn {
  Game *game;
  Rack used;
  double true_equity;
  double true_klv;
  int kept;
} SfitTurn;

// A player's turns since they last drew a whole rack, oldest first.
typedef struct SfitMemory {
  SfitTurn turns[SFIT_MAX_HISTORY];
  int count;
  int seat;
  const KLV *klv;
  MoveList *moves;
  // Lifts already computed for this position, keyed by level, depth and rack.
  uint64_t *cache_keys;
  double *cache_values;
  uint64_t cache_used;
} SfitMemory;

static uint64_t sfit_cache_key(int level, int depth, const MachineLetter *tiles,
                               int num_tiles) {
  uint64_t key = 1;
  key = (key << 3) | (uint64_t)level;
  key = (key << 3) | (uint64_t)depth;
  key = (key << 4) | (uint64_t)num_tiles;
  for (int tile_idx = 0; tile_idx < num_tiles; tile_idx++) {
    key = (key << 5) | (uint64_t)tiles[tile_idx];
  }
  return key;
}

static uint64_t sfit_cache_slot(uint64_t key) {
  return ((key * UINT64_C(0x9E3779B97F4A7C15)) >> (64 - SFIT_CACHE_BITS));
}

static const double sfit_memory_grid[SFIT_MEMORY_GRID] = {2, 3, 4, 6, 8};

// Carry-over lift for a rack held just after turns[level]: the static
// likelihood of that turn's play, averaged over which of the rack's tiles
// were the leave kept then. With depth > 1 each term is multiplied by the
// same lift one turn further back, for the rack that leave implies.
static void sfit_memory_lift(SfitMemory *memory, int level,
                             const MachineLetter *tiles, int num_tiles,
                             int depth, double *lift) {
  SfitTurn *turn = &memory->turns[level];
  const int dist_size = rack_get_dist_size(&turn->used);
  const uint64_t cache_key = sfit_cache_key(level, depth, tiles, num_tiles);
  uint64_t slot = sfit_cache_slot(cache_key);
  const uint64_t slot_mask = (UINT64_C(1) << SFIT_CACHE_BITS) - 1;
  while (memory->cache_keys[slot] != 0 &&
         memory->cache_keys[slot] != cache_key) {
    slot = (slot + 1) & slot_mask;
  }
  if (memory->cache_keys[slot] == cache_key) {
    for (int grid_idx = 0; grid_idx < SFIT_MEMORY_GRID; grid_idx++) {
      lift[grid_idx] =
          memory->cache_values[(slot * SFIT_MEMORY_GRID) + grid_idx];
    }
    return;
  }
  double sum[SFIT_MEMORY_GRID] = {0};
  int subsets = 0;
  Rack kept;
  Rack earlier;
  for (int mask = 0; mask < (1 << num_tiles); mask++) {
    int bits = 0;
    for (int bit = 0; bit < num_tiles; bit++) {
      bits += (mask >> bit) & 1;
    }
    if (bits != turn->kept) {
      continue;
    }
    rack_set_dist_size_and_reset(&kept, dist_size);
    for (int bit = 0; bit < num_tiles; bit++) {
      if ((mask >> bit) & 1) {
        rack_add_letter(&kept, tiles[bit]);
      }
    }
    rack_copy(&earlier, &kept);
    MachineLetter earlier_tiles[RACK_SIZE * 2];
    int num_earlier = 0;
    for (int ml = 0; ml < dist_size; ml++) {
      for (int copy = 0; copy < rack_get_letter(&turn->used, ml); copy++) {
        rack_add_letter(&earlier, (MachineLetter)ml);
      }
      for (int copy = 0; copy < rack_get_letter(&earlier, ml); copy++) {
        earlier_tiles[num_earlier++] = (MachineLetter)ml;
      }
    }
    rack_copy(player_get_rack(game_get_player(turn->game, memory->seat)),
              &earlier);
    const double best = equity_to_double(
        move_get_equity(get_top_equity_move(turn->game, memory->moves)));
    const double observed =
        turn->true_equity - turn->true_klv +
        equity_to_double(klv_get_leave_value(memory->klv, &kept));
    const double loss = fmax(0.0, best - observed);
    double deeper[SFIT_MEMORY_GRID];
    for (int grid_idx = 0; grid_idx < SFIT_MEMORY_GRID; grid_idx++) {
      deeper[grid_idx] = 1.0;
    }
    if (depth > 1 && level > 0 && num_earlier <= RACK_SIZE) {
      sfit_memory_lift(memory, level - 1, earlier_tiles, num_earlier, depth - 1,
                       deeper);
    }
    for (int grid_idx = 0; grid_idx < SFIT_MEMORY_GRID; grid_idx++) {
      sum[grid_idx] +=
          exp(-loss / sfit_memory_grid[grid_idx]) * deeper[grid_idx];
    }
    subsets++;
  }
  for (int grid_idx = 0; grid_idx < SFIT_MEMORY_GRID; grid_idx++) {
    lift[grid_idx] = subsets > 0 ? sum[grid_idx] / subsets : 1.0;
  }
  // Deeper calls may have filled this slot's neighbourhood: find it again.
  slot = sfit_cache_slot(cache_key);
  while (memory->cache_keys[slot] != 0) {
    slot = (slot + 1) & slot_mask;
  }
  if (memory->cache_used < (UINT64_C(1) << (SFIT_CACHE_BITS - 1))) {
    memory->cache_keys[slot] = cache_key;
    memory->cache_used++;
    for (int grid_idx = 0; grid_idx < SFIT_MEMORY_GRID; grid_idx++) {
      memory->cache_values[(slot * SFIT_MEMORY_GRID) + grid_idx] =
          lift[grid_idx];
    }
  }
}

// infsfit: replays logged control games like inftfit. Before each placement
// it takes the mover's true rack and INFSFIT_LEAVES racks drawn from the
// observer's prior (the played tiles plus a random leave), and for each one
// sims the top static plays plus the observed play from the mover's side.
// Prints static equities, sim win rates and sim equities so that likelihood
// models can be fitted offline. k=0 is the true rack.
void infer_decide_sfit_stdin(void) {
  const char *seed_text = getenv("INFTFIT_SEED");
  assert(seed_text != NULL);
  const uint64_t seed = strtoull(seed_text, NULL, 10);
  const int threads = sfit_env_int("INFSFIT_THREADS", 2);
  const int num_leaves = sfit_env_int("INFSFIT_LEAVES", 150);
  const int rollouts = sfit_env_int("INFSFIT_ROLLOUTS", 150);
  const int plies = sfit_env_int("INFSFIT_PLIES", 2);
  const int every = sfit_env_int("INFSFIT_EVERY", 1);
  // Extra sim slots for the best other placements of the observed tiles.
  const int same_quota = sfit_env_int("INFSFIT_SAME", 0);
  assert(same_quota <= SFIT_MAX_SAME);
  const bool no_sim = sfit_env_int("INFSFIT_NOSIM", 0) != 0;
  char *settings = get_formatted_string(
      "set -lex CSW24 -leaves CSW24 -wmp true -rit true -ritmmap true -wit "
      "true -s1 equity -s2 equity -r1 all -r2 all -pat auto -threads %d "
      "-numplays 100 -minp 500 -savesettings false -autosavegcg false",
      threads);
  Config *config = config_create_or_die(settings);
  free(settings);
  char *plies_cmd = get_formatted_string("set -plies %d", plies);
  load_and_exec_config_or_die(config, plies_cmd);
  free(plies_cmd);
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 -lex CSW24;");
  ErrorStack *errors = error_stack_create();
  config_load_win_pcts(config, errors);
  assert(error_stack_is_empty(errors) && config_get_win_pcts(config) != NULL);
  Game *game = config_get_game(config);
  Game *work = game_duplicate(game);
  Game *follow = game_duplicate(game);
  // Carry-over from each player's earlier turns.
  const int memory_depth = sfit_env_int("INFSFIT_MEMDEPTH", 2);
  SfitMemory memory[2];
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    memory[player_idx].count = 0;
    memory[player_idx].seat = player_idx;
    memory[player_idx].klv = NULL;
    memory[player_idx].moves = NULL;
    memory[player_idx].cache_keys =
        calloc_or_die((size_t)1 << SFIT_CACHE_BITS, sizeof(uint64_t));
    memory[player_idx].cache_values = calloc_or_die(
        ((size_t)1 << SFIT_CACHE_BITS) * SFIT_MEMORY_GRID, sizeof(double));
    memory[player_idx].cache_used = 0;
    for (int turn_idx = 0; turn_idx < SFIT_MAX_HISTORY; turn_idx++) {
      memory[player_idx].turns[turn_idx].game = game_duplicate(game);
    }
  }
  char memory_leave_str[2][DECIDE_MOVE_STR];
  MoveList *follow_moves = move_list_create(1);
  const LetterDistribution *ld = game_get_ld(game);
  ThreadControl *control = config_get_thread_control(config);
  printf("SFIT_SETTINGS seed=%llu leaves=%d rollouts=%d plies=%d every=%d "
         "top=%d threads=%d same=%d\n",
         (unsigned long long)seed, num_leaves, rollouts, plies, every,
         SFIT_TOP_PLAYS, threads, same_quota);
  MoveList *moves = move_list_create(SFIT_MOVE_CAPACITY);
  MoveList *candidates = move_list_create(SFIT_MAX_PLAYS);
  SimResults *results = sim_results_create(0);
  Rack no_rack, sim_played, known, observer, truth, used, leave;
  rack_set_dist_size_and_reset(&no_rack, ld_get_size(ld));
  rack_set_dist_size_and_reset(&sim_played, ld_get_size(ld));
  rack_set_dist_size_and_reset(&known, ld_get_size(ld));
  rack_set_dist_size_and_reset(&observer, ld_get_size(ld));
  rack_set_dist_size_and_reset(&used, ld_get_size(ld));
  char line[DECIDE_RANK_LINE];
  char move_str[DECIDE_MOVE_STR];
  char rack_str[DECIDE_MOVE_STR];
  char candidate_str[SFIT_MAX_PLAYS][DECIDE_MOVE_STR];
  int bad_game = -1;
  while (fgets(line, sizeof(line), stdin)) {
    int game_idx;
    int turn;
    char want_rack[64];
    char want_move[DECIDE_MOVE_STR];
    if (sscanf(line, "%d %d %63s %95s", &game_idx, &turn, want_rack,
               want_move) != 4) {
      log_fatal("infsfit: bad line: %s", line);
    }
    if (game_idx == bad_game) {
      continue;
    }
    if (turn == 0) {
      game_reset(game);
      game_seed(game, seed + (uint64_t)game_idx);
      game_set_starting_player_index(game, game_idx % 2);
      draw_starting_racks(game);
      memory[0].count = 0;
      memory[1].count = 0;
    }
    const int seat = game_get_player_on_turn_index(game);
    const Rack *mover = player_get_rack(game_get_player(game, seat));
    decide_rack_string(game, mover, rack_str, sizeof(rack_str));
    if (strcmp(rack_str, want_rack) != 0) {
      printf("SFIT_BAD game=%d turn=%d rack=%s want=%s\n", game_idx, turn,
             rack_str, want_rack);
      bad_game = game_idx;
      continue;
    }
    const MoveGenArgs gen = {.game = game,
                             .move_list = moves,
                             .move_record_type = MOVE_RECORD_ALL,
                             .move_sort_type = MOVE_SORT_EQUITY,
                             .target_equity = EQUITY_MAX_VALUE};
    generate_moves(&gen);
    move_list_sort_moves(moves);
    const Move *found = NULL;
    for (int idx = 0; idx < move_list_get_count(moves) && !found; idx++) {
      decide_move_string(game, move_list_get_move(moves, idx), move_str);
      if (strcmp(move_str, want_move) == 0) {
        found = move_list_get_move(moves, idx);
      }
    }
    if (!found) {
      printf("SFIT_BAD game=%d turn=%d move=%s\n", game_idx, turn, want_move);
      bad_game = game_idx;
      continue;
    }
    Move move;
    move_copy(&move, found);
    const int bag_tiles = bag_get_letters(game_get_bag(game));
    rack_reset(&used);
    rack_copy(&truth, mover);
    const bool placement =
        move_get_type(&move) == GAME_EVENT_TILE_PLACEMENT_MOVE;
    if (placement) {
      for (int idx = 0; idx < move_get_tiles_length(&move); idx++) {
        const MachineLetter ml = move_get_tile(&move, idx);
        if (ml != PLAYED_THROUGH_MARKER) {
          rack_add_letter(&used,
                          get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
        }
      }
      rack_subtract_using_floor_zero(&truth, &used);
    }
    if (placement) {
      // The best play that uses exactly the observed play's tiles. Its leave
      // is the same for every rack, so any gap is not about the leave.
      Equity same_equity = move_get_equity(&move);
      int same_score = equity_to_int(move_get_score(&move));
      int same_ties = 0;
      int same_within2 = 0;
      int same_within5 = 0;
      int same_others = 0;
      Rack other;
      for (int idx = 0; idx < move_list_get_count(moves); idx++) {
        const Move *candidate = move_list_get_move(moves, idx);
        if (move_get_type(candidate) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
            move_get_tiles_played(candidate) != move_get_tiles_played(&move)) {
          continue;
        }
        rack_set_dist_size_and_reset(&other, ld_get_size(ld));
        for (int tile_idx = 0; tile_idx < move_get_tiles_length(candidate);
             tile_idx++) {
          const MachineLetter ml = move_get_tile(candidate, tile_idx);
          if (ml != PLAYED_THROUGH_MARKER) {
            rack_add_letter(&other,
                            get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
          }
        }
        if (!racks_are_equal(&other, &used)) {
          continue;
        }
        if (candidate != found) {
          const Equity below =
              move_get_equity(&move) - move_get_equity(candidate);
          same_ties += below == 0;
          same_within2 += below > 0 && below <= int_to_equity(2);
          same_within5 += below > int_to_equity(2) && below <= int_to_equity(5);
          same_others++;
        }
        if (move_get_equity(candidate) > same_equity) {
          same_equity = move_get_equity(candidate);
        }
        if (equity_to_int(move_get_score(candidate)) > same_score) {
          same_score = equity_to_int(move_get_score(candidate));
        }
      }
      printf("SGAP game=%d turn=%d bag=%d ties=%d within2=%d within5=%d "
             "others=%d gap=%.2f scoregap=%d\n",
             game_idx, turn, bag_tiles, same_ties, same_within2, same_within5,
             same_others,
             equity_to_double(same_equity) -
                 equity_to_double(move_get_equity(&move)),
             same_score - equity_to_int(move_get_score(&move)));
    }
    if (num_leaves >= 0 && placement && !rack_is_empty(&truth) &&
        bag_tiles > 0 &&
        ((game_idx * 31) + turn) % every == sfit_env_int("INFSFIT_OFFSET", 0)) {
      game_copy(work, game);
      game_seed(work, (seed * UINT64_C(1000003)) +
                          ((uint64_t)game_idx * UINT64_C(7919)) +
                          (uint64_t)turn);
      memory[seat].klv = player_get_klv(game_get_player(game, seat));
      memory[seat].moves = follow_moves;
      memset(memory[seat].cache_keys, 0,
             ((size_t)1 << SFIT_CACHE_BITS) * sizeof(uint64_t));
      memory[seat].cache_used = 0;
      for (int leave_idx = 0; leave_idx <= num_leaves; leave_idx++) {
        if (leave_idx > 0) {
          set_random_rack(work, seat, &used);
        }
        rack_copy(&leave, player_get_rack(game_get_player(work, seat)));
        rack_subtract_using_floor_zero(&leave, &used);
        decide_rack_string(work, &leave, rack_str, sizeof(rack_str));
        const MoveGenArgs work_gen = {.game = work,
                                      .move_list = moves,
                                      .move_record_type = MOVE_RECORD_ALL,
                                      .move_sort_type = MOVE_SORT_EQUITY,
                                      .target_equity = EQUITY_MAX_VALUE};
        generate_moves(&work_gen);
        move_list_sort_moves(moves);
        const int count = move_list_get_count(moves);
        int observed_rank = -1;
        for (int idx = 0; idx < count && observed_rank < 0; idx++) {
          decide_move_string(work, move_list_get_move(moves, idx), move_str);
          if (strcmp(move_str, want_move) == 0) {
            observed_rank = idx;
          }
        }
        printf("SFIT game=%d turn=%d bag=%d tiles=%d k=%d leave=%s rank=%d "
               "count=%d",
               game_idx, turn, bag_tiles, move_get_tiles_played(&move),
               leave_idx, rack_str, observed_rank, count);
        if (observed_rank < 0) {
          // Not among the best SFIT_MOVE_CAPACITY plays: report how far
          // below the best play it must be.
          printf(
              " floor=%.3f\n",
              equity_to_double(move_get_equity(move_list_get_move(moves, 0))) -
                  equity_to_double(
                      move_get_equity(move_list_get_move(moves, count - 1))));
          continue;
        }
        // Follow-up features: the best play the kept tiles alone can make
        // before the observed play and after it (same player to move), to
        // show plays that set up, or block, the leave.
        game_copy(follow, work);
        rack_copy(player_get_rack(game_get_player(follow, seat)), &leave);
        const Move *before_move = get_top_equity_move(follow, follow_moves);
        const int before_score = equity_to_int(move_get_score(before_move));
        const double before_equity =
            equity_to_double(move_get_equity(before_move));
        game_copy(follow, work);
        play_move_without_drawing_tiles(
            move_list_get_move(moves, observed_rank), follow);
        game_set_player_on_turn_index(follow, seat);
        const Move *after_move = get_top_equity_move(follow, follow_moves);
        const int after_score = equity_to_int(move_get_score(after_move));
        const double after_equity =
            equity_to_double(move_get_equity(after_move));
        printf(" fb=%d,%.2f fa=%d,%.2f", before_score, before_equity,
               after_score, after_equity);
        // Carry-over lifts for this rack, looking back 1..memory_depth turns.
        if (memory[seat].count > 0) {
          const Rack *full_rack = player_get_rack(game_get_player(work, seat));
          MachineLetter tiles[RACK_SIZE];
          int num_tiles = 0;
          for (int ml = 0; ml < ld_get_size(ld); ml++) {
            for (int copy = 0; copy < rack_get_letter(full_rack, ml); copy++) {
              tiles[num_tiles++] = (MachineLetter)ml;
            }
          }
          printf(" pl=%s hist=%d kp=", memory_leave_str[seat],
                 memory[seat].count);
          for (int back = 0; back < memory[seat].count; back++) {
            printf("%s%d", back ? "," : "",
                   memory[seat].turns[memory[seat].count - 1 - back].kept);
          }
          for (int depth = 1;
               depth <= memory_depth && depth <= memory[seat].count; depth++) {
            double lift[SFIT_MEMORY_GRID];
            sfit_memory_lift(&memory[seat], memory[seat].count - 1, tiles,
                             num_tiles, depth, lift);
            printf(" m%d=", depth);
            for (int grid_idx = 0; grid_idx < SFIT_MEMORY_GRID; grid_idx++) {
              printf("%s%.5g", grid_idx ? "," : "", lift[grid_idx]);
            }
          }
        }
        double static_equity[SFIT_MAX_PLAYS];
        int num_candidates = 0;
        int observed_candidate = -1;
        move_list_reset(candidates);
        for (int idx = 0; idx < count && idx < SFIT_TOP_PLAYS; idx++) {
          const Move *candidate = move_list_get_move(moves, idx);
          if (idx == observed_rank) {
            observed_candidate = num_candidates;
          }
          static_equity[num_candidates] =
              equity_to_double(move_get_equity(candidate));
          decide_move_string(work, candidate, candidate_str[num_candidates]);
          move_list_add_move(candidates, candidate);
          num_candidates++;
        }
        if (observed_candidate < 0) {
          const Move *candidate = move_list_get_move(moves, observed_rank);
          observed_candidate = num_candidates;
          static_equity[num_candidates] =
              equity_to_double(move_get_equity(candidate));
          decide_move_string(work, candidate, candidate_str[num_candidates]);
          move_list_add_move(candidates, candidate);
          num_candidates++;
        }
        int num_same = 0;
        for (int idx = SFIT_TOP_PLAYS; idx < count && num_same < same_quota;
             idx++) {
          const Move *candidate = move_list_get_move(moves, idx);
          if (idx == observed_rank || !sfit_same_tiles(candidate, &used)) {
            continue;
          }
          static_equity[num_candidates] =
              equity_to_double(move_get_equity(candidate));
          decide_move_string(work, candidate, candidate_str[num_candidates]);
          move_list_add_move(candidates, candidate);
          num_candidates++;
          num_same++;
        }
        uint64_t sim_iterations = 0;
        double sim_win[SFIT_MAX_PLAYS];
        double sim_equity[SFIT_MAX_PLAYS];
        for (int idx = 0; idx < num_candidates; idx++) {
          sim_win[idx] = -1;
          sim_equity[idx] = 0;
        }
        if (num_candidates > 1 && !no_sim) {
          SimArgs args;
          rack_reset(&sim_played);
          rack_copy(&observer,
                    player_get_rack(game_get_player(work, 1 - seat)));
          config_fill_sim_args(config, &no_rack, &sim_played, &observer, &known,
                               &args);
          args.move_list = candidates;
          args.num_plays = num_candidates;
          args.game = work;
          args.use_inference = false;
          args.seed = (seed * UINT64_C(31)) +
                      ((uint64_t)game_idx * UINT64_C(1000003)) +
                      ((uint64_t)turn * UINT64_C(7919)) + (uint64_t)leave_idx;
          args.bai_options.threshold = BAI_THRESHOLD_NONE;
          args.bai_options.sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN;
          args.bai_options.sample_limit =
              (uint64_t)rollouts * (uint64_t)num_candidates;
          // The config's -minp would otherwise force 500 rollouts per play.
          args.bai_options.sample_minimum = (uint64_t)rollouts;
          args.bai_options.time_limit_seconds = 120;
          thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
          SimCtx *sim_ctx = NULL;
          simulate(&args, &sim_ctx, results, errors);
          if (!error_stack_is_empty(errors)) {
            error_stack_print_and_reset(errors);
            log_fatal("infsfit sim failed: game=%d turn=%d", game_idx, turn);
          }
          const int sim_plays = sim_results_get_number_of_plays(results);
          for (int play_idx = 0; play_idx < sim_plays; play_idx++) {
            const SimmedPlay *simmed =
                sim_results_get_simmed_play(results, play_idx);
            decide_move_string(work, simmed_play_get_move(simmed), move_str);
            for (int idx = 0; idx < num_candidates; idx++) {
              if (strcmp(candidate_str[idx], move_str) == 0) {
                sim_win[idx] =
                    stat_get_mean(simmed_play_get_win_pct_stat(simmed));
                sim_equity[idx] =
                    stat_get_mean(simmed_play_get_equity_stat(simmed));
              }
            }
          }
          sim_iterations = sim_results_get_iteration_count(results);
          sim_ctx_destroy(sim_ctx);
        }
        printf(" obs=%d n=%d ns=%d it=%llu se=", observed_candidate,
               num_candidates, num_same, (unsigned long long)sim_iterations);
        for (int idx = 0; idx < num_candidates; idx++) {
          printf("%s%.2f", idx ? "," : "", static_equity[idx]);
        }
        printf(" sw=");
        for (int idx = 0; idx < num_candidates; idx++) {
          printf("%s%.4f", idx ? "," : "", sim_win[idx]);
        }
        printf(" sq=");
        for (int idx = 0; idx < num_candidates; idx++) {
          printf("%s%.2f", idx ? "," : "", sim_equity[idx]);
        }
        printf("\n");
      }
      fflush(stdout);
    }
    if (placement && !rack_is_empty(&truth)) {
      SfitMemory *mine = &memory[seat];
      if (mine->count == SFIT_MAX_HISTORY) {
        Game *oldest = mine->turns[0].game;
        for (int turn_idx = 1; turn_idx < SFIT_MAX_HISTORY; turn_idx++) {
          mine->turns[turn_idx - 1] = mine->turns[turn_idx];
        }
        mine->turns[SFIT_MAX_HISTORY - 1].game = oldest;
        mine->count--;
      }
      SfitTurn *entry = &mine->turns[mine->count++];
      game_copy(entry->game, game);
      rack_copy(&entry->used, &used);
      entry->true_equity = equity_to_double(move_get_equity(&move));
      entry->true_klv = equity_to_double(klv_get_leave_value(
          player_get_klv(game_get_player(game, seat)), &truth));
      entry->kept = rack_get_total_letters(&truth);
      decide_rack_string(game, &truth, memory_leave_str[seat],
                         sizeof(memory_leave_str[seat]));
    } else {
      // A bingo, exchange or pass: start the chain again.
      memory[seat].count = 0;
    }
    play_move(&move, game, NULL);
  }
  sim_results_destroy(results);
  move_list_destroy(candidates);
  move_list_destroy(moves);
  game_destroy(work);
  game_destroy(follow);
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    for (int turn_idx = 0; turn_idx < SFIT_MAX_HISTORY; turn_idx++) {
      game_destroy(memory[player_idx].turns[turn_idx].game);
    }
    free(memory[player_idx].cache_keys);
    free(memory[player_idx].cache_values);
  }
  move_list_destroy(follow_moves);
  error_stack_destroy(errors);
  config_destroy(config);
}

static void steer_dump_leaves(Config *config, const char *tag) {
  load_and_exec_config_or_die(config, "infer");
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const AliasMethod *am = inference_results_get_alias_method(
      config_get_inference_results(config));
  Rack rack;
  rack_set_dist_size_and_reset(&rack, ld_get_size(ld));
  for (uint32_t idx = 0; idx < am->num_items; idx++) {
    const AliasMethodItem *item = &am->items[idx];
    for (int part = 0; part < 2; part++) {
      rack_decode(part == 0 ? &item->rack : &am->items[item->alias].rack,
                  &rack);
      const double share =
          (part == 0 ? item->probability : 1 - item->probability) /
          am->num_items;
      if (share <= 0) {
        continue;
      }
      StringBuilder *sb = string_builder_create();
      string_builder_add_rack(sb, &rack, ld, false);
      printf("LEAVE %s %s %.10g\n", tag, string_builder_peek(sb), share);
      string_builder_destroy(sb);
    }
  }
}

// Research (magpie_test infsteer): fits the sim-steered loss model for the
// move before one position of a game and prints the inferred leaves without
// it and with it. INFSTEER_GCG and INFSTEER_TURN (events played, the move
// inferred is the last of them) name the position; INFSTEER_RACKS and
// INFSTEER_ROLLOUTS set the fit's budget; INFSTEER_CARRY=false turns
// carry-over off. The temperature follows MAGPIE_INF_SCHEDULE.
void infer_steer_position(void) {
  const char *gcg = getenv("INFSTEER_GCG");
  const char *turn_text = getenv("INFSTEER_TURN");
  assert(gcg != NULL && turn_text != NULL);
  const int turn = (int)strtol(turn_text, NULL, 10);
  const int num_racks = sfit_env_int("INFSTEER_RACKS", 500);
  const int rollouts = sfit_env_int("INFSTEER_ROLLOUTS", 200);
  const int threads = sfit_env_int("INFSTEER_THREADS", 8);
  const char *carry = getenv("INFSTEER_CARRY");
  char *settings = get_formatted_string(
      "set -lex %s -wmp true -rit false -wit false -s1 equity -s2 equity "
      "-r1 all -r2 all -pat auto -threads %d -numplays 15 -inftemp 3 -ima "
      "10000 -infcarryover %s -savesettings false -autosavegcg false",
      getenv("INFSTEER_LEX") != NULL ? getenv("INFSTEER_LEX") : "CSW24",
      threads, carry != NULL ? carry : "true");
  Config *config = config_create_or_die(settings);
  free(settings);
  char *command = get_formatted_string("load %s", gcg);
  load_and_exec_config_or_die(config, command);
  free(command);
  command = get_formatted_string("goto %d", turn - 1);
  load_and_exec_config_or_die(config, command);
  free(command);
  Game *base = game_duplicate(config_get_game(config));
  const GameHistory *history = config_get_game_history(config);
  const GameEvent *event = game_history_get_event(history, turn - 1);
  const GameEvent *next = game_history_get_event(history, turn);
  const int mover = game_event_get_player_index(event);
  const int ld_size = ld_get_size(game_get_ld(base));
  const Move *move = validated_moves_get_move(game_event_get_vms(event), 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(game_event_get_player_index(next) == 1 - mover);
  // The replay leaves the observer's tiles in the bag; their rack is the one
  // their next move records.
  Rack *observer = player_get_rack(game_get_player(base, 1 - mover));
  if (rack_is_empty(observer)) {
    draw_leave_from_bag(game_get_bag(base), 1 - mover, observer,
                        game_event_get_const_rack(next));
  }
  Rack played;
  rack_set_dist_size_and_reset(&played, ld_size);
  for (int idx = 0; idx < move_get_tiles_length(move); idx++) {
    const MachineLetter ml = move_get_tile(move, idx);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&played, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }
  static InferenceLossModel model;
  double mean_static = 0;
  double mean_sim = 0;
  Timer timer;
  ctimer_start(&timer);
  const int fitted =
      steer_fit(base, mover, move, &played, threads, num_racks, rollouts,
                (uint64_t)sfit_env_int("INFSTEER_SEED", 1), &model,
                &mean_static, &mean_sim);
  printf("FIT racks=%d rollouts=%d fitted=%d seconds=%.2f mean_static=%.3f "
         "mean_sim=%.3f intercept=%.3f slope=%.3f\n",
         num_racks, rollouts, fitted, ctimer_elapsed_seconds(&timer),
         mean_static, mean_sim, fitted > 0 ? model.intercept : 0.0,
         fitted > 0 ? model.slope : 0.0);
  const LetterDistribution *ld = game_get_ld(base);
  for (int first = 0; fitted > 0 && first < ld_size; first++) {
    char *letter = ld_ml_to_hl(ld, (MachineLetter)first);
    printf("TILE %s %.3f\n", letter, model.letter[first]);
    for (int second = first; second < ld_size; second++) {
      if (model.pair[first][second] != 0) {
        char *other = ld_ml_to_hl(ld, (MachineLetter)second);
        printf("PAIR %s%s %.3f\n", letter, other, model.pair[first][second]);
        free(other);
      }
    }
    free(letter);
  }
  game_destroy(base);
  command = get_formatted_string("goto %d", turn);
  load_and_exec_config_or_die(config, command);
  free(command);
  steer_dump_leaves(config, "static");
  if (fitted > 0) {
    static const char *const scales[] = {"1.0", "0.7", "0.5"};
    inference_research_loss_model = &model;
    for (int idx = 0; idx < 3; idx++) {
      setenv("MAGPIE_INF_SCHEDULE_SCALE", scales[idx], 1);
      char *tag = get_formatted_string("steer%s", scales[idx]);
      steer_dump_leaves(config, tag);
      free(tag);
    }
    inference_research_loss_model = NULL;
    unsetenv("MAGPIE_INF_SCHEDULE_SCALE");
  }
  if (getenv("INFSTEER_SIM") != NULL) {
    // The position's sim with the static inference, then with the model.
    load_and_exec_config_or_die(
        config, "set -plies 4 -it 600000 -scondition none -sr rr -sinfer true "
                "-seed 99 -maxnumdplays 225");
    load_and_exec_config_or_die(config, "gen");
    static const char *const sim_scales[] = {"1.0", "1.0", "0.7"};
    for (int arm = 0; arm < (fitted > 0 ? 3 : 1); arm++) {
      inference_research_loss_model = arm == 0 ? NULL : &model;
      setenv("MAGPIE_INF_SCHEDULE_SCALE", sim_scales[arm], 1);
      load_and_exec_config_or_die(config, "sim");
      const SimResults *results = config_get_sim_results(config);
      const int sim_plays = sim_results_get_number_of_plays(results);
      for (int idx = 0; idx < sim_plays; idx++) {
        const SimmedPlay *simmed = sim_results_get_simmed_play(results, idx);
        char move_str[80];
        decide_move_string(config_get_game(config), simmed_play_get_move(simmed),
                           move_str);
        printf("SIM %s%s | %s | %.4f %.4f %.3f %llu\n",
               arm == 0 ? "static" : "steer", arm == 0 ? "" : sim_scales[arm],
               move_str,
               stat_get_mean(simmed_play_get_win_pct_stat(simmed)),
               stat_get_mean(simmed_play_get_utility_stat(simmed)),
               stat_get_mean(simmed_play_get_equity_stat(simmed)),
               (unsigned long long)simmed_play_get_num_rollouts(simmed));
      }
    }
    inference_research_loss_model = NULL;
    unsetenv("MAGPIE_INF_SCHEDULE_SCALE");
  }
  config_destroy(config);
}
