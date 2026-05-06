#ifndef BAI_PEG_H
#define BAI_PEG_H

/*
 * BAI-style adaptive pre-endgame solver. Picks the best move on a
 * 1-in-bag position by allocating endgame compute adaptively across
 * (candidate, depth) pairs using a PUCT-like selection rule, evaluating
 * each candidate against every possible bag tile to integrate over the
 * unseen-tile distribution. Anytime: the best-so-far candidate is
 * meaningful at every moment, and the caller can stop on wall-clock
 * time, evaluation budget, or a confidence-based early exit when the
 * leader is statistically dominant.
 *
 * Background and further reading:
 *
 * PUCT selection rule. The variant here uses the AlphaZero formula
 *     bonus = c * prior * sqrt(N_parent) / (1 + N_child)
 * with `N` measured in cumulative wall-time spent, so cheap candidates
 * accumulate visits faster and exploration narrows once a leader emerges:
 *   AlphaZero (Silver et al. 2017, arXiv:1712.01815)
 *     https://arxiv.org/abs/1712.01815
 *   Original predictor-UCT formulation:
 *     Rosin 2011, "Multi-armed bandits with episode context"
 *     https://link.springer.com/article/10.1007/s10472-011-9258-6
 *
 * Progressive widening with `active = ceil(c * sqrt(visits))` keeps the
 * effective candidate pool small at low budget and grows it as the
 * search refines. New candidates inherit Q from their static-score
 * neighbor at admission so PUCT inputs are sensible without a cold
 * playout:
 *   Chaslot et al. 2008, "Progressive Strategies for Monte-Carlo
 *   Tree Search"
 *     https://dke.maastrichtuniversity.nl/m.winands/documents/pMCTS.pdf
 *
 * Best-Arm Identification framing (anytime stopping, confidence-based
 * early exit). Selection here is PUCT rather than a BAI sampling rule,
 * but the stopping framework is BAI-flavored. See bai.h for the BAI
 * sampling rules implemented in this repo over a fixed arm set.
 */

#include "../def/game_defs.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../util/io_util.h"

// Maximum endgame plies the solver will explore per candidate. Hard-capped
// at MAX_SEARCH_DEPTH (25) by endgame_solve.
enum { BAI_PEG_MAX_DEPTH = 25 };

// Caller-supplied callback fired whenever a candidate's depth advances (or
// the solver wants to checkpoint progress). Receives the current ranking
// (by mean spread, descending) so the caller can render an intermediate
// answer or decide to terminate.
typedef void (*BaiPegProgressCallback)(
    int evaluations_done, double seconds_elapsed, const SmallMove *ranked_moves,
    const double *ranked_win_pcts, const double *ranked_mean_spreads,
    const int *ranked_depths_evaluated, int num_ranked, const Game *game,
    void *user_data);

