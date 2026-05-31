#include "peg.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../compat/ctime.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/transposition_table.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "move_gen.h"
#include "peg_pool.h"

// Stage table (see peg.h). Stage 0 is the greedy seed (top-K = all, greedy
// leaf); the halving stages narrow the surviving set while adding a ply of
// fidelity. The tail is top-2, never top-1 — a stage re-ranks a set, so its
// output needs >= 2 candidates to compare.
const int PEG_STAGE_TOP_K[PEG_NUM_STAGES] = {INT32_MAX, 32, 16, 8, 4, 2};
const int PEG_STAGE_NONEMPTY_INNER_D[PEG_NUM_STAGES] = {0, 0, 1, 2, 3, 4};
const int PEG_STAGE_EMPTIER_PLIES[PEG_NUM_STAGES] = {0, 2, 3, 4, 5, 6};

enum {
  // Candidate move-list capacity for the root generate_moves.
  PEG_CAND_LIST_CAP = 16384,
  // Greedy playout depth ceiling (a PEG playout terminates well before this).
  PEG_PLAYOUT_MAX_PLIES = 40,
  // Fixed endgame seed for deterministic leaf solves.
  PEG_ENDGAME_SEED = 1,
};

// Per-worker scratch: a greedy-playout move list plus a reusable endgame
// context/results pair. Indexed by the pool worker_idx (one extra slot for the
// main thread when it helps drain the queue).
typedef struct PegWorker {
  MoveList *playout_ml;
  EndgameCtx *eg_ctx;
  EndgameResults *eg_results;
  // Reused per-scenario game: game_copy into it instead of game_duplicate, so
  // we don't malloc/free a whole Game per leaf. Lazily allocated.
  Game *scratch_game;
  // Shared endgame TT, reused across every leaf solve this worker runs. Many
  // scenarios reach identical board states, so cross-scenario reuse is the
  // dominant endgame speedup.
  TranspositionTable *eg_tt;
  int thread_index_offset;
} PegWorker;

// ----- combinatorics -------------------------------------------------------

static int64_t peg_binomial(int n, int k) {
  if (k < 0 || k > n) {
    return 0;
  }
  if (k == 0 || k == n) {
    return 1;
  }
  if (k > n - k) {
    k = n - k;
  }
  int64_t result = 1;
  for (int i = 0; i < k; i++) {
    result = result * (n - i) / (i + 1);
  }
  return result;
}

// In-place lexicographic next-permutation; only enumerates distinct orderings
// (skips duplicates). Caller sorts ascending before the first call. Returns
// false at the last permutation.
static bool peg_next_perm(MachineLetter *arr, int n) {
  if (n <= 1) {
    return false;
  }
  int pivot = n - 2;
  while (pivot >= 0 && arr[pivot] >= arr[pivot + 1]) {
    pivot--;
  }
  if (pivot < 0) {
    return false;
  }
  int swap_idx = n - 1;
  while (arr[pivot] >= arr[swap_idx]) {
    swap_idx--;
  }
  MachineLetter tmp = arr[pivot];
  arr[pivot] = arr[swap_idx];
  arr[swap_idx] = tmp;
  int lo = pivot + 1;
  int hi = n - 1;
  while (lo < hi) {
    tmp = arr[lo];
    arr[lo] = arr[hi];
    arr[hi] = tmp;
    lo++;
    hi--;
  }
  return true;
}

// ----- position setup ------------------------------------------------------

// Tiles not visible to the mover: full distribution minus mover's rack minus
// the board. Returns the total count.
static int peg_compute_unseen(const Game *game, int mover_idx,
                              uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  memset(unseen, 0, sizeof(uint8_t) * MAX_ALPHABET_SIZE);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mover_rack, ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col)) {
        continue;
      }
      const MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
        if (unseen[BLANK_MACHINE_LETTER] > 0) {
          unseen[BLANK_MACHINE_LETTER]--;
        }
      } else if (unseen[ml] > 0) {
        unseen[ml]--;
      }
    }
  }
  int total = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total += unseen[ml];
  }
  return total;
}

// Set opp's rack to (unseen minus the bag tiles) — i.e. the tiles opp must
// be holding once the bag is fixed to `bag_tiles`.
static void peg_set_opp_rack(Rack *opp_rack,
                             const uint8_t unseen[MAX_ALPHABET_SIZE],
                             int ld_size, const MachineLetter *bag_tiles,
                             int n_bag) {
  uint8_t remaining[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < ld_size; ml++) {
    remaining[ml] = unseen[ml];
  }
  for (int i = 0; i < n_bag; i++) {
    if (remaining[bag_tiles[i]] > 0) {
      remaining[bag_tiles[i]]--;
    }
  }
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    for (int i = 0; i < remaining[ml]; i++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

// Build the post-cand game for one (mover_drawn, bag_remaining) split into the
// worker's reused scratch game: bag holds (mover_drawn ++ bag_remaining), opp
// rack is unseen minus the bag, cand is played, then the mover draws their
// k_drawn tiles. Returns the scratch game (owned by the worker; not destroyed
// per call).
static Game *peg_make_post_cand_game(PegWorker *worker, const Game *base_game,
                                     int mover_idx, const uint8_t *unseen,
                                     int ld_size, const Move *cand, int k_drawn,
                                     const MachineLetter *mover_drawn,
                                     int n_bag_remaining,
                                     const MachineLetter *bag_remaining) {
  if (worker->scratch_game == NULL) {
    worker->scratch_game = game_duplicate(base_game);
  } else {
    game_copy(worker->scratch_game, base_game);
  }
  Game *game = worker->scratch_game;
  game_set_endgame_solving_mode(game);
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  Bag *bag = game_get_bag(game);
  Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover_idx));
  Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));

  MachineLetter all_bag[PEG_MAX_BAG + 1];
  const int n_bag = k_drawn + n_bag_remaining;
  for (int i = 0; i < k_drawn; i++) {
    all_bag[i] = mover_drawn[i];
  }
  for (int i = 0; i < n_bag_remaining; i++) {
    all_bag[k_drawn + i] = bag_remaining[i];
  }
  bag_set_to_tiles(bag, all_bag, n_bag);
  peg_set_opp_rack(opp_rack, unseen, ld_size, all_bag, n_bag);
  play_move_without_drawing_tiles(cand, game);
  for (int i = 0; i < k_drawn; i++) {
    rack_add_letter(mover_rack, mover_drawn[i]);
    (void)bag_draw_letter(bag, mover_drawn[i], 0);
  }
  // play_move_without_drawing_tiles may flag GAME_END_REASON_STANDARD when the
  // rack empties post-placement (the no-draw endgame world). We re-stock the
  // rack right after, so clear the stale flag unless the rack is genuinely
  // empty — otherwise the greedy playout bails before simulating anything.
  if (!rack_is_empty(mover_rack)) {
    game_set_game_end_reason(game, GAME_END_REASON_NONE);
  }
  return game;
}

// Greedy playout to game end; returns signed mover spread (points), with the
// usual rack-leave adjustment when the game has not actually ended.
static int32_t peg_greedy_playout(Game *game, int mover_idx,
                                  MoveList *playout_ml) {
  const LetterDistribution *ld = game_get_ld(game);
  for (int ply = 0; ply < PEG_PLAYOUT_MAX_PLIES; ply++) {
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      break;
    }
    const bool bag_has_tiles = bag_get_letters(game_get_bag(game)) > 0;
    const MoveGenArgs args = {
        .game = game,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = bag_has_tiles ? MOVE_SORT_EQUITY : MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .thread_index = 0,
        .move_list = playout_ml,
        .tiles_played_bv = NULL,
        .initial_tiles_bv = 0,
    };
    generate_moves(&args);
    if (move_list_get_count(playout_ml) == 0) {
      break;
    }
    play_move(move_list_get_move(playout_ml, 0), game, NULL);
  }
  const Player *me = game_get_player(game, mover_idx);
  const Player *op = game_get_player(game, 1 - mover_idx);
  int32_t spread = equity_to_int(player_get_score(me) - player_get_score(op));
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    spread -= equity_to_int(rack_get_score(ld, player_get_rack(me)));
    spread += equity_to_int(rack_get_score(ld, player_get_rack(op)));
  }
  return spread;
}

// ----- per-candidate scenario evaluation -----------------------------------

typedef struct PegEvalCtx {
  const Game *base_game;
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  const Move *cand;
  int bag_size;
  int k_drawn;
  // 0 = greedy leaf (Stage 0); > 0 = emptier scenarios solved exactly with an
  // endgame_solve at this ply depth (non-emptier still uses the greedy leaf).
  int fidelity_plies;
  int64_t deadline_ns;
  ThreadControl *thread_control;
  PegWorker *worker;
  // accumulators
  double total_weight;
  double win_weight; // wins + 0.5 * draws, weighted
  double spread_weight;
  int64_t weight_sum;
  int n_scenarios;
} PegEvalCtx;