typedef struct BaiPegArgs {
  // Game must have exactly 1 tile in the bag.
  const Game *game;
  ThreadControl *thread_control;
  int num_threads;

  // Memory fraction for the shared transposition table used by inner
  // endgame solves. 0 = no TT (each scenario solves cold).
  double tt_fraction_of_mem;
  // If non-NULL, all endgame solves share this TT instead of creating a
  // private one. Caller owns the lifetime.
  TranspositionTable *shared_tt;

  // Optional: per-thread TT pointers. When non-NULL, each worker thread
  // gets its own dedicated TT (eliminating cross-core cache contention on
  // the shared one). Length must equal num_threads. Caller owns lifetime.
  TranspositionTable **shared_tt_per_thread;

  dual_lexicon_mode_t dual_lexicon_mode;

  // Base offset added to every internal thread index (for initial movegen,
  // playout/eval workers, and EndgameArgs.thread_index_offset on each
  // scenario solve). Move generation uses a global per-thread cache keyed
  // by thread_index, so concurrent bai_peg_solve calls in the same process
  // must use disjoint [thread_index_offset, thread_index_offset+num_threads)
  // ranges to avoid corrupting each other's cache entries. 0 is fine for a
  // single-threaded caller.
  int thread_index_offset;

  // After greedy generation, keep at most this many candidates by static
  // score for the adaptive phase. 0 = use a sensible default.
  int initial_top_k;

  // Maximum depth (plies) any candidate is searched to. Capped to
  // BAI_PEG_MAX_DEPTH internally. 0 = use BAI_PEG_MAX_DEPTH.
  int max_depth;

  // Per-scenario endgame solve time budget (seconds). Each (cand, depth)
  // evaluation runs the endgame at depth `depth` with this time cap on each
  // of the bag-tile scenarios. 0 = unbounded (depth alone gates).
  double endgame_time_per_solve;

  // Total wall-clock budget for the whole bai_peg_solve call (seconds).
  // 0 = no limit; rely on max_evaluations / confidence stop.
  double time_budget_seconds;

  // Hard cap on the number of (candidate, depth) evaluations performed
  // across the whole solve. 0 = no cap.
  int max_evaluations;

  // Confidence-based early stop. If the leader's mean spread exceeds the
  // best challenger's mean spread by `early_stop_gap` AND both have been
  // evaluated to at least `early_stop_min_depth` plies, terminate. 0/0
  // disables the early-stop heuristic and only time / max_evaluations stop.
  double early_stop_gap;
  int early_stop_min_depth;

  // PUCT exploration constant. Higher = more exploration of candidates
  // with weaker priors but unexplored depth. ~1.0 is a sane default.
  double puct_c;

  // Utility weighting: optimize U = win_pct + utility_alpha * mean_spread.
  // 0 = pure win% (default); 0.01 = "1 cent per point, $1 per game" (spread
  // of 100 = 1 win); higher values emphasize spread further. Affects PUCT
  // selection, candidate ranking, and the final pick.
  double utility_alpha;

  // If true, populate result->cand_stats with per-candidate snapshots
  // (playout_q, neighbor_q, final_q, depth, etc.) for offline analysis.
  // Caller must bai_cand_stats_free(result->cand_stats) when done.
  bool request_cand_stats;

  // Blend weights for the initial Q assigned at widening admission, when
  // BOTH playout and neighbor signals are available. Weights should sum to
  // ~1; the empirical regression at d=1-2 found w_neighbor≈0.6, w_playout
  // ≈0.4. Set both to 0 to disable (use whatever signal is available alone).
  double blend_w_playout;
  double blend_w_neighbor;

  // If true, evaluate every retained candidate at depth=0 (static greedy
  // playout to end-of-game) before entering the PUCT loop. Gives a
  // scenario-aware Q prior so weak candidates get pruned/de-prioritized
  // earlier than they would from the move-score prior alone. Cheap (~ms
  // per candidate) compared to a real depth-1 endgame. Ignored when
  // progressive_widening is true (which uses lazy first-visit playouts).
  bool initial_playout;

  // If true and initial_playout is also true, use a streamlined greedy
  // playout (movegen + play, no endgame_solve plumbing, no TT, no alpha-
  // beta) to compute the d=0 prior. ~10-100x faster than going through
  // endgame_solve_inline at plies=0. Only meaningful with initial_playout.
  bool pure_playout;

  // If true, use progressive widening with lazy playout instead of the
  // top-K-fixed pre-staged design. Behavior:
  //   - initial_top_k is ignored; ALL generated moves stay in the pool.
  //   - No upfront Phase 0 / Phase 1 sweeps. PUCT handles everything.
  //   - At any point, only the top ceil(widening_c * sqrt(total_visits))
  //     candidates (by move-score order) are considered for selection;
  //     this grows over time so weak moves never get touched at short
  //     budgets.
  //   - At admission, a newly-active candidate inherits Q from its
  //     rank-up neighbor (a calibration-free seed); no playout is run.
  //     If initial_playout is also set, every candidate additionally gets
  //     a Phase 0 depth-0 greedy playout up front.
  //   - First negamax visit to a candidate is at depth=1; subsequent
  //     visits do depth=2, 3, ...
  //   - post_cand_game is created lazily on first visit, not pre-staged.
  // Solves the failure mode where k=256 + Phase 0 playout actively misleads
  // PUCT at short budgets (e.g. 3s).
  bool progressive_widening;
  // Widening constant: active = ceil(widening_c * sqrt(total_visits)).
  // 0 = use a sane default (2.0). Higher = wider; lower = narrower.
  double widening_c;
  // Minimum active set size when progressive_widening is on. Defaults to 0
  // (widening starts at 2 and grows). When > 0 the top min_active cands
  // are unconditionally active from the start AND get a Phase 1 d=1
  // warmup; widening grows the active set beyond min_active as visits
  // accumulate. Combines a reliable narrow-baseline (k=32 d=1 warmup)
  // with the option to widen if budget permits.
  int min_active;

  // Optional progress callback invoked after every depth advancement.
  // num_top is how many ranked candidates to surface to the callback.
  BaiPegProgressCallback progress_callback;
  void *progress_callback_data;
  int progress_num_top;
} BaiPegArgs;

// Per-candidate snapshot for downstream regression / diagnostics. Captures
// the two prior signals (playout, neighbor) at the moment they were set,
// alongside the final Q reached by negamax. Comparing playout vs neighbor
// vs final tells us which signal is more predictive at each candidate's
// final search depth.
typedef struct BaiCandStats {
  SmallMove move;
  int static_score;
  int rank; // 0 = best static score
  // Playout signal (Phase 0). Only populated when initial_playout was on.
  bool playout_q_set;
  double playout_q_win_pct;
  double playout_q_mean_spread;
  // Neighbor signal (progressive widening). Snapshot of cand[rank-1]'s q
  // at the moment cand[rank] was admitted to the active set. Only populated
  // when progressive_widening was on and rank > 0.
  bool neighbor_q_set;
  double neighbor_q_win_pct;
  double neighbor_q_mean_spread;
  // Final Q after all evaluations.
  double final_q_win_pct;
  double final_q_mean_spread;
  int depth_evaluated;
  int visits;
  bool is_best; // True for the picked move.
} BaiCandStats;

typedef struct BaiPegResult {
  SmallMove best_move;
  double best_win_pct;      // [0, 1] over the bag-tile distribution
  double best_mean_spread;  // Average spread (mover's perspective)
  int best_depth_evaluated; // Deepest plies the best move was searched to

  // Diagnostics.
  int candidates_considered; // Top-K size after greedy
  int evaluations_done;      // Total (cand, depth) evaluations completed
  // Per-depth visit count: visits_at_depth[d] = number of distinct candidates
  // that finished an evaluation at depth d. Sum across all depths equals
  // evaluations_done. Index 0 is unused (depths start at 1).
  int visits_at_depth[BAI_PEG_MAX_DEPTH + 1];
  double seconds_elapsed;
  bool stopped_by_confidence;
  bool stopped_by_time;
  bool stopped_by_max_evals;
  // If non-NULL on input, allocated by bai_peg_solve and filled with one
  // BaiCandStats per cand (length = candidates_considered). Caller must
  // free via bai_cand_stats_free. NULL = no stats requested.
  BaiCandStats *cand_stats;
} BaiPegResult;

// Free the per-candidate stats array allocated by bai_peg_solve. Safe on NULL.
void bai_cand_stats_free(BaiCandStats *cand_stats);

void bai_peg_solve(const BaiPegArgs *args, BaiPegResult *result,
                   ErrorStack *error_stack);

#endif