// Evaluate the leaf of one fully-resolved scenario (a specific post-cand game).
// Returns mover's signed spread (points) — exact via endgame_solve for emptier
// scenarios at fidelity > 0, else the greedy playout.
static int32_t peg_eval_leaf(PegEvalCtx *ctx, Game *game) {
  const bool emptier = bag_get_letters(game_get_bag(game)) == 0 &&
                       game_get_game_end_reason(game) == GAME_END_REASON_NONE;
  if (ctx->fidelity_plies <= 0 || !emptier) {
    return peg_greedy_playout(game, ctx->mover_idx, ctx->worker->playout_ml);
  }
  // Exact endgame leaf. After the mover plays and draws it is the opponent's
  // turn, so the solved value is from the on-turn player's perspective; fold
  // it into the mover lead accordingly.
  EndgameArgs ea;
  memset(&ea, 0, sizeof(ea));
  ea.thread_control = ctx->thread_control;
  ea.game = game;
  ea.plies = ctx->fidelity_plies;
  ea.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  ea.num_threads = 1;
  ea.use_heuristics = true;
  ea.num_top_moves = 1;
  ea.thread_index_offset = ctx->worker->thread_index_offset;
  ea.external_deadline_ns = ctx->deadline_ns;
  ea.shared_tt = ctx->worker->eg_tt;
  ea.seed = PEG_ENDGAME_SEED;
  endgame_results_reset(ctx->worker->eg_results);
  endgame_solve_inline(&ctx->worker->eg_ctx, &ea, ctx->worker->eg_results);
  const int eg_val =
      endgame_results_get_value(ctx->worker->eg_results, ENDGAME_RESULT_BEST);
  const Player *me = game_get_player(game, ctx->mover_idx);
  const Player *op = game_get_player(game, 1 - ctx->mover_idx);
  const int32_t mover_lead =
      equity_to_int(player_get_score(me) - player_get_score(op));
  const int turn = game_get_player_on_turn_index(game);
  return (turn == ctx->mover_idx) ? mover_lead + eg_val : mover_lead - eg_val;
}

// Evaluate one (mover_drawn, bag_remaining) split: walk the distinct orderings
// of bag_remaining (each equally likely), evaluate each leaf, and fold the
// multiset weight into the accumulator.
static void peg_eval_split(PegEvalCtx *ctx, const MachineLetter *mover_drawn,
                           int n_bag_remaining, MachineLetter *bag_remaining,
                           int64_t weight) {
  // Sort bag_remaining ascending so next_perm enumerates distinct orderings.
  for (int i = 1; i < n_bag_remaining; i++) {
    MachineLetter key = bag_remaining[i];
    int j = i - 1;
    while (j >= 0 && bag_remaining[j] > key) {
      bag_remaining[j + 1] = bag_remaining[j];
      j--;
    }
    bag_remaining[j + 1] = key;
  }
  double ordering_win = 0.0;
  double ordering_spread = 0.0;
  int n_orderings = 0;
  do {
    Game *game = peg_make_post_cand_game(
        ctx->worker, ctx->base_game, ctx->mover_idx, ctx->unseen, ctx->ld_size,
        ctx->cand, ctx->k_drawn, mover_drawn, n_bag_remaining, bag_remaining);
    const int32_t value = peg_eval_leaf(ctx, game);
    ordering_win += (value > 0) ? 1.0 : ((value == 0) ? 0.5 : 0.0);
    ordering_spread += (double)value;
    n_orderings++;
  } while (peg_next_perm(bag_remaining, n_bag_remaining));

  // Each ordering is equally likely within this multiset, so the multiset's
  // weight is split evenly across its orderings.
  ctx->total_weight += (double)weight;
  ctx->win_weight += (double)weight * (ordering_win / n_orderings);
  ctx->spread_weight += (double)weight * (ordering_spread / n_orderings);
  ctx->weight_sum += weight;
  ctx->n_scenarios += n_orderings;
}

// Recursively choose, per machine letter, how many tiles go to the mover's
// draw (m) and to the bag remainder (b), with m+b <= unseen[ml], mover total ==
// k_drawn and bag-remainder total == n_bag_remaining. Opp gets the complement.
static void peg_enum_splits(PegEvalCtx *ctx, int ml, int mover_left,
                            int bag_rem_left, int64_t weight,
                            MachineLetter *mover_drawn, int n_mover,
                            MachineLetter *bag_remaining, int n_bag_rem) {
  if (ml == ctx->ld_size) {
    if (mover_left == 0 && bag_rem_left == 0) {
      // k_drawn! accounts for the order in which the mover draws its tiles.
      int64_t full_weight = weight;
      for (int f = 2; f <= ctx->k_drawn; f++) {
        full_weight *= f;
      }
      peg_eval_split(ctx, mover_drawn, n_bag_rem, bag_remaining, full_weight);
    }
    return;
  }
  const int avail = ctx->unseen[ml];
  const int max_mover = mover_left < avail ? mover_left : avail;
  for (int m = 0; m <= max_mover; m++) {
    const int max_bag = bag_rem_left < (avail - m) ? bag_rem_left : (avail - m);
    for (int b = 0; b <= max_bag; b++) {
      const int64_t add_weight =
          peg_binomial(avail, m) * peg_binomial(avail - m, b);
      for (int i = 0; i < m; i++) {
        mover_drawn[n_mover + i] = (MachineLetter)ml;
      }
      for (int i = 0; i < b; i++) {
        bag_remaining[n_bag_rem + i] = (MachineLetter)ml;
      }
      peg_enum_splits(ctx, ml + 1, mover_left - m, bag_rem_left - b,
                      weight * add_weight, mover_drawn, n_mover + m,
                      bag_remaining, n_bag_rem + b);
    }
  }
}

// One candidate-evaluation job (cand at a given fidelity), dispatched to a
// pool worker or run inline.
typedef struct PegCandJob {
  const Game *base_game;
  int mover_idx;
  const uint8_t *unseen;
  int ld_size;
  const Move *cand;
  int bag_size;
  int fidelity_plies;
  int64_t deadline_ns;
  ThreadControl *thread_control;
  PegWorker *workers; // array; indexed by worker_idx
  PegRankedCand *out;
} PegCandJob;

static void peg_cand_worker_fn(void *arg, int worker_idx) {
  PegCandJob *job = (PegCandJob *)arg;
  PegEvalCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.base_game = job->base_game;
  ctx.mover_idx = job->mover_idx;
  ctx.unseen = job->unseen;
  ctx.ld_size = job->ld_size;
  ctx.cand = job->cand;
  ctx.bag_size = job->bag_size;
  ctx.fidelity_plies = job->fidelity_plies;
  ctx.deadline_ns = job->deadline_ns;
  ctx.thread_control = job->thread_control;
  ctx.worker = &job->workers[worker_idx];
  const int tiles_played = move_get_tiles_played(job->cand);
  ctx.k_drawn = tiles_played < job->bag_size ? tiles_played : job->bag_size;
  const int n_bag_remaining = job->bag_size - ctx.k_drawn;
  MachineLetter mover_drawn[PEG_MAX_BAG + 1];
  MachineLetter bag_remaining[PEG_MAX_BAG + 1];
  peg_enum_splits(&ctx, /*ml=*/0, ctx.k_drawn, n_bag_remaining, /*weight=*/1,
                  mover_drawn, 0, bag_remaining, 0);
  job->out->move = *job->cand;
  job->out->win_pct =
      ctx.total_weight > 0 ? ctx.win_weight / ctx.total_weight : 0.0;
  job->out->mean_spread =
      ctx.total_weight > 0 ? ctx.spread_weight / ctx.total_weight : 0.0;
  job->out->weight_sum = ctx.weight_sum;
  job->out->n_scenarios = ctx.n_scenarios;
}

static int peg_rank_cmp(const void *lhs, const void *rhs) {
  const PegRankedCand *a = (const PegRankedCand *)lhs;
  const PegRankedCand *b = (const PegRankedCand *)rhs;
  const double a_key = a->win_pct + 1e-4 * a->mean_spread;
  const double b_key = b->win_pct + 1e-4 * b->mean_spread;
  if (a_key < b_key) {
    return 1;
  }
  if (a_key > b_key) {
    return -1;
  }
  return 0;
}

// Evaluate `n` candidate moves at `fidelity_plies`, writing ranked[i] for each.
// Parallel across candidates when a pool is present, else inline.
static void peg_eval_candidates(PegPool *pool, PegWorker *workers,
                                const Game *game, int mover_idx,
                                const uint8_t *unseen, int ld_size, int bag_size,
                                const Move *const *cands, int n,
                                int fidelity_plies, int64_t deadline_ns,
                                ThreadControl *thread_control,
                                PegRankedCand *ranked) {
  PegCandJob *jobs = malloc_or_die((size_t)n * sizeof(PegCandJob));
  for (int i = 0; i < n; i++) {
    jobs[i].base_game = game;
    jobs[i].mover_idx = mover_idx;
    jobs[i].unseen = unseen;
    jobs[i].ld_size = ld_size;
    jobs[i].cand = cands[i];
    jobs[i].bag_size = bag_size;
    jobs[i].fidelity_plies = fidelity_plies;
    jobs[i].deadline_ns = deadline_ns;
    jobs[i].thread_control = thread_control;
    jobs[i].workers = workers;
    jobs[i].out = &ranked[i];
  }
  if (pool) {
    void **ptrs = malloc_or_die((size_t)n * sizeof(void *));
    for (int i = 0; i < n; i++) {
      ptrs[i] = &jobs[i];
    }
    // Helper (main) thread uses the scratch slot past the worker range.
    peg_pool_submit_and_wait(pool, peg_cand_worker_fn, ptrs, n,
                             peg_pool_num_workers(pool));
    free(ptrs);
  } else {
    for (int i = 0; i < n; i++) {
      peg_cand_worker_fn(&jobs[i], 0);
    }
  }
  free(jobs);
}

// ----- result publishing ---------------------------------------------------

static void peg_publish(PegResult *out, const PegRankedCand *ranked, int count,
                        int stage) {
  free(out->top_cands);
  out->top_cands = malloc_or_die((size_t)count * sizeof(PegRankedCand));
  memcpy(out->top_cands, ranked, (size_t)count * sizeof(PegRankedCand));
  out->n_top_cands = count;
  out->best_move = ranked[0].move;
  out->best_win = ranked[0].win_pct;
  out->best_spread = ranked[0].mean_spread;
  out->last_completed_stage = stage;
}

// ----- public entry --------------------------------------------------------

void peg_solve(const PegArgs *args, PegResult *out, ErrorStack *error_stack) {
  memset(out, 0, sizeof(*out));
  out->last_completed_stage = -1;

  Timer timer;
  ctimer_start(&timer);

  const Game *game = args->game;
  const int bag_size = bag_get_letters(game_get_bag(game));
  if (bag_size < PEG_MIN_BAG || bag_size > PEG_MAX_BAG) {
    error_stack_push(
        error_stack, ERROR_STATUS_PEG_BAG_OUT_OF_RANGE,
        get_formatted_string("PEG requires a bag of %d..%d tiles, but found %d",
                             PEG_MIN_BAG, PEG_MAX_BAG, bag_size));
    return;
  }

  const int mover_idx = game_get_player_on_turn_index(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  peg_compute_unseen(game, mover_idx, unseen);

  // Per-stage halving counts: the override, else the built-in table tail.
  const int default_counts[PEG_NUM_STAGES - 1] = {32, 16, 8, 4, 2};
  const int *counts = (args->stage_top_k && args->num_stages > 0)
                          ? args->stage_top_k
                          : default_counts;
  int num_stages = (args->stage_top_k && args->num_stages > 0)
                       ? args->num_stages
                       : PEG_NUM_STAGES - 1;
  if (args->max_stage > 0 && args->max_stage < num_stages) {
    num_stages = args->max_stage;
  }

  // Wall-clock deadline (0 = unbounded). Each endgame leaf is also capped by
  // this so a single deep solve cannot overrun the budget.
  const double budget = args->time_budget_seconds;
  const int64_t deadline_ns =
      budget > 0.0 ? ctimer_monotonic_ns() + (int64_t)(budget * 1.0e9) : 0;

  // Per-worker scratch. One extra slot for the main thread when it helps the
  // pool drain the queue (helper index == num_workers).
  const int n_threads = args->num_threads > 1 ? args->num_threads : 1;
  PegPool *pool = n_threads > 1 ? peg_pool_create(n_threads, 0) : NULL;
  const int n_scratch = pool ? n_threads + 1 : 1;
  // Per-worker endgame TT. Shallow PEG endgames need little, and the total
  // across workers stays well under the 50%-RAM ceiling.
  double tt_fraction = 0.25 / (double)n_scratch;
  if (tt_fraction > 0.05) {
    tt_fraction = 0.05;
  }
  PegWorker *workers = malloc_or_die((size_t)n_scratch * sizeof(PegWorker));
  for (int w = 0; w < n_scratch; w++) {
    workers[w].playout_ml = move_list_create(1);
    workers[w].eg_ctx = NULL;
    workers[w].eg_results = endgame_results_create();
    workers[w].scratch_game = NULL;
    workers[w].eg_tt = transposition_table_create(tt_fraction);
    // Distinct MoveGen cleanup slots per worker; the +2 clears the endgame
    // display/solver reserved slots.
    workers[w].thread_index_offset = w * 4 + 2;
  }

  // Candidate generation (full list, equity-sorted).
  MoveList *cand_ml = move_list_create(PEG_CAND_LIST_CAP);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = 0,
      .move_list = cand_ml,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&gen_args);
  const int n_cands = move_list_get_count(cand_ml);

  if (n_cands > 0) {
    PegRankedCand *ranked =
        malloc_or_die((size_t)n_cands * sizeof(PegRankedCand));
    const Move **moves = malloc_or_die((size_t)n_cands * sizeof(Move *));

    // Stage 0: greedy evaluation of every candidate.
    for (int i = 0; i < n_cands; i++) {
      moves[i] = move_list_get_move(cand_ml, i);
    }
    peg_eval_candidates(pool, workers, game, mover_idx, unseen, ld_size,
                        bag_size, moves, n_cands, /*fidelity_plies=*/0,
                        deadline_ns, args->thread_control, ranked);
    qsort(ranked, (size_t)n_cands, sizeof(PegRankedCand), peg_rank_cmp);
    int survivors = n_cands < counts[0] ? n_cands : counts[0];
    peg_publish(out, ranked, survivors, /*stage=*/0);

    // Halving stages. Stage s re-evaluates the surviving top counts[s-1] at one
    // more ply of fidelity, then re-ranks; a stage needs >= 2 candidates to be
    // meaningful, and is skipped once the budget is spent.
    for (int s = 1; s <= num_stages; s++) {
      const int eval_count = n_cands < counts[s - 1] ? n_cands : counts[s - 1];
      if (eval_count < 2) {
        break;
      }
      if (deadline_ns != 0 && ctimer_monotonic_ns() >= deadline_ns) {
        break;
      }
      for (int i = 0; i < eval_count; i++) {
        moves[i] = &ranked[i].move;
      }
      // Evaluate into a separate buffer: moves[] aliases ranked[].move, so the
      // worker must not overwrite ranked[i] while later moves still point into
      // it.
      PegRankedCand *restaged =
          malloc_or_die((size_t)eval_count * sizeof(PegRankedCand));
      peg_eval_candidates(pool, workers, game, mover_idx, unseen, ld_size,
                          bag_size, moves, eval_count, /*fidelity_plies=*/s + 1,
                          deadline_ns, args->thread_control, restaged);
      qsort(restaged, (size_t)eval_count, sizeof(PegRankedCand), peg_rank_cmp);
      memcpy(ranked, restaged, (size_t)eval_count * sizeof(PegRankedCand));
      free(restaged);
      peg_publish(out, ranked, eval_count, s);
    }

    free(moves);
    free(ranked);
  }

  move_list_destroy(cand_ml);
  for (int w = 0; w < n_scratch; w++) {
    move_list_destroy(workers[w].playout_ml);
    endgame_ctx_destroy(workers[w].eg_ctx);
    endgame_results_destroy(workers[w].eg_results);
    if (workers[w].scratch_game) {
      game_destroy(workers[w].scratch_game);
    }
    transposition_table_destroy(workers[w].eg_tt);
  }
  free(workers);
  if (pool) {
    peg_pool_destroy(pool);
  }
  out->elapsed_seconds = ctimer_elapsed_seconds(&timer);
}

void peg_result_destroy(PegResult *r) {
  if (!r) {
    return;
  }
  free(r->top_cands);
  r->top_cands = NULL;
  r->n_top_cands = 0;
  free(r->per_scenario);
  r->per_scenario = NULL;
  r->n_per_scenario = 0;
}
