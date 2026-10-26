#include "endgame.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/csched.h"
#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/cross_set_cache.h"
#include "../ent/dictionary_word.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/lane_shadow_cache.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/move_undo.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/small_move_arena.h"
#include "../ent/static_eval.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../ent/xoshiro.h"
#include "../ent/zobrist.h"
#include "../str/letter_distribution_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/stable_sort.h"
#include "../util/string_util.h"
#include "gameplay.h"
#include "kwg_maker.h"
#include "move_gen.h"
#include "path_move_lists.h"
#include "word_prune.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Endgame move lists start with this many slots and double when a generation
  // fills them. Over the notes/peg_positions PEG solves, 99.97% of generations
  // produced fewer than 4,096 plays and the largest produced 20,479.
  ENDGAME_MOVELIST_INITIAL_CAPACITY = 4096,
  // Maximum moves for stack allocation in ABDADA deferred tracking
  // Keep small to avoid stack overflow in deep recursive searches
  MAX_DEFERRED_STACK = 64,
  // ABDADA: nodes at least this many plies above the leaves mark themselves
  // busy while they are searched, and a busy one reached as a younger brother
  // is deferred. Depth-2 nodes are worth sharing out: two threads in the same
  // depth-3 node otherwise both search every one of its children.
  ABDADA_MIN_DEPTH = 2,
  // How many abdada_negamax calls between per-depth deadline checks.
  // Checked per-worker (no cross-thread contention). At ~100K nodes/s/thread,
  // 1024 nodes ≈ 10ms granularity — tighter response for PEG inner solves
  // where each scenario's endgame budget can be tens of ms.
  DEPTH_DEADLINE_CHECK_INTERVAL = 1024,
  // Points added to a play that uses the whole rack when picking an interior
  // node's first reply before its move list exists, so going out ranks first,
  // as GOING_OUT_BF makes the move ordering rank it.
  FIRST_REPLY_GOING_OUT_PRIORITY = 100000,
  // Bit flags for move estimates. These large numbers will force these
  // estimated values to sort first.
  LARGE_VALUE = 1 << 30, // for alpha-beta pruning
  EARLY_PASS_BF = 1 << 29,
  HASH_MOVE_BF = 1 << 28,
  GOING_OUT_BF = 1 << 27,
  // ABDADA: sentinel value returned when node is being searched by another
  // processor
  ON_EVALUATION = -(1 << 29),
  ABDADA_INTERRUPTED = -(1 << 28),
  // Aspiration window initial size
  ASPIRATION_WINDOW = 25,
  // Lazy move ordering at interior nodes: selection-pick the best remaining
  // move for this many indices before sorting the rest once. Beta cutoffs
  // usually happen within the first few moves, so most nodes never pay for
  // a full sort of the move list.
  LAZY_SELECTION_LIMIT = 8,
  // Conservation bonus weights: penalize playing tiles when opponent is stuck
  CONSERVATION_TILE_WEIGHT = 7,
  CONSERVATION_VALUE_WEIGHT = 2,
  // Random noise range for thread jitter, centered around zero
  THREAD_JITTER_NOISE = 8,
  // Max top-K ranked PVLines built for the per-ply display/callback.
  MAX_RANKED_CALLBACK_PVS = 10,
  // Default cap on the depth-0 interrupt-fallback sweep when first_win_optim is
  // set: 0 = use this default, <0 = skip the sweep, >0 = explicit cap.
  FIRST_WIN_D0_FALLBACK_MOVES = 12,
  // Most moves a greedy leaf playout plays; in practice the game ends first.
  EG_MAX_LEAF_PLAYOUT = MAX_SEARCH_DEPTH + 1,
  // Most moves on a worker's board at once: one per slot of its move_undos
  // and pass_undos.
  EG_MAX_PLAYED_MOVES = (2 * MAX_SEARCH_DEPTH) + EG_MAX_LEAF_PLAYOUT + 1,
  // sort_small_moves_by_estimate sorts runs this short by insertion.
  SMALL_MOVE_INSERTION_SORT_MAX = 16,
  // compute_build_chain_values' lines: passes, then each row, then each
  // column.
  BUILD_CHAIN_LINES = 1 + 2 * BOARD_DIM,
  // The int arrays of move_count entries compute_build_chain_values keeps in
  // its scratch.
  BUILD_CHAIN_SCRATCH_ARRAYS = 6,
  // Entries in a worker's cache of the opponent's playable tile types; a
  // power of two.
  STUCK_TILES_CACHE_SIZE = 1 << 15,
  // Entries in a worker's cache of the rest of stuck playouts; a power of
  // two.
  PLAYOUT_TAIL_CACHE_SIZE = 1 << 15,
};

// A stuck-tile greedy playout extends the incremental lists' path past the
// negamax leaf and stops where the path is full; see
// negamax_greedy_leaf_playout's max_playout. Leave room for at least one move
// past the deepest negamax and bypass path.
static_assert(PATH_MOVE_LISTS_MAX_PATH > 2 * MAX_SEARCH_DEPTH + 1,
              "incremental lists' path too short for the greedy playout");

// Salt for the transposition-table key of a capped leaf; see leaf_tt_key.
static const uint64_t CAPPED_LEAF_KEY_SALT = 0x9E3779B97F4A7C15ULL;
// Salt for the key of the rest of a stuck playout, whose top half also carries
// the stuck fraction's bits; see negamax_greedy_leaf_playout.
static const uint64_t PLAYOUT_TAIL_KEY_SALT = 0xC2B2AE3D27D4EB4FULL;
static_assert(sizeof(float) == sizeof(uint32_t),
              "a stuck fraction's bits fill half a key");
// What each solve adds to a worker's stuck-tiles cache salt.
static const uint64_t STUCK_TILES_SALT_STEP = 0xD1B54A32D192ED03ULL;

// Returns fraction of opponent's rack score that is stuck (0.0 = none, 1.0 =
// all). A tile is "stuck" if no legal move plays that tile type.
// tiles_played_bv: bitvector where bit i is set if machine letter i appears in
//   any valid move (from MOVE_RECORD_TILES_PLAYED).
static float stuck_tile_fraction_from_bv(const LetterDistribution *ld,
                                         const Rack *rack,
                                         uint64_t tiles_played_bv) {
  int total_score = 0;
  int stuck_score = 0;
  int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    int count = rack_get_letter(rack, ml);
    if (count > 0) {
      int score = count * ld_get_score(ld, ml);
      total_score += score;
      if (!(tiles_played_bv & ((uint64_t)1 << ml))) {
        stuck_score += score;
      }
    }
  }
  if (total_score == 0) {
    return 0.0F;
  }
  return (float)stuck_score / (float)total_score;
}

typedef struct EndgameCtxWorker EndgameCtxWorker;

struct EndgameCtx {
  int initial_spread;
  int solving_player;

  int initial_small_move_arena_size;
  bool iterative_deepening_optim;
  bool first_win_optim;
  // Cap on the first-win depth-0 interrupt fallback sweep (root moves greedy-
  // evaluated). 0 = built-in default, <0 = skip the sweep, >0 = explicit cap.
  int first_win_fallback_moves;
  bool initial_window_optim;
  int32_t initial_window_alpha;
  int32_t initial_window_beta;
  bool transposition_table_optim;
  bool negascout_optim;
  bool use_heuristics;
  bool forced_pass_bypass;
  bool incremental_movegen;
  // EndgameArgs.disable_best_reply_shortcuts, inverted.
  bool best_reply_shortcuts;
  PVLine principal_variation;
  // If non-NULL, guarantee an exact value for this move alongside the best
  // move (see EndgameArgs.actual_move). Owned by the caller; valid only for
  // the duration of the solve.
  const Move *actual_move;

  KWG *pruned_kwgs[2];
  // Each tile type's face value, by machine letter, for the sums of the tiles
  // moves place.
  int tile_face_values[MAX_ALPHABET_SIZE];
  dual_lexicon_mode_t dual_lexicon_mode;
  double soft_time_limit;
  double hard_time_limit;

  int num_top_moves;
  int requested_plies;
  int threads;
  double tt_fraction_of_mem;
  TranspositionTable *transposition_table;
  bool tt_is_external; // true when using a caller-provided shared TT

  // Depth-0 root sweep shared by every worker (see root_greedy_sweep): the
  // root moves in thread 0's static-estimate order, published once per solve
  // (root_sweep_count stays -1 until then), and the next index to claim.
  SmallMove *root_sweep_moves;
  int root_sweep_capacity;
  atomic_int root_sweep_count;
  atomic_int root_sweep_next;

  // Signal for threads to stop early (0=running, 1=done)
  atomic_int search_complete;
  // Per-depth deadline: absolute CLOCK_MONOTONIC nanoseconds; 0 = disabled.
  // Thread 0 sets this after each completed depth (from EBF estimate).
  // All worker threads check it periodically and bail if exceeded.
  _Atomic int64_t depth_deadline_ns;
  // Caller-supplied absolute deadline (CLOCK_MONOTONIC ns). 0 = disabled.
  // check_depth_deadline takes min(this, depth_deadline_ns) so a caller's
  // wall-clock budget always wins over the EBF-projected per-depth budget.
  int64_t external_deadline_ns;
  // The move's budget, from the thread that started the solve: every worker,
  // the late ones too, charges it, and check_depth_deadline stops on it.
  MovegenBudget *movegen_budget;
  // Flag: stuck-tile mode has been logged (0=not yet, 1=logged)
  atomic_int stuck_tile_logged;
  // Fraction of opponent's tiles that are stuck at the root (0.0 = none)
  float initial_opp_stuck_frac;

  // Root move progress tracking (thread 0 only, for external polling)
  atomic_int root_moves_completed; // How many root moves finished this depth
  atomic_int root_moves_total;     // Total root moves at this depth
  atomic_int current_depth;        // Depth currently being searched

  // Ply-2 progress tracking (children of root move #1, thread 0 only)
  atomic_int ply2_moves_completed;
  atomic_int ply2_moves_total;

  // Per-ply callback for iterative deepening progress
  EndgamePerPlyCallback per_ply_callback;
  void *per_ply_callback_data;
  // Fired once on thread 0 before iterative deepening begins, with the sorted
  // d=0 root-move list (for an initial leaderboard render).
  EndgameBeforeSearchCallback before_search_callback;
  void *before_search_callback_data;
  // Fired on thread 0 each time a root move completes at the current IDS depth
  // (for live per-root leaderboard updates).
  EndgamePerRootMoveCallback per_root_move_callback;
  void *per_root_move_callback_data;

  // Owned by the caller:
  EndgameResults *results;
  ThreadControl *thread_control;
  const Game *game;

  // Reusable game copy for PV extension (avoid repeated game_duplicate calls).
  // Lazily initialized on first use; reset via game_copy on subsequent uses.
  Game *ext_game;

  // Persistent worker pool (reused across solves).
  // workers_ld tracks the LD the pool was allocated against; if it changes
  // (e.g., different lexicon), the pool is destroyed and rebuilt to avoid
  // bag_copy into incompatibly-sized allocations.
  EndgameCtxWorker **workers;
  cpthread_t *worker_ids;
  // _Atomic so the lock-free live-view accessors can read it (acquire) while
  // endgame_add_worker grows it (under add_mutex) during a solve.
  atomic_int cap_workers; // created worker structs (workers[0..cap_workers))
  int arr_cap; // allocated length of workers[]/worker_ids[] (>= cap_workers)
  const LetterDistribution *workers_ld;
  // Dynamic worker injection (endgame_add_worker): grow an in-flight solve's
  // worker count up to arr_cap as cores free up. All guarded by add_mutex.
  int max_workers;           // ceiling for live_workers; 0 => no growth
  uint64_t worker_base_seed; // ABDADA jitter seed, reused for late workers
  atomic_int live_workers;   // worker threads spawned this solve (initial+late)
  atomic_int adding_closed; // 1 = injection window shut (between/ending solves)
  _Atomic int64_t window_open_ns; // monotonic ns when the window last opened
  cpthread_mutex_t add_mutex; // serializes add vs the close+snapshot in solve
};

// Which squares hold a tile, row by row (bit col of occupied_rows[row]), and
// which cannot take one (holding a tile or bricked), for the scans of the
// squares a single tile can drop on. Read from the letters on the board, or
// from the occupancy before the last move plus that move's tiles; the board's
// own records of which squares hold cross words can be stale after an
// unplay.
typedef struct BoardOccupancy {
  uint32_t occupied_rows[BOARD_DIM];
  uint32_t unavailable_rows[BOARD_DIM];
} BoardOccupancy;

// A board and opponent rack, by stuck_tiles_key, and the tile types of that
// rack that appear in at least one of the opponent's plays there. Key 0 is
// empty.
typedef struct StuckTilesEntry {
  uint64_t key;
  uint64_t tiles_bv;
} StuckTilesEntry;

// A position a stuck playout passed through, by its key (see
// negamax_greedy_leaf_playout), with the value of the rest of the playout
// from there, from the solving player's side, and the rest's length. Key 0 is
// empty.
typedef struct PlayoutTailEntry {
  uint64_t key;
  int32_t value;
  int32_t length;
} PlayoutTailEntry;

// What the opponent's stuck-tile check knows from the position before the
// last move: the tile types playable there, and the squares the last move
// changed for each lane's plays (see path_move_lists_last_move_lanes). A play
// spanning none of those squares was a play there too, and the opponent's
// rack has lost tiles since or stayed the same, so a tile type that had no
// play there has one now only in a play through a changed square.
typedef struct StuckTilesHint {
  uint64_t playable_before;
  uint64_t lanes;
  const uint32_t *lane_hits;
} StuckTilesHint;

struct EndgameCtxWorker {
  // Worker ordinal within this solve (0..live_workers-1, counting helpers lent
  // by endgame_add_worker; == position in the cached workers[] array, so
  // stable across solves). ordinal 0 is the main worker: it owns root move
  // ordering, ply-2 tracking, the soft-time check, and PV display. Drives
  // per-worker jitter parity. Each worker reaches move generation through
  // get_movegen(), which auto-assigns a distinct per-pthread MoveGen cache
  // slot, so no explicit slot is tracked here.
  int ordinal;
  Game *game_copy;
  Arena *small_move_arena;
  MoveList *move_list;
  // The incremental move lists (EndgameArgs.incremental_movegen), or NULL
  // when the current solve generates from scratch at every node. The lists
  // themselves are allocated on the first solve that enables them and kept
  // in path_lists_storage for the worker's lifetime.
  PathMoveLists *path_lists;
  PathMoveLists *path_lists_storage;
  EndgameCtx *solver;
  int current_iterative_deepening_depth;
  // Counts heuristic events (depth-limited leaf evaluations and hits on
  // transposition entries that are not pure). A search during which the count
  // does not move proved its value from terminal positions only, so the value
  // holds at any depth.
  uint64_t impure_events;
  // Array of MoveUndo structures for incremental play/unplay. Negamax's moves
  // occupy the slots below requested_plies; the greedy leaf playout continues
  // from slot requested_plies. Sized so the playout always has
  // EG_MAX_LEAF_PLAYOUT slots, whatever the requested depth: a time-limited
  // solve requests MAX_SEARCH_DEPTH plies and stops iterative deepening on
  // the clock, and its leaves must still play out to the end of the game.
  MoveUndo move_undos[MAX_SEARCH_DEPTH + EG_MAX_LEAF_PLAYOUT];
  // Per-depth MoveUndo for forced-pass bypass. Pass recurses at the same depth
  // so it cannot share move_undos[]; indexed by the depth parameter (0..25).
  MoveUndo pass_undos[MAX_SEARCH_DEPTH + 1];
  // The undos of the moves on game_copy, oldest first. Moves are played with
  // their anchor and cross-set updates deferred, and update_deferred_moves
  // applies those of the moves from first_deferred_move up just before
  // anything reads the board. A child answered from the transposition table
  // never looks at its board, so it never pays for them.
  // first_deferred_move_at_play[i] is first_deferred_move before move i was
  // played; unplaying move i restores it.
  MoveUndo *played_move_undos[EG_MAX_PLAYED_MOVES];
  int first_deferred_move_at_play[EG_MAX_PLAYED_MOVES];
  int num_played_moves;
  int first_deferred_move;
  // The occupancy of the board with the first i played moves on it, valid
  // when occupancy_valid[i]: playing a move clears the flag of the level it
  // leads to, and a node sets its level's from the level below.
  BoardOccupancy occupancy_stack[EG_MAX_PLAYED_MOVES + 1];
  bool occupancy_valid[EG_MAX_PLAYED_MOVES + 1];
  // Scratch, grown as needed, for sort_small_moves_by_estimate and for
  // compute_build_chain_values (its values and its bookkeeping). The
  // capacities sit together, ahead of the pointers, to limit the padding.
  int sort_scratch_capacity;
  int build_chain_scratch_capacity;
  SmallMove *sort_scratch;
  int *build_chain_scratch;
  // The opponent's playable tile types by board and opponent rack (see
  // compute_opp_stuck_fraction), with keys salted per solve so that entries
  // from an earlier solve, perhaps in another lexicon, never match.
  StuckTilesEntry *stuck_tiles_cache;
  uint64_t stuck_tiles_salt;
  // The rest of the stuck playouts that passed through a position, with keys
  // salted by stuck_tiles_salt too (see negamax_greedy_leaf_playout).
  PlayoutTailEntry *playout_tail_cache;
  // The lane shadows of the worker's best-play generations, salted by
  // stuck_tiles_salt too (see lane_shadow_cache.h).
  LaneShadowCache *lane_shadow_cache;
  // The classic cross sets of the worker's game, salted by stuck_tiles_salt
  // too, since each solve prunes its own lexica (see cross_set_cache.h).
  CrossSetCache *cross_set_cache;
  // The opponent's playable tile types at each position along the incremental
  // lists' path, by path length, valid while stuck_tiles_stamp[length] is the
  // stamp of that position's prefix (see worker_opp_stuck_fraction). A stamp
  // of 0 is no entry.
  uint64_t stuck_tiles_playable[PATH_MOVE_LISTS_MAX_PATH + 1];
  uint64_t stuck_tiles_stamp[PATH_MOVE_LISTS_MAX_PATH + 1];
  XoshiroPRNG *prng;       // Per-thread PRNG for jitter
  PVLine best_pv;          // Thread-local best PV
  int32_t best_pv_value;   // Thread-local best value
  int completed_depth;     // Depth this thread completed
  int n_initial_moves;     // Number of root moves (thread-local to avoid races)
  bool in_first_root_move; // True when thread 0 is inside root move idx==0
  // Counter for throttling per-depth deadline checks in abdada_negamax
  uint64_t nodes_since_deadline_check;
  // The playout_tail_cache key and the solving player's spread of each
  // position a stuck playout passes through, by its move count (see
  // negamax_greedy_leaf_playout).
  uint64_t playout_tail_keys[EG_MAX_LEAF_PLAYOUT + 1];
  int32_t playout_tail_spreads[EG_MAX_LEAF_PLAYOUT + 1];
  // The line of the depth in progress whose root move beat every root move
  // searched before it at that depth, or no moves when none has (see
  // adopt_unfinished_depth_best).
  PVLine unfinished_best_pv;

  // Per-thread node counter for the live-progress getter. Plain uint64
  // (no atomics on the writer side); flushed periodically to the
  // shared atomic via the deadline-check rhythm. The reader sums per-
  // thread atomics, so a brief lag (up to DEPTH_DEADLINE_CHECK_INTERVAL
  // nodes) is acceptable.
  uint64_t local_nodes_searched;
  _Atomic uint64_t published_nodes_searched;

  // Per-thread "current line being explored" for the live-progress
  // getter. current_line[i] is the tiny_move played at the i-th ply
  // from the root; current_line_len is the number of valid entries.
  // Worker writes the move slot, then atomic_store_release the length;
  // reader does atomic_load_acquire on the length, then reads the line.
  // The slots are relaxed atomics so concurrent reader/writer access is a
  // well-defined relaxed access, not a C data race: the reader may still
  // observe a slot the writer is updating (the value is "stale", not torn
  // or UB), but the result stays structurally consistent (length matches
  // what has been published).
  _Atomic uint64_t current_line[MAX_SEARCH_DEPTH];
  _Atomic int current_line_len;

  // Live best-PV-from-root snapshot. Updated by abdada_negamax every
  // time best_value improves at the root (is_root=true), so a polling
  // reader can watch the engine's evolving best line refine through
  // each depth.  Seqlock pattern: live_pv_seq is even when the
  // snapshot is consistent, odd while a write is in progress. Reader
  // loads seq, reads length+moves+value, loads seq again; retries on
  // odd or mismatched seq. The data slots are relaxed atomics (accessed
  // under the seq's release/acquire fences) so the lock-free protocol is
  // free of C data races.
  _Atomic uint64_t live_pv_tiny_moves[MAX_SEARCH_DEPTH];
  _Atomic int32_t live_pv_value;
  _Atomic int live_pv_length;
  _Atomic uint64_t live_pv_seq;

  // Live multi-PV top-K snapshot, updated per root completion (so
  // every per_root_move_callback firing also republishes here).
  // Each slot has (root tiny_move, spread-adjusted value, the
  // continuation tiny_moves from after the root move, and the
  // continuation length).  Entries are sorted descending by value.
  // live_top_k_filled is how many of MAX_ENDGAME_DISPLAY_PVS slots
  // are currently populated. Same seqlock pattern as live_pv; the data
  // slots are relaxed atomics so the protocol is C-data-race-free.
  // Reset to 0 entries at the start of each new IDS depth so the
  // leaderboard reflects the current depth's evaluations only.
  _Atomic uint64_t live_top_k_root_tiny[MAX_ENDGAME_DISPLAY_PVS];
  _Atomic int32_t live_top_k_value[MAX_ENDGAME_DISPLAY_PVS];
  _Atomic uint64_t live_top_k_continuation[MAX_ENDGAME_DISPLAY_PVS]
                                          [MAX_SEARCH_DEPTH];
  _Atomic int live_top_k_continuation_len[MAX_ENDGAME_DISPLAY_PVS];
  _Atomic int live_top_k_filled;
  _Atomic uint64_t live_top_k_seq;
};

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// These consumers need root values beyond the best move and must share the
// same search-mode decision in root PVS and outer aspiration setup.
static bool solver_needs_exact_root_values(const EndgameCtx *solver) {
  return solver->num_top_moves > 1 || solver->actual_move != NULL ||
         solver->per_ply_callback != NULL ||
         solver->per_root_move_callback != NULL;
}

// Whether a movegen budget (-movebudget) can stop this solve: a limit of 0 is
// never spent.
static bool solver_has_movegen_budget(const EndgameCtx *solver) {
  return solver->movegen_budget != NULL && solver->movegen_budget->limit > 0;
}

// A solve with a real target depth, no clock and no movegen budget only needs
// the target: iterations of the other parity are skipped (see
// iterative_deepening_skips_ply). A solve asked for MAX_SEARCH_DEPTH is "as
// deep as it can get", and one under a time limit or a movegen budget falls
// back to its last completed depth, so all of them keep every iteration. So do
// actual-move checks, per-root-move callbacks and the first-win and
// initial-window modes. A per-ply callback does not: it is told about each
// depth searched, 1, 2, 4 and 6 for six plies. Nor does a multi-PV solve,
// whose ranked values all come from the target depth's search.
static bool solver_steps_depth_by_two(const EndgameCtx *solver, int plies) {
  return plies < MAX_SEARCH_DEPTH && solver->soft_time_limit <= 0.0 &&
         solver->hard_time_limit <= 0.0 && !solver_has_movegen_budget(solver) &&
         solver->actual_move == NULL &&
         solver->per_root_move_callback == NULL && !solver->first_win_optim &&
         !solver->initial_window_optim;
}

// Iterations cost in pairs: depth 2k+1 and 2k+2 search about the same number of
// nodes, and each pair costs roughly ten times the one before. The iteration
// that does not share the target's parity mostly seeds the transposition table
// for the next one, so stepping by two from the target is cheaper overall and
// returns the same value. Depth 1 is always searched; it is nearly free and
// orders the root moves.
static bool iterative_deepening_skips_ply(int ply, int plies) {
  return ply > 1 && ply < plies && ((plies - ply) & 1) != 0;
}

// Insert a value into a sorted (descending) top-K array.
// Returns the Kth-best value (or -LARGE_VALUE if fewer than K values stored).
static inline int32_t topk_insert(int32_t *topk, int *n, int k, int32_t val) {
  // Find insertion point (descending order)
  int pos = *n;
  for (int i = 0; i < *n; i++) {
    if (val > topk[i]) {
      pos = i;
      break;
    }
  }
  // Shift elements down if room
  if (*n < k) {
    for (int i = *n; i > pos; i--) {
      topk[i] = topk[i - 1];
    }
    topk[pos] = val;
    (*n)++;
  } else if (pos < k) {
    for (int i = k - 1; i > pos; i--) {
      topk[i] = topk[i - 1];
    }
    topk[pos] = val;
  }
  // Return the Kth-best (last element) or -LARGE_VALUE if not enough values
  if (*n >= k) {
    return topk[k - 1];
  }
  return -(1 << 30); // -LARGE_VALUE
}

static void pvline_clear(PVLine *pv_line) {
  pv_line->num_moves = 0;
  pv_line->score = 0;
  pv_line->negamax_depth = 0;
}

static void pvline_update(PVLine *pv_line, const PVLine *new_pv_line,
                          const SmallMove *move, int32_t score) {
  pvline_clear(pv_line);
  pv_line->moves[0].metadata = move->metadata;
  pv_line->moves[0].tiny_move = move->tiny_move;
  int child_moves = new_pv_line->num_moves;
  if (child_moves > MAX_VARIANT_LENGTH - 1) {
    child_moves = MAX_VARIANT_LENGTH - 1;
  }
  for (int i = 0; i < child_moves; i++) {
    pv_line->moves[i + 1].metadata = new_pv_line->moves[i].metadata;
    pv_line->moves[i + 1].tiny_move = new_pv_line->moves[i].tiny_move;
  }
  pv_line->num_moves = child_moves + 1;
  pv_line->score = score;
  // negamax_depth is not updated here; callers set it explicitly.
}

// Greedy playout for display: extend PV with highest-scoring moves after
// TT extension. Returns number of moves appended.
static int greedy_playout_for_display(PVLine *pv_line, int start_idx,
                                      Game *game_copy, MoveList *move_list) {
  int num_moves = start_idx;
  while (num_moves < MAX_VARIANT_LENGTH &&
         game_get_game_end_reason(game_copy) == GAME_END_REASON_NONE) {
    const MoveGenArgs greedy_args = {
        .game = game_copy,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL_SMALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&greedy_args);
    int nplays = move_list->count;
    if (nplays == 0) {
      break;
    }

    // Check for consecutive passes ending the game
    if (nplays == 1 && small_move_is_pass(move_list->small_moves[0]) &&
        game_get_consecutive_scoreless_turns(game_copy) >= 1) {
      break;
    }

    // Pick highest-scoring move
    int best_idx = 0;
    int best_score = INT32_MIN;
    for (int j = 0; j < nplays; j++) {
      int sc = small_move_get_score(move_list->small_moves[j]);
      if (sc > best_score) {
        best_score = sc;
        best_idx = j;
      }
    }

    SmallMove best_sm = *move_list->small_moves[best_idx];
    small_move_to_move(move_list->spare_move, &best_sm,
                       game_get_board(game_copy));
    play_move(move_list->spare_move, game_copy, NULL);

    pv_line->moves[num_moves] = best_sm;
    num_moves++;
  }
  return num_moves - start_idx;
}

// Extend the PV by probing the transposition table for moves beyond
// what the search PV already contains. Existing PV moves (which have
// correct scores) are preserved; only new moves are appended.
void pvline_extend_from_tt(PVLine *pv_line, Game *game_copy,
                           TranspositionTable *tt, int solving_player,
                           int max_depth) {
  if (!tt) {
    return;
  }

  MoveList *move_list =
      move_list_create_small(ENDGAME_MOVELIST_INITIAL_CAPACITY);

  // Play existing PV moves to advance game state to where extension begins.
  // These moves already have correct scores in their metadata.
  for (int i = 0; i < pv_line->num_moves; i++) {
    if (game_get_game_end_reason(game_copy) != GAME_END_REASON_NONE) {
      small_move_list_destroy(move_list);
      return;
    }
    small_move_to_move(move_list->spare_move, &pv_line->moves[i],
                       game_get_board(game_copy));
    play_move(move_list->spare_move, game_copy, NULL);
  }

  // Extend PV from TT
  int num_moves = pv_line->num_moves;

  while (num_moves < max_depth &&
         game_get_game_end_reason(game_copy) == GAME_END_REASON_NONE) {
    int on_turn = game_get_player_on_turn_index(game_copy);
    const Player *solving = game_get_player(game_copy, solving_player);
    const Player *other = game_get_player(game_copy, 1 - solving_player);

    uint64_t hash = zobrist_calculate_hash(
        tt->zobrist, game_get_board(game_copy), player_get_rack(solving),
        player_get_rack(other), on_turn != solving_player,
        game_get_consecutive_scoreless_turns(game_copy));

    TTEntry tt_entry = transposition_table_lookup(tt, hash);
    if (!ttentry_valid(tt_entry)) {
      break;
    }

    uint64_t tiny_move = ttentry_move(tt_entry);
    if (tiny_move == INVALID_TINY_MOVE) {
      break;
    }

    SmallMove sm = {0};
    sm.tiny_move = tiny_move;
    uint16_t move_score = 0;
    if (small_move_is_pass(&sm)) {
      move_list_set_spare_move_as_pass(move_list);
    } else {
      small_move_to_move(move_list->spare_move, &sm, game_get_board(game_copy));
      const bool kwgs_shared =
          game_get_data_is_shared(game_copy, PLAYERS_DATA_TYPE_KWG);
      const int on_turn_idx = game_get_player_on_turn_index(game_copy);
      const Equity score_equity = static_eval_get_move_score(
          game_get_ld(game_copy), move_list->spare_move,
          game_get_board(game_copy), game_get_bingo_bonus(game_copy),
          board_get_cross_set_index(kwgs_shared, on_turn_idx));
      move_score = (uint16_t)equity_to_int(score_equity);
      move_list->spare_move->score = score_equity;
    }
    play_move(move_list->spare_move, game_copy, NULL);

    pv_line->moves[num_moves].tiny_move = tiny_move;
    memset(&pv_line->moves[num_moves].metadata, 0,
           sizeof(pv_line->moves[num_moves].metadata));
    pv_line->moves[num_moves].metadata.score = move_score;
    num_moves++;
  }

  // Preserve original negamax_depth from the search. TT-extended moves
  // are from cached entries, not the current depth's negamax search.
  // The | separator in PV display marks the search depth boundary.

  // Greedy playout: if game isn't over, extend PV with highest-scoring moves.
  // This handles cases where the search PV was truncated (e.g., parallel search
  // effects) and TT entries were overwritten.
  num_moves +=
      greedy_playout_for_display(pv_line, num_moves, game_copy, move_list);

  pv_line->num_moves = num_moves;
  small_move_list_destroy(move_list);
}

// TT depth stored for entries whose proof contains no heuristic leaf; it
// satisfies every depth check, and 63 is the largest the entry can hold.
enum { TT_PURE_DEPTH = 63 };

static bool iterative_deepening_should_stop(EndgameCtx *solver);
static bool check_depth_deadline(EndgameCtxWorker *worker);

// Returns the pruned KWG for the given player index.
// In shared-KWG mode, only pruned_kwgs[0] exists, so it is always returned.
// In non-shared mode, each player index maps to its own pruned KWG.
// When skip_word_pruning was used (pruned_kwgs are NULL), falls back to the
// game's effective KWG (which respects any override KWGs set by the caller).
static inline const KWG *solver_get_pruned_kwg(const EndgameCtx *solver,
                                               int player_index) {
  if (solver->pruned_kwgs[0] == NULL) {
    return game_get_effective_kwg(solver->game, player_index);
  }
  if (solver->pruned_kwgs[1] == NULL) {
    return solver->pruned_kwgs[0];
  }
  return solver->pruned_kwgs[player_index];
}

// The rack tile types a small move places (a blank-designated tile counts as
// the blank), as a MOVE_RECORD_TILES_PLAYED generation records them.
static inline uint64_t small_move_tile_types_bv(const SmallMove *small_move) {
  uint64_t tile_types = 0;
  for (int tile_idx = 0; tile_idx < SMALL_MOVE_MAX_TILES; tile_idx++) {
    const MachineLetter tile = small_move_get_tile(small_move, tile_idx);
    if (tile == 0) {
      break;
    }
    const MachineLetter ml = small_move_tile_is_blank(small_move, tile_idx)
                                 ? BLANK_MACHINE_LETTER
                                 : tile;
    tile_types |= (uint64_t)1 << ml;
  }
  return tile_types;
}

static void board_occupancy_init(const Board *board, BoardOccupancy *occ) {
  for (int row = 0; row < BOARD_DIM; row++) {
    occ->occupied_rows[row] = 0;
    occ->unavailable_rows[row] = 0;
    for (int col = 0; col < BOARD_DIM; col++) {
      const uint32_t square_bit = UINT32_C(1) << col;
      if (!board_is_empty(board, row, col)) {
        occ->occupied_rows[row] |= square_bit;
      }
      if (board_is_nonempty_or_bricked(board, row, col)) {
        occ->unavailable_rows[row] |= square_bit;
      }
    }
  }
}

// The empty squares of row next to a tile, across (bits of *across) or down
// (bits of *down); returns their union.
static inline uint32_t board_occupancy_candidates(const BoardOccupancy *occ,
                                                  int row, uint32_t *across,
                                                  uint32_t *down) {
  const uint32_t row_mask = (UINT32_C(1) << BOARD_DIM) - 1;
  *across = (occ->occupied_rows[row] << 1) | (occ->occupied_rows[row] >> 1);
  *down = (row > 0 ? occ->occupied_rows[row - 1] : 0) |
          (row < BOARD_DIM - 1 ? occ->occupied_rows[row + 1] : 0);
  return (*across | *down) & ~occ->unavailable_rows[row] & row_mask;
}

// board_get_playable_tiles_bv over the squares occ lists as next to a tile,
// in the same order, so with the same early exit and the same result.
static uint64_t occupancy_playable_tiles_bv(const Board *board,
                                            const BoardOccupancy *occ,
                                            int cross_set_index,
                                            uint64_t rack_tiles_bv) {
  uint64_t playable = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    uint32_t across;
    uint32_t down;
    uint32_t candidates = board_occupancy_candidates(occ, row, &across, &down);
    while (candidates) {
      const int col = __builtin_ctz(candidates);
      candidates &= candidates - 1;
      playable |=
          board_get_cross_set(board, row, col, BOARD_HORIZONTAL_DIRECTION,
                              cross_set_index) &
          board_get_cross_set(board, row, col, BOARD_VERTICAL_DIRECTION,
                              cross_set_index);
      if (rack_tiles_bv && (playable & rack_tiles_bv) == rack_tiles_bv) {
        return playable;
      }
    }
  }
  return playable;
}

// Generate opponent's moves in TILES_PLAYED mode and return stuck-tile
// fraction. Saves and restores player-on-turn if it differs from opp_idx.
// If tiles_played_bv_out is non-NULL, writes the bitvector of tile types
// that appear in at least one valid move.
// solver is nullable; when non-NULL an interrupt check is performed before
// the expensive generate_moves call so a fired interrupt cuts the work short;
// the result is then meaningless, and callers detect the interrupt themselves
// after this returns.
// `opp_plays`, when non-NULL, is the opponent's complete list of plays on
// this board (`opp_play_count` entries; a trailing pass is harmless), which
// the caller already has. Its tile types then answer exactly, without the
// MOVE_RECORD_TILES_PLAYED generation, so no interrupt check is needed.
// `hint`, when non-NULL, describes the position before the last move. When
// every tile type the cross-set scan leaves undecided had no play there, the
// generation covers only the anchors whose plays can reach a square the last
// move changed.
static float compute_opp_stuck_fraction(
    Game *game, MoveList *move_list, const KWG *pruned_kwg, int opp_idx,
    uint64_t *tiles_played_bv_out, EndgameCtx *solver,
    const SmallMove *opp_plays, int opp_play_count,
    const BoardOccupancy *occupancy, StuckTilesEntry *cache, uint64_t cache_key,
    const StuckTilesHint *hint) {
  int saved_on_turn = game_get_player_on_turn_index(game);
  StuckTilesEntry *cache_entry =
      cache != NULL && cache_key != 0
          ? &cache[cache_key & (STUCK_TILES_CACHE_SIZE - 1)]
          : NULL;
  const Rack *opp_rack = player_get_rack(game_get_player(game, opp_idx));
  // The tile types are those of the rack that appear in at least one play,
  // however they are found, so a cached answer stands for every path below.
  if (cache_entry != NULL && cache_entry->key == cache_key) {
    if (tiles_played_bv_out) {
      *tiles_played_bv_out = cache_entry->tiles_bv;
    }
    return stuck_tile_fraction_from_bv(game_get_ld(game), opp_rack,
                                       cache_entry->tiles_bv);
  }
  if (saved_on_turn != opp_idx) {
    game_set_player_on_turn_index(game, opp_idx);
  }
  // Cross-set scan: scan the board to find which rack tiles have valid
  // single-tile plays. If all tiles are playable, skip movegen entirely.
  // For 1-tile racks the result is authoritative. For multi-tile partial
  // results, fall through to movegen with opp_tiles_bv pre-seeded.
  uint64_t opp_tiles_bv = 0;
  const Board *board = game_get_board(game);
  const bool cross_sets_valid = board_get_cross_sets_valid(board);
  uint64_t rack_tiles_bv = 0;
  if (cross_sets_valid) {
    bool kwgs_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
    int ci = board_get_cross_set_index(kwgs_shared, opp_idx);
    const LetterDistribution *ld = game_get_ld(game);
    int ld_size = ld_get_size(ld);
    for (int ml = 0; ml < ld_size; ml++) {
      if (rack_get_letter(opp_rack, ml) > 0) {
        rack_tiles_bv |= ((uint64_t)1 << ml);
      }
    }
    uint64_t rack_non_blank = rack_tiles_bv & ~(uint64_t)1;
    uint64_t playable_bv =
        occupancy != NULL
            ? occupancy_playable_tiles_bv(board, occupancy, ci, rack_non_blank)
            : board_get_playable_tiles_bv(board, ci, rack_non_blank);
    opp_tiles_bv = playable_bv & rack_tiles_bv;
    if ((rack_tiles_bv & 1) && (playable_bv >> 1)) {
      opp_tiles_bv |= 1;
    }
    bool all_playable = (opp_tiles_bv == rack_tiles_bv);
    if (all_playable || rack_get_total_letters(opp_rack) == 1) {
      if (cache_entry != NULL) {
        cache_entry->key = cache_key;
        cache_entry->tiles_bv = opp_tiles_bv;
      }
      float frac = all_playable ? 0.0F
                                : stuck_tile_fraction_from_bv(ld, opp_rack,
                                                              opp_tiles_bv);
      if (saved_on_turn != opp_idx) {
        game_set_player_on_turn_index(game, saved_on_turn);
      }
      if (tiles_played_bv_out) {
        *tiles_played_bv_out = opp_tiles_bv;
      }
      return frac;
    }
    // Multi-tile partial result: not authoritative. Fall through to movegen
    // with opp_tiles_bv pre-seeded so movegen skips re-discovering the
    // already-known playable tiles.
  }
  if (opp_plays != NULL) {
    for (int move_idx = 0; move_idx < opp_play_count; move_idx++) {
      opp_tiles_bv |= small_move_tile_types_bv(&opp_plays[move_idx]);
    }
    if (cache_entry != NULL) {
      cache_entry->key = cache_key;
      cache_entry->tiles_bv = opp_tiles_bv;
    }
    const float derived_result =
        stuck_tile_fraction_from_bv(game_get_ld(game), opp_rack, opp_tiles_bv);
    if (saved_on_turn != opp_idx) {
      game_set_player_on_turn_index(game, saved_on_turn);
    }
    if (tiles_played_bv_out) {
      *tiles_played_bv_out = opp_tiles_bv;
    }
    return derived_result;
  }
  // Check for interrupt before the expensive movegen call.  If already fired,
  // restore game state and return early — the caller will re-detect the
  // interrupt immediately and discard this 0.0F result.
  if (solver && iterative_deepening_should_stop(solver)) {
    if (saved_on_turn != opp_idx) {
      game_set_player_on_turn_index(game, saved_on_turn);
    }
    if (tiles_played_bv_out) {
      *tiles_played_bv_out = opp_tiles_bv;
    }
    return 0.0F;
  }
  const MoveGenArgs tp_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_TILES_PLAYED,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = pruned_kwg,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .tiles_played_bv = &opp_tiles_bv,
      .initial_tiles_bv = opp_tiles_bv,
  };
  // The scan settles every single-tile play exactly, so an undecided tile type
  // can only be in a play of two or more tiles, which is recorded in its own
  // lane: if it had no play before the last move, any it has now spans a
  // square that move changed.
  if (hint != NULL && cross_sets_valid &&
      (rack_tiles_bv & ~opp_tiles_bv & hint->playable_before) == 0) {
    generate_tiles_played_in_lanes(&tp_args, hint->lanes, hint->lane_hits);
  } else {
    generate_moves(&tp_args);
  }
  if (cache_entry != NULL) {
    cache_entry->key = cache_key;
    cache_entry->tiles_bv = opp_tiles_bv;
  }
  float movegen_result =
      stuck_tile_fraction_from_bv(game_get_ld(game), opp_rack, opp_tiles_bv);
  if (saved_on_turn != opp_idx) {
    game_set_player_on_turn_index(game, saved_on_turn);
  }
  if (tiles_played_bv_out) {
    *tiles_played_bv_out = opp_tiles_bv;
  }
  return movegen_result;
}

// Compute the initial stuck-tile fraction for the opponent at the root.
// Duplicates the game to avoid modifying the original.
static float compute_initial_stuck_fraction(const EndgameCtx *solver,
                                            const Game *game) {
  int opp_idx = 1 - solver->solving_player;
  Game *root_game = game_duplicate(game);
  MoveList *tmp_ml = move_list_create_small(ENDGAME_MOVELIST_INITIAL_CAPACITY);
  float frac = compute_opp_stuck_fraction(
      root_game, tmp_ml, solver_get_pruned_kwg(solver, opp_idx), opp_idx, NULL,
      NULL, NULL, 0, NULL, NULL, 0, NULL);
  small_move_list_destroy(tmp_ml);
  game_destroy(root_game);
  return frac;
}

void endgame_ctx_reset(EndgameCtx *es, EndgameResults *results,
                       const EndgameArgs *endgame_args) {
  es->first_win_optim = endgame_args->first_win;
  es->first_win_fallback_moves = endgame_args->first_win_fallback_moves;
  // first_win and an initial window are mutually exclusive: iterative_deepening
  // treats first_win as a fixed [-1, 1] window and would silently override a
  // caller's initial window. Make the precedence explicit here (first_win wins)
  // so every downstream flag check sees a consistent state.
  es->initial_window_optim =
      endgame_args->use_initial_window && !endgame_args->first_win;
  es->initial_window_alpha = endgame_args->initial_alpha;
  es->initial_window_beta = endgame_args->initial_beta;
  es->transposition_table_optim = true;
  es->iterative_deepening_optim = true;
  es->negascout_optim = true;
  es->use_heuristics = endgame_args->use_heuristics;
  es->forced_pass_bypass = endgame_args->forced_pass_bypass;
  es->incremental_movegen = endgame_args->incremental_movegen;
  es->best_reply_shortcuts = !endgame_args->disable_best_reply_shortcuts;
  es->num_top_moves = endgame_args->num_top_moves;
  // The in-search top-K array (topk_values) and the live multi-PV leaderboard
  // are sized to MAX_ENDGAME_DISPLAY_PVS; clamp so a caller asking for more
  // displayed root moves can't overrun them.
  if (es->num_top_moves > MAX_ENDGAME_DISPLAY_PVS) {
    es->num_top_moves = MAX_ENDGAME_DISPLAY_PVS;
  }
  es->threads = endgame_args->num_threads;
  if (es->threads < 1) {
    es->threads = 1;
  }
  // Normalize the injection ceiling so later logic has one interpretation:
  // clamp to MAX_THREADS (injected workers draw MoveGen slots from a pool
  // capped there, and the worker arrays are sized to it), and treat any
  // ceiling <= threads as "no growth" (0).
  es->max_workers = endgame_args->max_workers;
  if (es->max_workers > MAX_THREADS) {
    es->max_workers = MAX_THREADS;
  }
  if (es->max_workers <= es->threads) {
    es->max_workers = 0;
  }
  // Injection window starts shut: endgame_solve opens it once the initial
  // workers are spawned, and shuts it again before the join. live_workers
  // counts the initial set until adds bump it. Clear window_open_ns so a
  // monitor can't read a stale timestamp from a previous solve.
  atomic_store(&es->adding_closed, 1);
  atomic_store(&es->live_workers, es->threads);
  atomic_store(&es->window_open_ns, (int64_t)0);
  es->requested_plies = endgame_args->plies;
  es->solving_player = game_get_player_on_turn_index(endgame_args->game);
  es->initial_small_move_arena_size =
      endgame_args->initial_small_move_arena_size;
  const Player *player =
      game_get_player(endgame_args->game, es->solving_player);
  const Player *opponent =
      game_get_player(endgame_args->game, 1 - es->solving_player);

  es->initial_spread =
      equity_to_int(player_get_score(player) - player_get_score(opponent));
  const LetterDistribution *ld = game_get_ld(endgame_args->game);
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    es->tile_face_values[ml] = equity_to_int(ld_get_score(ld, ml));
  }

  kwg_destroy(es->pruned_kwgs[0]);
  kwg_destroy(es->pruned_kwgs[1]);
  es->pruned_kwgs[0] = NULL;
  es->pruned_kwgs[1] = NULL;

  es->dual_lexicon_mode = endgame_args->dual_lexicon_mode;
  // INFORMED mode with shared KWGs is meaningless (both players have the same
  // lexicon). Coerce to IGNORANT to avoid building only pruned_kwgs[0] while
  // get_kwg_for_cross_set would try to dereference pruned_kwgs[1] as NULL.
  bool shared_kwg =
      game_get_data_is_shared(endgame_args->game, PLAYERS_DATA_TYPE_KWG);
  if (es->dual_lexicon_mode == DUAL_LEXICON_MODE_INFORMED && shared_kwg) {
    es->dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT;
  }
  es->soft_time_limit = endgame_args->soft_time_limit;
  es->hard_time_limit = endgame_args->hard_time_limit;
  es->external_deadline_ns = endgame_args->external_deadline_ns;
  bool create_separate_kwgs =
      (es->dual_lexicon_mode == DUAL_LEXICON_MODE_INFORMED) && !shared_kwg;

  // skip_word_pruning: leave pruned_kwgs NULL so move gen uses the full KWG
  // (or any override KWGs the caller set on the game). Used by PEG which
  // builds its own pruned KWGs once at the root position.
  if (!endgame_args->skip_word_pruning) {
    // Generate pruned KWG(s) from the set of possible words on this board.
    // In IGNORANT mode (or shared-KWG), one pruned KWG is used for everything.
    // In INFORMED mode with different lexicons, each player index gets its own
    // pruned KWG so that cross-set index i uses the pruned KWG derived from
    // player i's lexicon.
    for (int player_idx = 0; player_idx < (create_separate_kwgs ? 2 : 1);
         player_idx++) {
      const KWG *full_kwg =
          player_get_kwg(game_get_player(endgame_args->game, player_idx));
      DictionaryWordList *word_list = dictionary_word_list_create();
      // Cross-check-aware pruning is only sound for classic play with a
      // shared lexicon; see generate_possible_words_with_cross_checks.
      if (game_get_variant(endgame_args->game) == GAME_VARIANT_CLASSIC &&
          shared_kwg) {
        generate_possible_words_with_cross_checks(endgame_args->game, full_kwg,
                                                  word_list);
      } else {
        generate_possible_words(endgame_args->game, full_kwg, word_list);
      }
      es->pruned_kwgs[player_idx] = make_kwg_from_words_small(
          word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
      dictionary_word_list_destroy(word_list);
    }
  }

  atomic_store(&es->root_sweep_count, -1);
  atomic_store(&es->root_sweep_next, 0);

  // Initialize ABDADA synchronization
  atomic_store(&es->search_complete, 0);
  atomic_store(&es->depth_deadline_ns, 0);
  atomic_store(&es->stuck_tile_logged, 0);
  atomic_store(&es->root_moves_completed, 0);
  atomic_store(&es->root_moves_total, 0);
  atomic_store(&es->current_depth, 0);
  atomic_store(&es->ply2_moves_completed, 0);
  atomic_store(&es->ply2_moves_total, 0);

  es->thread_control = endgame_args->thread_control;
  es->game = endgame_args->game;
  es->actual_move = endgame_args->actual_move;
  es->per_ply_callback = endgame_args->per_ply_callback;
  es->per_ply_callback_data = endgame_args->per_ply_callback_data;
  es->before_search_callback = endgame_args->before_search_callback;
  es->before_search_callback_data = endgame_args->before_search_callback_data;
  es->per_root_move_callback = endgame_args->per_root_move_callback;
  es->per_root_move_callback_data = endgame_args->per_root_move_callback_data;
  if (endgame_args->shared_tt) {
    // Use caller-provided TT. Free any previously owned TT.
    if (!es->tt_is_external) {
      transposition_table_destroy(es->transposition_table);
    }
    es->transposition_table = endgame_args->shared_tt;
    es->tt_fraction_of_mem = 0;
    es->tt_is_external = true;
  } else {
    if (!es->tt_is_external) {
      if (endgame_args->tt_fraction_of_mem == 0) {
        transposition_table_destroy(es->transposition_table);
        es->transposition_table = NULL;
      } else if (es->tt_fraction_of_mem != endgame_args->tt_fraction_of_mem) {
        transposition_table_destroy(es->transposition_table);
        es->transposition_table =
            transposition_table_create(endgame_args->tt_fraction_of_mem);
      }
    } else {
      // Transitioning from external to owned. Don't destroy the external TT.
      es->transposition_table = NULL;
      if (endgame_args->tt_fraction_of_mem > 0) {
        es->transposition_table =
            transposition_table_create(endgame_args->tt_fraction_of_mem);
      }
    }
    es->tt_fraction_of_mem = endgame_args->tt_fraction_of_mem;
    es->tt_is_external = false;
  }
  // Disable TT optimization when no TT is available.
  if (es->transposition_table == NULL) {
    es->transposition_table_optim = false;
  }
  es->results = results;
  if (es->results) {
    endgame_results_lock(es->results, ENDGAME_RESULT_DISPLAY);
    endgame_results_reset(es->results);
    endgame_results_set_start_game(es->results, endgame_args->game);
    endgame_results_set_pvline_extend_args(es->results, es->transposition_table,
                                           es->solving_player,
                                           es->requested_plies);
    endgame_results_set_valid_for_current_game_state(es->results, true);
    endgame_results_unlock(es->results, ENDGAME_RESULT_DISPLAY);
  }
  // Compute initial stuck-tile fraction for root move ordering.
  // Per-node detection in abdada_negamax recomputes this dynamically.
  es->initial_opp_stuck_frac = 0.0F;
  if (es->use_heuristics) {
    es->initial_opp_stuck_frac =
        compute_initial_stuck_fraction(es, endgame_args->game);
  }
}

EndgameCtx *endgame_ctx_create(void) {
  EndgameCtx *solver = calloc_or_die(1, sizeof(EndgameCtx));
  cpthread_mutex_init(&solver->add_mutex);
  // Closed until a solve opens the window; otherwise a never-solved ctx (calloc
  // zeroes adding_closed) would look injectable to an external monitor.
  atomic_store(&solver->adding_closed, 1);
  return solver;
}

const TranspositionTable *
endgame_ctx_get_transposition_table(const EndgameCtx *ctx) {
  return ctx->transposition_table;
}

void endgame_ctx_get_progress(const EndgameCtx *ctx, int *current_depth,
                              int *root_moves_completed, int *root_moves_total,
                              int *ply2_moves_completed,
                              int *ply2_moves_total) {
  *current_depth = atomic_load(&ctx->current_depth);
  *root_moves_completed = atomic_load(&ctx->root_moves_completed);
  *root_moves_total = atomic_load(&ctx->root_moves_total);
  *ply2_moves_completed = atomic_load(&ctx->ply2_moves_completed);
  *ply2_moves_total = atomic_load(&ctx->ply2_moves_total);
}

void endgame_ctx_clear_transposition_table(EndgameCtx *ctx) {
  if (ctx->transposition_table != NULL) {
    transposition_table_reset(ctx->transposition_table);
  }
}

// Number of workers valid to observe in the current solve: live_workers
// (initial + injected) clamped to cap_workers. The clamp guards the brief
// window at solve start before prepare_workers has created worker structs up
// to live_workers. Workers at indices >= this count are not part of the
// current solve and hold stale live-view state left over from a previous one,
// so the accessors below must not read them.
static int endgame_active_worker_count(const EndgameCtx *ctx) {
  int active = atomic_load_explicit(&ctx->live_workers, memory_order_acquire);
  const int cap = atomic_load_explicit(&ctx->cap_workers, memory_order_acquire);
  if (active > cap) {
    active = cap;
  }
  if (active < 0) {
    active = 0;
  }
  return active;
}

uint64_t endgame_ctx_get_nodes_searched(const EndgameCtx *ctx) {
  uint64_t total = 0;
  const int active = endgame_active_worker_count(ctx);
  for (int worker_idx = 0; worker_idx < active; worker_idx++) {
    total += atomic_load_explicit(
        &ctx->workers[worker_idx]->published_nodes_searched,
        memory_order_relaxed);
  }
  return total;
}

int endgame_ctx_get_current_line(const EndgameCtx *ctx, int worker_index,
                                 uint64_t *out_line, int max_len) {
  if (max_len <= 0 || worker_index < 0 ||
      worker_index >= endgame_active_worker_count(ctx)) {
    return 0;
  }
  const EndgameCtxWorker *worker = ctx->workers[worker_index];
  // Acquire on length pairs with release on writer side: by the time we
  // observe length L, all writes to current_line[0..L-1] up to that
  // store are visible. The worker may have moved on since then; entries
  // beyond what we read may correspond to a different sibling subtree.
  int len =
      atomic_load_explicit(&worker->current_line_len, memory_order_acquire);
  if (len > max_len) {
    len = max_len;
  }
  if (len > MAX_SEARCH_DEPTH) {
    len = MAX_SEARCH_DEPTH;
  }
  for (int ply_idx = 0; ply_idx < len; ply_idx++) {
    out_line[ply_idx] = atomic_load_explicit(&worker->current_line[ply_idx],
                                             memory_order_relaxed);
  }
  return len;
}

int endgame_ctx_get_live_pv(const EndgameCtx *ctx, int worker_index,
                            uint64_t *out_moves, int max_len,
                            int32_t *out_value) {
  if (max_len <= 0 || worker_index < 0 ||
      worker_index >= endgame_active_worker_count(ctx)) {
    *out_value = 0;
    return 0;
  }
  const EndgameCtxWorker *worker = ctx->workers[worker_index];
  // Seqlock read: try a few times to capture a consistent snapshot
  // (length, moves, value all from the same writer update).
  for (int attempt = 0; attempt < 4; attempt++) {
    const uint64_t seq1 =
        atomic_load_explicit(&worker->live_pv_seq, memory_order_acquire);
    if ((seq1 & 1ULL) != 0) {
      continue; // writer in progress
    }
    int len =
        atomic_load_explicit(&worker->live_pv_length, memory_order_relaxed);
    const int32_t value =
        atomic_load_explicit(&worker->live_pv_value, memory_order_relaxed);
    if (len > max_len) {
      len = max_len;
    }
    if (len > MAX_SEARCH_DEPTH) {
      len = MAX_SEARCH_DEPTH;
    }
    for (int ply_idx = 0; ply_idx < len; ply_idx++) {
      out_moves[ply_idx] = atomic_load_explicit(
          &worker->live_pv_tiny_moves[ply_idx], memory_order_relaxed);
    }
    const uint64_t seq2 =
        atomic_load_explicit(&worker->live_pv_seq, memory_order_acquire);
    if (seq1 == seq2) {
      *out_value = value;
      return len;
    }
  }
  *out_value = 0;
  return 0;
}

int endgame_ctx_get_live_top_k_pvs(const EndgameCtx *ctx, int worker_index,
                                   EndgameLivePvSnapshot *out, int max_k) {
  if (max_k <= 0 || worker_index < 0 ||
      worker_index >= endgame_active_worker_count(ctx)) {
    return 0;
  }
  const EndgameCtxWorker *worker = ctx->workers[worker_index];
  for (int attempt = 0; attempt < 4; attempt++) {
    const uint64_t seq1 =
        atomic_load_explicit(&worker->live_top_k_seq, memory_order_acquire);
    if ((seq1 & 1ULL) != 0) {
      continue; // writer in progress
    }
    int filled =
        atomic_load_explicit(&worker->live_top_k_filled, memory_order_relaxed);
    if (filled > max_k) {
      filled = max_k;
    }
    if (filled > MAX_ENDGAME_DISPLAY_PVS) {
      filled = MAX_ENDGAME_DISPLAY_PVS;
    }
    for (int pv_idx = 0; pv_idx < filled; pv_idx++) {
      out[pv_idx].root_tiny = atomic_load_explicit(
          &worker->live_top_k_root_tiny[pv_idx], memory_order_relaxed);
      out[pv_idx].value = atomic_load_explicit(
          &worker->live_top_k_value[pv_idx], memory_order_relaxed);
      int cont_len = atomic_load_explicit(
          &worker->live_top_k_continuation_len[pv_idx], memory_order_relaxed);
      if (cont_len > MAX_SEARCH_DEPTH) {
        cont_len = MAX_SEARCH_DEPTH;
      }
      out[pv_idx].continuation_len = cont_len;
      for (int ply_idx = 0; ply_idx < cont_len; ply_idx++) {
        out[pv_idx].continuation_tiny[ply_idx] = atomic_load_explicit(
            &worker->live_top_k_continuation[pv_idx][ply_idx],
            memory_order_relaxed);
      }
    }
    const uint64_t seq2 =
        atomic_load_explicit(&worker->live_top_k_seq, memory_order_acquire);
    if (seq1 == seq2) {
      return filled;
    }
  }
  return 0;
}

// Defined below; used by both the worker create and reset paths.
static void reset_worker_analysis_state(EndgameCtxWorker *worker);

// Point worker->path_lists at the worker's incremental move lists for a solve
// that enables them (allocating them on first use), or at NULL for one that
// does not, so every hook in the search reads one pointer to know.
static void configure_worker_path_lists(EndgameCtxWorker *worker,
                                        const EndgameCtx *solver) {
  if (!solver->incremental_movegen) {
    worker->path_lists = NULL;
    return;
  }
  if (worker->path_lists_storage == NULL) {
    worker->path_lists_storage = path_move_lists_create();
  }
  worker->path_lists = worker->path_lists_storage;
  path_move_lists_reset(worker->path_lists);
}

static void solver_worker_destroy(EndgameCtxWorker *solver_worker) {
  if (!solver_worker) {
    return;
  }
  game_destroy(solver_worker->game_copy);
  small_move_list_destroy(solver_worker->move_list);
  arena_destroy(solver_worker->small_move_arena);
  path_move_lists_destroy(solver_worker->path_lists_storage);
  free(solver_worker->sort_scratch);
  free(solver_worker->build_chain_scratch);
  free(solver_worker->stuck_tiles_cache);
  free(solver_worker->playout_tail_cache);
  lane_shadow_cache_destroy(solver_worker->lane_shadow_cache);
  cross_set_cache_destroy(solver_worker->cross_set_cache);
  prng_destroy(solver_worker->prng);
  free(solver_worker);
}

void endgame_ctx_destroy(EndgameCtx *ctx) {
  if (!ctx) {
    return;
  }
  for (int worker_idx = 0; worker_idx < atomic_load(&ctx->cap_workers);
       worker_idx++) {
    solver_worker_destroy(ctx->workers[worker_idx]);
  }
  free(ctx->workers);
  free(ctx->worker_ids);
  if (!ctx->tt_is_external) {
    transposition_table_destroy(ctx->transposition_table);
  }
  kwg_destroy(ctx->pruned_kwgs[0]);
  kwg_destroy(ctx->pruned_kwgs[1]);
  game_destroy(ctx->ext_game);
  free(ctx->root_sweep_moves);
  free(ctx);
}

// Create a new worker, duplicating the given game state.
// Called only when growing the worker pool.
static EndgameCtxWorker *endgame_ctx_create_worker(EndgameCtx *solver,
                                                   int ordinal,
                                                   uint64_t base_seed,
                                                   const Game *template_game) {
  EndgameCtxWorker *solver_worker = malloc_or_die(sizeof(EndgameCtxWorker));

  solver_worker->ordinal = ordinal;
  solver_worker->game_copy = game_duplicate(template_game);
  game_set_endgame_solving_mode(solver_worker->game_copy);

  solver_worker->move_list =
      move_list_create_small(ENDGAME_MOVELIST_INITIAL_CAPACITY);

  solver_worker->small_move_arena =
      create_arena(solver->initial_small_move_arena_size, 16);
  solver_worker->path_lists_storage = NULL;
  configure_worker_path_lists(solver_worker, solver);

  solver_worker->solver = solver;
  memset(solver_worker->move_undos, 0, sizeof(solver_worker->move_undos));
  solver_worker->num_played_moves = 0;
  solver_worker->first_deferred_move = 0;
  memset(solver_worker->occupancy_valid, 0,
         sizeof(solver_worker->occupancy_valid));
  solver_worker->sort_scratch = NULL;
  solver_worker->sort_scratch_capacity = 0;
  solver_worker->build_chain_scratch = NULL;
  solver_worker->build_chain_scratch_capacity = 0;

  // Seed by ordinal (stable across solves), not the MoveGen slot, so jitter is
  // deterministic regardless of which slots were borrowed.
  solver_worker->stuck_tiles_cache =
      calloc_or_die(STUCK_TILES_CACHE_SIZE, sizeof(StuckTilesEntry));
  solver_worker->stuck_tiles_salt = STUCK_TILES_SALT_STEP;
  solver_worker->playout_tail_cache =
      calloc_or_die(PLAYOUT_TAIL_CACHE_SIZE, sizeof(PlayoutTailEntry));
  solver_worker->lane_shadow_cache =
      lane_shadow_cache_create(ENDGAME_LANE_SHADOW_CACHE_INDEX_BITS);
  lane_shadow_cache_set_salt(solver_worker->lane_shadow_cache,
                             solver_worker->stuck_tiles_salt);
  solver_worker->cross_set_cache =
      cross_set_cache_create(CROSS_SET_CACHE_INDEX_BITS);
  cross_set_cache_set_salt(solver_worker->cross_set_cache,
                           solver_worker->stuck_tiles_salt);
  game_set_cross_set_cache(solver_worker->game_copy,
                           solver_worker->cross_set_cache);
  solver_worker->prng = prng_create(base_seed + (uint64_t)ordinal * 12345);

  solver_worker->best_pv.game = solver_worker->game_copy;
  solver_worker->best_pv.num_moves = 0;
  solver_worker->best_pv_value = -LARGE_VALUE;
  solver_worker->completed_depth = 0;
  solver_worker->nodes_since_deadline_check = 0;
  reset_worker_analysis_state(solver_worker);

  return solver_worker;
}

// Zero the per-worker live-progress / live-PV / live-top-K snapshot state.
// Called from both the worker create and reset paths so a fresh solve never
// inherits a stale leaderboard or node count from a prior solve.
static void reset_worker_analysis_state(EndgameCtxWorker *worker) {
  worker->local_nodes_searched = 0;
  atomic_store_explicit(&worker->published_nodes_searched, 0,
                        memory_order_relaxed);
  atomic_store_explicit(&worker->current_line_len, 0, memory_order_relaxed);
  atomic_store_explicit(&worker->live_pv_length, 0, memory_order_relaxed);
  atomic_store_explicit(&worker->live_pv_value, 0, memory_order_relaxed);
  atomic_store_explicit(&worker->live_pv_seq, 0, memory_order_relaxed);
  atomic_store_explicit(&worker->live_top_k_filled, 0, memory_order_relaxed);
  atomic_store_explicit(&worker->live_top_k_seq, 0, memory_order_relaxed);
}

// Reset an existing worker for a new solve without reallocating.
static void endgame_ctx_reset_worker(EndgameCtxWorker *worker,
                                     EndgameCtx *solver,
                                     const Game *template_game,
                                     uint64_t base_seed) {
  worker->solver = solver;
  game_copy(worker->game_copy, template_game);
  game_set_endgame_solving_mode(worker->game_copy);
  arena_reset(worker->small_move_arena);
  configure_worker_path_lists(worker, solver);
  memset(worker->move_undos, 0, sizeof(worker->move_undos));
  worker->num_played_moves = 0;
  worker->first_deferred_move = 0;
  memset(worker->occupancy_valid, 0, sizeof(worker->occupancy_valid));
  worker->stuck_tiles_salt += STUCK_TILES_SALT_STEP;
  lane_shadow_cache_set_salt(worker->lane_shadow_cache,
                             worker->stuck_tiles_salt);
  cross_set_cache_set_salt(worker->cross_set_cache, worker->stuck_tiles_salt);
  memset(worker->stuck_tiles_stamp, 0, sizeof(worker->stuck_tiles_stamp));
  prng_seed(worker->prng, base_seed + (uint64_t)worker->ordinal * 12345);
  worker->best_pv.game = worker->game_copy;
  worker->best_pv.num_moves = 0;
  worker->best_pv_value = -LARGE_VALUE;
  worker->completed_depth = 0;
  worker->nodes_since_deadline_check = 0;
  reset_worker_analysis_state(worker);
}

void endgame_ctx_reset_worker_and_game(EndgameCtx *solver, uint64_t base_seed,
                                       int worker_index) {
  endgame_ctx_reset_worker(solver->workers[worker_index], solver, solver->game,
                           base_seed);
  // When skip_word_pruning was used, pruned_kwgs are NULL; the game's existing
  // KWGs and cross-sets (set by the caller, e.g. PEG) are preserved.
  if (solver->pruned_kwgs[0] != NULL) {
    game_set_override_kwgs(solver->workers[worker_index]->game_copy,
                           solver->pruned_kwgs[0], solver->pruned_kwgs[1],
                           solver->dual_lexicon_mode);
    game_gen_all_cross_sets(solver->workers[worker_index]->game_copy);
  }
}

// Prepare the worker pool for a new solve. Computes pruned-KWG cross-sets
// on worker 0's game copy, then copies that state to the remaining workers.
// No temporary template game is allocated.
static void endgame_ctx_prepare_workers(EndgameCtx *solver,
                                        uint64_t base_seed) {
  const LetterDistribution *ld = game_get_ld(solver->game);

  solver->worker_base_seed = base_seed;

  // If the letter distribution changed (e.g., different lexicon), the worker
  // game copies have incompatibly-sized bags/racks. Destroy and rebuild.
  if (solver->workers_ld != NULL && solver->workers_ld != ld) {
    for (int idx = 0; idx < atomic_load(&solver->cap_workers); idx++) {
      solver_worker_destroy(solver->workers[idx]);
    }
    free(solver->workers);
    free(solver->worker_ids);
    solver->workers = NULL;
    solver->worker_ids = NULL;
    atomic_store(&solver->cap_workers, 0);
    solver->arr_cap = 0;
  }
  solver->workers_ld = ld;

  // Size the arrays to the injection ceiling so endgame_add_worker can append
  // late workers without reallocating the arrays while threads are running.
  const int needed_cap = solver->max_workers > solver->threads
                             ? solver->max_workers
                             : solver->threads;
  if (solver->arr_cap < needed_cap) {
    solver->workers = realloc_or_die(solver->workers,
                                     sizeof(EndgameCtxWorker *) * needed_cap);
    solver->worker_ids =
        realloc_or_die(solver->worker_ids, sizeof(cpthread_t) * needed_cap);
    for (int idx = solver->arr_cap; idx < needed_cap; idx++) {
      solver->workers[idx] = NULL;
    }
    solver->arr_cap = needed_cap;
  }
  // Create the initial worker structs lazily (reused across solves). Late
  // workers beyond solver->threads are created on demand by endgame_add_worker.
  // needed_cap >= threads above, so whenever this loop runs (threads > 0) the
  // array was allocated; assert it for the static analyzer and as a guard.
  assert(solver->threads == 0 || solver->workers != NULL);
  for (int idx = atomic_load(&solver->cap_workers); idx < solver->threads;
       idx++) {
    solver->workers[idx] =
        endgame_ctx_create_worker(solver, idx, base_seed, solver->game);
  }
  if (atomic_load(&solver->cap_workers) < solver->threads) {
    atomic_store(&solver->cap_workers, solver->threads);
  }
  // Cached workers persist across solves; the ordinal is the array position and
  // never changes. Each worker gets its MoveGen cache slot on demand from
  // get_movegen() (per-pthread), so there is no per-solve slot to reassign.
  for (int idx = 0; idx < solver->threads; idx++) {
    solver->workers[idx]->ordinal = idx;
  }
  // Copy worker 0's game state (with cross-sets) to workers 1..N-1
  for (int idx = 0; idx < solver->threads; idx++) {
    if (idx == 0) {
      endgame_ctx_reset_worker_and_game(solver, base_seed, idx);
    } else {
      endgame_ctx_reset_worker(solver->workers[idx], solver,
                               solver->workers[0]->game_copy, base_seed);
    }
  }
}

// The sum of the face values of the board tiles from (row, col) on, stepping
// by (row_step, col_step), up to the first empty or bricked square or the
// edge of the board.
static Equity board_run_score(const Board *board, const LetterDistribution *ld,
                              int row, int col, int row_step, int col_step) {
  Equity sum = 0;
  while (row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM &&
         !board_is_empty_or_bricked(board, row, col)) {
    const MachineLetter ml = board_get_letter(board, row, col);
    sum += get_is_blanked(ml) ? 0 : ld_get_score(ld, ml);
    row += row_step;
    col += col_step;
  }
  return sum;
}

// Whether (row, col) is on the board and holds a tile.
static inline bool occupancy_has_tile(const BoardOccupancy *occupancy, int row,
                                      int col) {
  return row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM &&
         (occupancy->occupied_rows[row] & (UINT32_C(1) << col)) != 0;
}

// The sum of the face values of the tiles beside the empty square (row, col)
// across a word along (row_step, col_step), or 0 when it has none: its cross
// score for the direction across.
static inline Equity tiles_beside_score(const Board *board,
                                        const BoardOccupancy *occupancy,
                                        int cross_set_index, int row, int col,
                                        int row_step, int col_step,
                                        bool *has_beside) {
  // A horizontal step (row_step == 0) has the tiles beside it above and
  // below, which the horizontal cross score sums.
  *has_beside = occupancy_has_tile(occupancy, row - col_step, col - row_step) ||
                occupancy_has_tile(occupancy, row + col_step, col + row_step);
  if (!*has_beside) {
    return 0;
  }
  return board_get_cross_score(board, row, col,
                               row_step == 0 ? BOARD_HORIZONTAL_DIRECTION
                                             : BOARD_VERTICAL_DIRECTION,
                               cross_set_index);
}

// At most what a drop worth tile_value (before multipliers) scores on the
// square (row, col), next to a run of tiles worth run_score that lies on the
// near side of it, stepping (row_step, col_step) away from that run, scored
// whether or not the words are in the lexicon; 0 off the board or on an
// occupied or bricked square. The word along the step holds the run, the
// drop and the board tiles beyond the square. The word across it holds only
// board tiles: the run lies on the line along the step, so no tile of the
// move is beside the square.
static Equity drop_bound_beside_run(const Board *board,
                                    const BoardOccupancy *occupancy,
                                    const LetterDistribution *ld,
                                    int cross_set_index, int row, int col,
                                    int row_step, int col_step,
                                    Equity run_score, Equity tile_value) {
  if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM ||
      (occupancy->unavailable_rows[row] & (UINT32_C(1) << col)) != 0) {
    return 0;
  }
  const BonusSquare bonus_square = board_get_bonus_square(board, row, col);
  const int word_multiplier = bonus_square_get_word_multiplier(bonus_square);
  const Equity tile_score =
      tile_value * bonus_square_get_letter_multiplier(bonus_square);
  Equity total = (run_score + tile_score +
                  board_run_score(board, ld, row + row_step, col + col_step,
                                  row_step, col_step)) *
                 word_multiplier;
  bool has_across;
  const Equity across_score =
      tiles_beside_score(board, occupancy, cross_set_index, row, col, row_step,
                         col_step, &has_across);
  if (has_across) {
    total += (across_score + tile_score) * word_multiplier;
  }
  return total;
}

// The best-scoring drop of one tile on an empty square.
typedef struct SingleDrop {
  Equity score;
  int row;
  int col;
  bool vertical;
  int start; // leftmost col (H) or topmost row (V) of the word
  int play_length;
  MachineLetter ml; // for blank: the letter the blank plays as
} SingleDrop;

// Finds the best-scoring drop of the tile tile_ml (or the blank, played as
// any letter, when is_blank) by scanning the cross-sets, bypassing the
// GADDAG. Returns false when it has no legal square.
static bool find_best_single_drop(const Board *board,
                                  const BoardOccupancy *occupancy,
                                  const LetterDistribution *ld,
                                  int cross_set_index, MachineLetter tile_ml,
                                  bool is_blank, SingleDrop *best) {
  assert(board_get_cross_sets_valid(board));
  const int ci = cross_set_index;
  const int ld_size = ld_get_size(ld);
  bool found = false;

  BoardOccupancy local_occupancy;
  if (occupancy == NULL) {
    board_occupancy_init(board, &local_occupancy);
    occupancy = &local_occupancy;
  }
  for (int row = 0; row < BOARD_DIM; row++) {
    uint32_t horizontal_neighbors;
    uint32_t vertical_neighbors;
    uint32_t candidates = board_occupancy_candidates(
        occupancy, row, &horizontal_neighbors, &vertical_neighbors);
    // Ascending columns preserve the original row-major tie-breaking order.
    while (candidates) {
      const int col = __builtin_ctz(candidates);
      const uint32_t square_bit = UINT32_C(1) << col;
      candidates &= candidates - 1;
      const bool has_v_nbrs = (vertical_neighbors & square_bit) != 0;
      const bool has_h_nbrs = (horizontal_neighbors & square_bit) != 0;

      // cross_set(H) validates the vertical word; cross_set(V) validates
      // the horizontal word.  The tile must satisfy both.
      uint64_t h_cs =
          board_get_cross_set(board, row, col, BOARD_HORIZONTAL_DIRECTION, ci);
      uint64_t v_cs =
          board_get_cross_set(board, row, col, BOARD_VERTICAL_DIRECTION, ci);
      uint64_t combined = h_cs & v_cs;

      // Reject before touching bonus squares or scanning word extents:
      // most candidate squares fail the cross-set test.
      if (!is_blank) {
        if (!board_is_letter_allowed_in_cross_set(combined, tile_ml)) {
          continue;
        }
      } else {
        // A blank is playable if the combined cross-set permits any
        // non-blank letter.
        if (!board_cross_set_allows_any_letter(combined)) {
          continue;
        }
      }

      BonusSquare bsq = board_get_bonus_square(board, row, col);
      int lm = bonus_square_get_letter_multiplier(bsq);
      int wm = bonus_square_get_word_multiplier(bsq);
      // cross_score(V) = sum of existing horizontal neighbor tile values
      // cross_score(H) = sum of existing vertical neighbor tile values
      Equity h_nbr_score =
          board_get_cross_score(board, row, col, BOARD_VERTICAL_DIRECTION, ci);
      Equity v_nbr_score = board_get_cross_score(
          board, row, col, BOARD_HORIZONTAL_DIRECTION, ci);

      // Blank has zero face value; its score depends only on position.
      Equity tile_ls = is_blank ? 0 : ld_get_score(ld, tile_ml) * lm;
      Equity score = 0;
      if (has_h_nbrs) {
        score += (tile_ls + h_nbr_score) * wm;
      }
      if (has_v_nbrs) {
        score += (tile_ls + v_nbr_score) * wm;
      }
      if (found && score <= best->score) {
        continue;
      }

      // New best: only now compute the word extent (for play_length and
      // col_start/row_start encoding) and, for a blank, pick its letter.
      // Use vertical direction only when there are vertical neighbors and no
      // horizontal neighbors; otherwise label the play horizontal.
      bool dir_vertical = has_v_nbrs && !has_h_nbrs;
      int word_start;
      int word_end;
      if (!dir_vertical) {
        word_start = col - 1;
        while (word_start >= 0 &&
               !board_is_empty_or_bricked(board, row, word_start)) {
          word_start--;
        }
        word_start++;
        word_end = col + 1;
        while (word_end < BOARD_DIM &&
               !board_is_empty_or_bricked(board, row, word_end)) {
          word_end++;
        }
        word_end--;
      } else {
        word_start = row - 1;
        while (word_start >= 0 &&
               !board_is_empty_or_bricked(board, word_start, col)) {
          word_start--;
        }
        word_start++;
        word_end = row + 1;
        while (word_end < BOARD_DIM &&
               !board_is_empty_or_bricked(board, word_end, col)) {
          word_end++;
        }
        word_end--;
      }

      MachineLetter chosen_ml = tile_ml;
      if (is_blank) {
        chosen_ml = 0;
        for (MachineLetter ml = 1; ml < (MachineLetter)ld_size; ml++) {
          if (board_is_letter_allowed_in_cross_set(combined, ml)) {
            chosen_ml = ml;
            break;
          }
        }
      }
      found = true;
      best->score = score;
      best->row = row;
      best->col = col;
      best->vertical = dir_vertical;
      best->start = word_start;
      best->play_length = word_end - word_start + 1;
      best->ml = chosen_ml;
    }
  }

  return found;
}

// What a node needs to bound the moves that leave one tile on the rack.
typedef struct KeepOneBound {
  const Board *board;
  const LetterDistribution *ld;
  const Rack *rack;
  int rack_size;
  // The sum of the rack's machine letters, a blank counting as
  // BLANK_MACHINE_LETTER.
  int rack_letter_sum;
  int cross_set_index;
  int32_t on_turn_spread;
  Equity out_bonus;
  // The node's occupancy, for the scans of the kept tiles' drops.
  const BoardOccupancy *occupancy;
  // Best drop of each tile kept so far (bit ml of scanned set), or -1 when it
  // has no legal square.
  uint64_t scanned;
  Equity best_drop[MAX_ALPHABET_SIZE];
} KeepOneBound;

static void keep_one_bound_init(KeepOneBound *bound, const Game *game,
                                int32_t on_turn_spread,
                                const BoardOccupancy *occupancy) {
  const int on_turn = game_get_player_on_turn_index(game);
  bound->board = game_get_board(game);
  bound->ld = game_get_ld(game);
  bound->rack = player_get_rack(game_get_player(game, on_turn));
  bound->rack_size = rack_get_total_letters(bound->rack);
  bound->rack_letter_sum = 0;
  for (int ml = 0; ml < ld_get_size(bound->ld); ml++) {
    bound->rack_letter_sum += ml * rack_get_letter(bound->rack, ml);
  }
  bound->cross_set_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), on_turn);
  bound->on_turn_spread = on_turn_spread;
  bound->occupancy = occupancy;
  bound->out_bonus = calculate_end_rack_points(
      player_get_rack(game_get_player(game, 1 - on_turn)), bound->ld);
  bound->scanned = 0;
}

// A move that leaves one tile y on the rack is worth at most its score, plus
// the most y can drop for next, plus the going-out bonus: the side to move's
// value after the move, from the node's spread. The opponent can always pass,
// so whatever they reply leaves the side to move no better off than a pass
// would; with one tile, the opponent is searched with their best drop only,
// which leaves less still. After a pass, y drops where it could before the
// move, which its best drop now bounds, or on a square whose words hold one
// of the move's tiles, which drop_bound_next_to bounds. A drop the move
// blocks only lowers what y can score, and if y cannot be played the game
// ends on passes with less than the going-out bonus.
static int32_t keep_one_bound(KeepOneBound *bound, const SmallMove *play_sm,
                              int32_t alpha) {
  const Board *board = bound->board;
  const LetterDistribution *ld = bound->ld;
  Move play;
  small_move_to_move(&play, play_sm, board);
  const bool vertical = board_is_dir_vertical(move_get_dir(&play));
  const int row_step = vertical ? 1 : 0;
  const int col_step = vertical ? 0 : 1;
  // The move leaves one tile, so it is the rack's sum of machine letters
  // less the played tiles' (a blank counting as BLANK_MACHINE_LETTER).
  int kept_sum = bound->rack_letter_sum;
  const int tiles_length = move_get_tiles_length(&play);
  for (int tile_idx = 0; tile_idx < tiles_length; tile_idx++) {
    const MachineLetter tile = move_get_tile(&play, tile_idx);
    if (tile != PLAYED_THROUGH_MARKER) {
      kept_sum -= get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile;
    }
  }
  const MachineLetter kept = (MachineLetter)kept_sum;
  const bool kept_is_blank = kept == BLANK_MACHINE_LETTER;
  if (!(bound->scanned & ((uint64_t)1 << kept))) {
    SingleDrop best;
    bound->best_drop[kept] = find_best_single_drop(board, bound->occupancy, ld,
                                                   bound->cross_set_index, kept,
                                                   kept_is_blank, &best)
                                 ? best.score
                                 : -1;
    bound->scanned |= (uint64_t)1 << kept;
  }
  const int32_t move_bound = bound->on_turn_spread +
                             (int32_t)small_move_get_score(play_sm) +
                             equity_to_int(bound->out_bonus);
  Equity next_drop = MAX(bound->best_drop[kept], 0);
  // The squares the move touched can only raise the bound, so when it already
  // beats alpha there is no need to look at them.
  if (move_bound + equity_to_int(next_drop) > alpha) {
    return move_bound + equity_to_int(next_drop);
  }
  const Equity kept_value = kept_is_blank ? 0 : ld_get_score(ld, kept);
  const int start_row = move_get_row_start(&play);
  const int start_col = move_get_col_start(&play);
  // The main word, the move's tiles and the board tiles it plays through,
  // and the empty squares at its two ends.
  Equity word_score = 0;
  for (int tile_idx = 0; tile_idx < tiles_length; tile_idx++) {
    const MachineLetter tile = move_get_tile(&play, tile_idx);
    const int row = start_row + row_step * tile_idx;
    const int col = start_col + col_step * tile_idx;
    const MachineLetter on_square = tile == PLAYED_THROUGH_MARKER
                                        ? board_get_letter(board, row, col)
                                        : tile;
    word_score += get_is_blanked(on_square) ? 0 : ld_get_score(ld, on_square);
  }
  const BoardOccupancy *occupancy = bound->occupancy;
  const int ci = bound->cross_set_index;
  next_drop =
      MAX(next_drop,
          drop_bound_beside_run(board, occupancy, ld, ci, start_row - row_step,
                                start_col - col_step, -row_step, -col_step,
                                word_score, kept_value));
  next_drop =
      MAX(next_drop,
          drop_bound_beside_run(board, occupancy, ld, ci,
                                start_row + row_step * tiles_length,
                                start_col + col_step * tiles_length, row_step,
                                col_step, word_score, kept_value));
  // Each placed tile joins the board tiles on either side of its square,
  // across the main word; the empty squares at the two ends of that run are
  // touched too.
  for (int tile_idx = 0; tile_idx < tiles_length; tile_idx++) {
    const MachineLetter tile = move_get_tile(&play, tile_idx);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = start_row + row_step * tile_idx;
    const int col = start_col + col_step * tile_idx;
    bool has_across;
    const Equity run_score =
        (get_is_blanked(tile) ? 0 : ld_get_score(ld, tile)) +
        tiles_beside_score(board, occupancy, ci, row, col, row_step, col_step,
                           &has_across);
    for (int sign = -1; sign <= 1; sign += 2) {
      const int across_row_step = sign * col_step;
      const int across_col_step = sign * row_step;
      int end_row = row + across_row_step;
      int end_col = col + across_col_step;
      while (occupancy_has_tile(occupancy, end_row, end_col)) {
        end_row += across_row_step;
        end_col += across_col_step;
      }
      next_drop =
          MAX(next_drop,
              drop_bound_beside_run(board, occupancy, ld, ci, end_row, end_col,
                                    across_row_step, across_col_step, run_score,
                                    kept_value));
    }
  }
  return move_bound + equity_to_int(next_drop);
}

// Generate moves for a single-tile rack by scanning cross-sets, bypassing
// the GADDAG entirely.  In a bag-empty endgame, every tile placement uses the
// last rack tile and ends the game, so only the best-scoring position matters.
// Always returns 1; writes the best-scoring placement to the arena, or a pass
// move if no legal placement exists.
static int generate_single_tile_plays(EndgameCtxWorker *worker,
                                      const BoardOccupancy *occupancy) {
  const Game *game = worker->game_copy;
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  int stm_idx = game_get_player_on_turn_index(game);
  const Rack *stm_rack = player_get_rack(game_get_player(game, stm_idx));
  bool kwgs_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
  int ci = board_get_cross_set_index(kwgs_shared, stm_idx);
  int ld_size = ld_get_size(ld);

  // Find the single tile on the rack.
  MachineLetter tile_ml = BLANK_MACHINE_LETTER;
  bool is_blank = false;
  for (int ml = 0; ml < ld_size; ml++) {
    if (rack_get_letter(stm_rack, ml) > 0) {
      tile_ml = ml;
      is_blank = (ml == BLANK_MACHINE_LETTER);
      break;
    }
  }

  SingleDrop best;
  const bool found =
      find_best_single_drop(board, occupancy, ld, ci, tile_ml, is_blank, &best);
  SmallMove *sm =
      (SmallMove *)arena_alloc(worker->small_move_arena, sizeof(SmallMove));
  sm->metadata.estimated_value = 0;

  if (!found) {
    // Stuck: the only legal move is pass.
    sm->metadata.score = 0;
    sm->metadata.play_length = 0;
    sm->metadata.tiles_played = 0;
    sm->tiny_move = 0;
    return 1;
  }

  sm->metadata.score = (uint16_t)equity_to_int(best.score);
  sm->metadata.play_length = (uint8_t)best.play_length;
  sm->metadata.tiles_played = 1;

  // Build tiny_move following the same convention as small_move_set_all:
  // for vertical, row_start and col_start are swapped before storing.
  uint64_t tm = (uint64_t)best.ml << SMALL_MOVE_TILES_SHIFT;
  if (is_blank) {
    tm |= 1ULL << SMALL_MOVE_BLANKS_SHIFT; // blank flag for tile index 0
  }
  if (best.vertical) {
    tm |= 1; // direction bit
    // small_move_set_all swaps row/col for vertical, so the true board
    // column goes in the column field and the true row in the row field.
    tm |= (uint64_t)best.col << SMALL_MOVE_COL_SHIFT;
    tm |= (uint64_t)best.start << SMALL_MOVE_ROW_SHIFT;
  } else {
    tm |= (uint64_t)best.start << SMALL_MOVE_COL_SHIFT;
    tm |= (uint64_t)best.row << SMALL_MOVE_ROW_SHIFT;
  }
  sm->tiny_move = tm;
  return 1;
}

// One tile making words in both directions has two legal notations. Compare
// its board square and machine letter (including blank designation), not the
// direction or the surrounding playthrough letters.
static bool endgame_moves_match(const Move *candidate,
                                const Move *actual_move) {
  if (compare_moves_without_equity(candidate, actual_move, true) == -1) {
    return true;
  }
  if (move_get_type(candidate) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
      move_get_type(actual_move) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
      candidate->tiles_played != 1 || actual_move->tiles_played != 1) {
    return false;
  }
  int candidate_offset = 0;
  int actual_offset = 0;
  while (candidate_offset < candidate->tiles_length &&
         candidate->tiles[candidate_offset] == PLAYED_THROUGH_MARKER) {
    candidate_offset++;
  }
  while (actual_offset < actual_move->tiles_length &&
         actual_move->tiles[actual_offset] == PLAYED_THROUGH_MARKER) {
    actual_offset++;
  }
  if (candidate_offset == candidate->tiles_length ||
      actual_offset == actual_move->tiles_length) {
    return false;
  }
  const bool candidate_vertical = board_is_dir_vertical(candidate->dir);
  const bool actual_vertical = board_is_dir_vertical(actual_move->dir);
  const int candidate_row =
      candidate->row_start + (candidate_vertical ? candidate_offset : 0);
  const int candidate_col =
      candidate->col_start + (candidate_vertical ? 0 : candidate_offset);
  const int actual_row =
      actual_move->row_start + (actual_vertical ? actual_offset : 0);
  const int actual_col =
      actual_move->col_start + (actual_vertical ? 0 : actual_offset);
  return candidate_row == actual_row && candidate_col == actual_col &&
         candidate->tiles[candidate_offset] ==
             actual_move->tiles[actual_offset];
}

// generate_single_tile_plays only keeps the single highest-scoring placement
// (see above), so the caller-specified actual_move (EndgameArgs.actual_move,
// pinned to root index 0 for exact-value extraction) can be entirely absent
// from that one-slot root move list if the move actually played wasn't the
// greedy best. When that happens, append it as a second root move so
// force_actual_move_to_front / extract_actual_move_pvline can still find it.
// Only meaningful at the root (depth == requested_plies): actual_move refers
// to the real historical position, not any node reached deeper in the search.
static int augment_single_tile_actual_move(EndgameCtxWorker *worker,
                                           int move_count) {
  const EndgameCtx *solver = worker->solver;
  if (!solver->actual_move) {
    return move_count;
  }
  const Board *board = game_get_board(worker->game_copy);

  const SmallMove *existing = (SmallMove *)worker->small_move_arena->memory;
  Move candidate;
  small_move_to_move(&candidate, existing, board);
  if (endgame_moves_match(&candidate, solver->actual_move)) {
    return move_count; // already present
  }

  SmallMove *actual_sm =
      (SmallMove *)arena_alloc(worker->small_move_arena, sizeof(SmallMove));
  actual_sm->metadata.estimated_value = 0;
  if (move_get_type(solver->actual_move) == GAME_EVENT_PASS) {
    small_move_set_as_pass(actual_sm);
  } else {
    const bool is_vertical = board_is_dir_vertical(solver->actual_move->dir);
    // small_move_set_all consumes movegen's traversal coordinates and swaps
    // vertical ones. Convert the validated move's physical coordinates first.
    int row_start = solver->actual_move->row_start;
    int col_start = solver->actual_move->col_start;
    if (is_vertical) {
      const int swap = row_start;
      row_start = col_start;
      col_start = swap;
    }
    small_move_set_all(actual_sm, solver->actual_move->tiles, 0,
                       solver->actual_move->tiles_length - 1,
                       solver->actual_move->score, row_start, col_start,
                       solver->actual_move->tiles_played, is_vertical,
                       solver->actual_move->move_type);
  }
  return move_count + 1;
}

// The side-to-move's plays from the incremental move lists, copied into the
// arena with the trailing pass a scratch generation would end in. Returns
// the play count including the pass.
static int copy_derived_plays_to_arena(EndgameCtxWorker *worker,
                                       const MoveGenArgs *args) {
  const SmallMove *derived = NULL;
  const int count =
      path_move_lists_moves_at(worker->path_lists, args, &derived);
  SmallMove *arena_small_moves = (SmallMove *)arena_alloc(
      worker->small_move_arena, (size_t)(count + 1) * sizeof(SmallMove));
  if (count > 0) {
    memcpy(arena_small_moves, derived, (size_t)count * sizeof(SmallMove));
  }
  small_move_set_as_pass(&arena_small_moves[count]);
  return count + 1;
}

// Records the move just played on the worker's game with `undo`.
static void push_played_move(EndgameCtxWorker *worker, MoveUndo *undo) {
  assert(worker->num_played_moves < EG_MAX_PLAYED_MOVES);
  worker->first_deferred_move_at_play[worker->num_played_moves] =
      worker->first_deferred_move;
  worker->played_move_undos[worker->num_played_moves] = undo;
  worker->num_played_moves++;
  worker->occupancy_valid[worker->num_played_moves] = false;
}

// The occupancy of the worker's board: the level's own when valid, else the
// nearest valid level below's with the tiles of the moves played since added,
// else read from the board.
static const BoardOccupancy *worker_board_occupancy(EndgameCtxWorker *worker) {
  const int level = worker->num_played_moves;
  BoardOccupancy *occupancy = &worker->occupancy_stack[level];
  if (worker->occupancy_valid[level]) {
    return occupancy;
  }
  int base = level - 1;
  while (base >= 0 && !worker->occupancy_valid[base]) {
    base--;
  }
  if (base < 0) {
    board_occupancy_init(game_get_board(worker->game_copy), occupancy);
  } else {
    *occupancy = worker->occupancy_stack[base];
    for (int move_idx = base; move_idx < level; move_idx++) {
      const MoveUndo *undo = worker->played_move_undos[move_idx];
      const bool vertical = board_is_dir_vertical(undo->move_dir);
      for (int tile_idx = 0; tile_idx < undo->move_tiles_length; tile_idx++) {
        if (!(undo->tiles_placed_mask & (1U << tile_idx))) {
          continue;
        }
        const int row = undo->move_row_start + (vertical ? tile_idx : 0);
        const int col = undo->move_col_start + (vertical ? 0 : tile_idx);
        occupancy->occupied_rows[row] |= UINT32_C(1) << col;
        occupancy->unavailable_rows[row] |= UINT32_C(1) << col;
      }
    }
  }
  worker->occupancy_valid[level] = true;
  return occupancy;
}

// Unplays the most recent move on the worker's game, whose undo is `undo`.
static void unplay_played_move(EndgameCtxWorker *worker, const MoveUndo *undo) {
  assert(worker->num_played_moves > 0 &&
         worker->played_move_undos[worker->num_played_moves - 1] == undo);
  worker->num_played_moves--;
  worker->first_deferred_move =
      worker->first_deferred_move_at_play[worker->num_played_moves];
  unplay_move_incremental(worker->game_copy, undo);
}

// Applies the deferred anchor and cross-set updates of every move still on
// the board, reading the board as it is now. The squares written are saved
// into the most recent move's undo, so unplaying that move restores them and
// defers the earlier moves' updates again. Call before anything reads the
// anchors or cross-sets: move generation, the stuck-tile scan, or the
// single-tile plays.
static void update_deferred_moves(EndgameCtxWorker *worker) {
  if (worker->first_deferred_move == worker->num_played_moves) {
    return;
  }
  MoveUndo *latest_undo =
      worker->played_move_undos[worker->num_played_moves - 1];
  for (int move_index = worker->first_deferred_move;
       move_index < worker->num_played_moves; move_index++) {
    const MoveUndo *deferred_undo = worker->played_move_undos[move_index];
    if (deferred_undo->move_tiles_length > 0) {
      update_deferred_move_into_undo(deferred_undo, latest_undo,
                                     worker->game_copy);
    }
  }
  worker->first_deferred_move = worker->num_played_moves;
  board_set_cross_sets_valid(game_get_board(worker->game_copy), true);
}

int generate_stm_plays(EndgameCtxWorker *worker, int depth,
                       const BoardOccupancy *occupancy) {
  // stm means side to move
  update_deferred_moves(worker);

  int stm_idx = game_get_player_on_turn_index(worker->game_copy);
  const Rack *stm_rack =
      player_get_rack(game_get_player(worker->game_copy, stm_idx));

  if (stm_rack->number_of_letters == 1) {
    int move_count = generate_single_tile_plays(worker, occupancy);
    if (depth == worker->solver->requested_plies) {
      move_count = augment_single_tile_actual_move(worker, move_count);
    }
    return move_count;
  }

  const MoveGenArgs args = {
      .game = worker->game_copy,
      .move_list = worker->move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = solver_get_pruned_kwg(worker->solver, stm_idx),
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  // Only the root (depth == requested_plies) generates from scratch when the
  // incremental lists are on; every deeper node derives its list from them.
  // The single-tile path above never materializes a list, which is safe: a
  // side that is down to one tile stays there for the rest of the path.
  const bool is_root = depth == worker->solver->requested_plies;
  if (worker->path_lists != NULL && !is_root) {
    return copy_derived_plays_to_arena(worker, &args);
  }
  generate_moves(&args);
  if (worker->path_lists != NULL && is_root) {
    path_move_lists_set_root(worker->path_lists, 0, worker->move_list);
  }

  SmallMove *arena_small_moves = (SmallMove *)arena_alloc(
      worker->small_move_arena, worker->move_list->count * sizeof(SmallMove));
  for (int i = 0; i < worker->move_list->count; i++) {
    arena_small_moves[i] = *(worker->move_list->small_moves[i]);
  }
  return worker->move_list->count;
}

// Seed the opponent's root list for the incremental move lists: their plays
// on the root board, which the ply-1 lists derive from. Generated with the
// opponent temporarily on turn, exactly as a scratch generation at ply 1
// would see them before the root move lands.
static void seed_opponent_root_list(EndgameCtxWorker *worker) {
  Game *game = worker->game_copy;
  const int on_turn_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - on_turn_idx;
  const Rack *opp_rack = player_get_rack(game_get_player(game, opp_idx));
  if (rack_get_total_letters(opp_rack) < 2) {
    // Ply-1 nodes take generate_stm_plays' single-tile path instead.
    return;
  }
  game_set_player_on_turn_index(game, opp_idx);
  const MoveGenArgs args = {
      .game = game,
      .move_list = worker->move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = solver_get_pruned_kwg(worker->solver, opp_idx),
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  game_set_player_on_turn_index(game, on_turn_idx);
  path_move_lists_set_root(worker->path_lists, 1, worker->move_list);
}

// Sum the face values (tile_face_values, by machine letter) of the rack tiles
// a move places; a designated blank counts as the blank.
static int compute_played_tiles_face_value(const SmallMove *small_move,
                                           const int *tile_face_values) {
  int face_value = 0;
  const int tiles_played = small_move_get_tiles_played(small_move);
  for (int tile_idx = 0; tile_idx < tiles_played; tile_idx++) {
    const MachineLetter ml = small_move_tile_is_blank(small_move, tile_idx)
                                 ? BLANK_MACHINE_LETTER
                                 : small_move_get_tile(small_move, tile_idx);
    face_value += tile_face_values[ml];
  }
  return face_value;
}

// Conservation bonus: penalize playing tiles when opponent has stuck tiles.
// Returns (CONSERVATION_TILE_WEIGHT * tile_count +
//          CONSERVATION_VALUE_WEIGHT * face_value) * opp_stuck_frac.
static int compute_conservation_bonus(const SmallMove *small_move,
                                      const int *tile_face_values,
                                      float opp_stuck_frac) {
  const int tiles_played = small_move_get_tiles_played(small_move);
  const int face_value =
      compute_played_tiles_face_value(small_move, tile_face_values);
  return (int)((float)(CONSERVATION_TILE_WEIGHT * tiles_played +
                       CONSERVATION_VALUE_WEIGHT * face_value) *
               opp_stuck_frac);
}

// Whether another worker may be searching this solve's tree alongside this one,
// which is when ABDADA's bookkeeping pays. solver->threads alone does not say:
// endgame_add_worker lends helpers to a running solve and raises only
// live_workers, and endgame_solve_inline always runs with threads == 1. A
// helper (ordinal > 0) always has company, and the main worker sees helpers
// arrive through live_workers. A single-threaded solve that nobody helps pays
// one relaxed load, and only at the nodes where ABDADA could act.
static bool worker_has_company(const EndgameCtxWorker *worker) {
  return worker->ordinal > 0 || worker->solver->threads > 1 ||
         atomic_load_explicit(&worker->solver->live_workers,
                              memory_order_relaxed) > 1;
}

// Thread jitter for search diversity, for the workers lent to a running solve
// (endgame_add_worker, ordinals from threads up) only: each gets a unique bias
// based on tiles played, odd ordinals favoring aggressive play and even ones
// conservative play. A worker that joins late in the main worker's order
// retraces its path behind it and keeps running into the nodes it is
// searching.
//
// The workers a solve starts with get none. They reach each node together and
// all search its first move, while ABDADA shares out the younger brothers. A
// jittered helper's first move differs, and where the usual first move refutes
// the node, which it nearly always does, the helper's work there is wasted.
static int compute_thread_jitter(EndgameCtxWorker *worker, int tiles_played) {
  int thread_idx = worker->ordinal;
  if (thread_idx < worker->solver->threads) {
    return 0;
  }
  int bias = thread_idx * tiles_played;
  int jitter = (thread_idx % 2 == 1) ? bias : -bias;
  jitter += (int)(prng_get_random_number(worker->prng, THREAD_JITTER_NOISE)) -
            THREAD_JITTER_NOISE / 2;
  return jitter;
}

// Compute build chain values for recursive build detection. When opponent is
// stuck, cumulative build chain values reward moves that build toward longer
// scoring sequences (e.g., ED->RED->IRED->AIRED->WAIRED).
// Returns the values in the worker's scratch, valid until the next call, or
// NULL when move_count <= 1 or opp_stuck_frac == 0.
static int *compute_build_chain_values(EndgameCtxWorker *worker, int move_count,
                                       size_t arena_offset,
                                       float opp_stuck_frac) {
  if (opp_stuck_frac <= 0.0F || move_count <= 1) {
    return NULL;
  }

  const int scratch_needed = BUILD_CHAIN_SCRATCH_ARRAYS * move_count;
  if (scratch_needed > worker->build_chain_scratch_capacity) {
    worker->build_chain_scratch_capacity = scratch_needed;
    worker->build_chain_scratch = realloc_or_die(
        worker->build_chain_scratch, sizeof(int) * (size_t)scratch_needed);
  }
  const SmallMove *moves =
      (const SmallMove *)(worker->small_move_arena->memory + arena_offset);
  // Which squares hold tiles, for matching the tiles of nested moves.
  const BoardOccupancy *occupancy = worker_board_occupancy(worker);
  // By move: its value and its line.
  int *build_values = worker->build_chain_scratch;
  int *line = build_values + move_count;
  // The moves ordered by tiles_played descending, then in the final order.
  int *by_tiles = line + move_count;
  int *order = by_tiles + move_count;
  // By position in the final order: where the move's span starts along its
  // line and where it ends, one past its last square.
  int *span_start = order + move_count;
  int *span_end = span_start + move_count;

  // Only a move on the same line, in the same direction, can contain another,
  // so the moves are grouped by line (a pass, the row of a horizontal move,
  // the column of a vertical one) and, within a line, ordered by tiles_played
  // descending for bottom-up computation. A move is only compared with the
  // moves of its own line. A stable counting sort by tiles_played and then
  // one by line give that order, each over a handful of counts.
  int tile_starts[RACK_SIZE + 2] = {0};
  int line_starts[BUILD_CHAIN_LINES + 1] = {0};
  for (int i = 0; i < move_count; i++) {
    const SmallMove *sm = &moves[i];
    line[i] = 0;
    if (!small_move_is_pass(sm)) {
      line[i] = 1 + (small_move_is_vertical(sm)
                         ? BOARD_DIM + small_move_get_col_start(sm)
                         : small_move_get_row_start(sm));
    }
    line_starts[line[i] + 1]++;
    tile_starts[RACK_SIZE - small_move_get_tiles_played(sm) + 1]++;
  }
  for (int count_idx = 0; count_idx < RACK_SIZE + 1; count_idx++) {
    tile_starts[count_idx + 1] += tile_starts[count_idx];
  }
  for (int line_idx = 0; line_idx < BUILD_CHAIN_LINES; line_idx++) {
    line_starts[line_idx + 1] += line_starts[line_idx];
  }
  for (int i = 0; i < move_count; i++) {
    by_tiles[tile_starts[RACK_SIZE -
                         small_move_get_tiles_played(&moves[i])]++] = i;
  }
  for (int tiles_idx = 0; tiles_idx < move_count; tiles_idx++) {
    const int i = by_tiles[tiles_idx];
    const int oi = line_starts[line[i]]++;
    order[oi] = i;
    span_start[oi] = small_move_is_vertical(&moves[i])
                         ? small_move_get_row_start(&moves[i])
                         : small_move_get_col_start(&moves[i]);
    span_end[oi] = span_start[oi] + small_move_get_play_length(&moves[i]);
  }

  // Bottom-up: process moves from most tiles to fewest. The moves of oi's
  // line with more tiles than it are those from line_begin to
  // more_tiles_end, already processed, so their build_values are final.
  int line_begin = 0;
  int more_tiles_end = 0;
  for (int oi = 0; oi < move_count; oi++) {
    const int i = order[oi];
    const SmallMove *sm_a = &moves[i];
    const int tp_a = small_move_get_tiles_played(sm_a);
    if (oi == 0 || line[i] != line[order[oi - 1]]) {
      line_begin = oi;
      more_tiles_end = oi;
    } else if (tp_a != small_move_get_tiles_played(&moves[order[oi - 1]])) {
      more_tiles_end = oi;
    }
    build_values[i] = small_move_get_score(sm_a);
    if (small_move_is_pass(sm_a)) {
      continue;
    }
    const bool vert_a = small_move_is_vertical(sm_a);
    const int row_a = small_move_get_row_start(sm_a);
    const int col_a = small_move_get_col_start(sm_a);

    // Find the best extension: a move of the line with more tiles whose span
    // contains this one's.
    int best_extension = 0;
    for (int oj = line_begin; oj < more_tiles_end; oj++) {
      if (span_start[oi] < span_start[oj] || span_end[oi] > span_end[oj]) {
        continue;
      }
      const int j = order[oj];
      const SmallMove *sm_b = &moves[j];

      // Verify the tiles match. Both moves fill every empty square of their
      // spans, so A's placed tiles are B's, from the B tile after the empty
      // squares between their starts.
      int offset_in_b = 0;
      if (!vert_a) {
        const int col_b = span_start[oj];
        const uint32_t before_a =
            ((UINT32_C(1) << col_a) - 1) & ~((UINT32_C(1) << col_b) - 1);
        offset_in_b =
            col_a - col_b -
            __builtin_popcount(occupancy->occupied_rows[row_a] & before_a);
      } else {
        for (int row = span_start[oj]; row < row_a; row++) {
          if (!(occupancy->occupied_rows[row] & (UINT32_C(1) << col_a))) {
            offset_in_b++;
          }
        }
      }
      bool tiles_match = true;
      for (int tile_idx = 0; tile_idx < tp_a; tile_idx++) {
        if (small_move_get_tile(sm_a, tile_idx) !=
                small_move_get_tile(sm_b, tile_idx + offset_in_b) ||
            small_move_tile_is_blank(sm_a, tile_idx) !=
                small_move_tile_is_blank(sm_b, tile_idx + offset_in_b)) {
          tiles_match = false;
          break;
        }
      }
      if (!tiles_match) {
        continue;
      }

      if (build_values[j] > best_extension) {
        best_extension = build_values[j];
      }
    }
    if (best_extension > 0) {
      build_values[i] += best_extension;
    }
  }
  return build_values;
}

// Assign estimated values to the freshly generated moves at the top of the
// arena. Does not sort: interior nodes order moves lazily in abdada_negamax
// (selection picks + one tail sort), and the root sorts explicitly.
void assign_estimates(EndgameCtxWorker *worker, int move_count,
                      uint64_t tt_move, float opp_stuck_frac) {
  const int player_index = game_get_player_on_turn_index(worker->game_copy);
  const Player *player = game_get_player(worker->game_copy, player_index);
  const Player *opponent = game_get_player(worker->game_copy, 1 - player_index);
  const Rack *stm_rack = player_get_rack(player);
  const Rack *other_rack = player_get_rack(opponent);
  int ntiles_on_rack = stm_rack->number_of_letters;
  bool last_move_was_pass = game_get_consecutive_scoreless_turns(
                                worker->game_copy) == 1; // check if this is ok.

  size_t arena_offset =
      worker->small_move_arena->size - (sizeof(SmallMove) * move_count);

  const int *build_values = compute_build_chain_values(
      worker, move_count, arena_offset, opp_stuck_frac);

  const int *tile_face_values = worker->solver->tile_face_values;

  for (size_t i = 0; i < (size_t)move_count; i++) {
    size_t element_offset = arena_offset + i * sizeof(SmallMove);
    SmallMove *current_move =
        (SmallMove *)(worker->small_move_arena->memory + element_offset);

    bool is_non_pass_partial =
        !small_move_is_pass(current_move) &&
        small_move_get_tiles_played(current_move) < ntiles_on_rack;

    // Conservation bonus: penalize playing tiles when opponent has stuck tiles.
    int conservation_bonus = 0;
    if (opp_stuck_frac > 0.0F && is_non_pass_partial) {
      conservation_bonus = compute_conservation_bonus(
          current_move, tile_face_values, opp_stuck_frac);
    }

    if (small_move_get_tiles_played(current_move) == ntiles_on_rack) {
      small_move_set_estimated_value(
          current_move,
          equity_to_int(int_to_equity(small_move_get_score(current_move)) +
                        (calculate_end_rack_points(
                            other_rack, game_get_ld(worker->game_copy)))) |
              GOING_OUT_BF);
    } else if (build_values) {
      // Use build-chain estimate as center: when opponent has stuck tiles,
      // prorate the chain boost by stuck fraction. build_values[i] includes
      // the move's own score plus any extension chain value.
      int score = small_move_get_score(current_move);
      int estimate;
      if (build_values[i] > score) {
        estimate =
            score + (int)(opp_stuck_frac * (float)(build_values[i] - score));
      } else {
        estimate = score;
      }
      estimate -= conservation_bonus;
      estimate += compute_thread_jitter(
          worker, small_move_get_tiles_played(current_move));
      small_move_set_estimated_value(current_move, estimate);
    } else {
      int score = small_move_get_score(current_move);
      int estimate = score - conservation_bonus;
      // Static endgame equity: score minus face value of tiles played.
      if (is_non_pass_partial) {
        estimate -=
            compute_played_tiles_face_value(current_move, tile_face_values);
      }
      estimate += compute_thread_jitter(
          worker, small_move_get_tiles_played(current_move));
      small_move_set_estimated_value(current_move, estimate);
    }

    if (current_move->tiny_move == tt_move) {
      small_move_add_estimated_value(current_move, HASH_MOVE_BF);
    }

    // Consider pass first if we've just passed. This will allow us to see that
    // branch of the tree faster.
    if (last_move_was_pass && small_move_is_pass(current_move)) {
      small_move_add_estimated_value(current_move, EARLY_PASS_BF);
    }
  }
}

// assign_estimates plus a full sort by estimated value, biggest first.
// Used for the root move list, which is iterated in full every depth.
void assign_estimates_and_sort(EndgameCtxWorker *worker, int move_count,
                               uint64_t tt_move, float opp_stuck_frac) {
  assign_estimates(worker, move_count, tt_move, opp_stuck_frac);
  SmallMove *small_moves = (SmallMove *)(worker->small_move_arena->memory +
                                         worker->small_move_arena->size -
                                         (sizeof(SmallMove) * move_count));
  stable_sort(small_moves, move_count, sizeof(SmallMove),
              compare_small_moves_by_estimated_value);
}

// Play play_idx of a playout step's nplays plays: those of move_list, or,
// when in_place, plays[0] to plays[nplays - 2] and then pass. The incremental
// lists' plays are read where they lie rather than copied into move_list; in
// that order, the pass last, they are the list a MOVE_RECORD_ALL_SMALL
// generation would produce. An empty list's plays may be NULL, so in_place,
// not plays, says where they are.
static inline const SmallMove *
playout_step_play(const MoveList *move_list, bool in_place,
                  const SmallMove *plays, int nplays, const SmallMove *pass,
                  int play_idx) {
  if (!in_place) {
    return move_list->small_moves[play_idx];
  }
  return play_idx < nplays - 1 ? &plays[play_idx] : pass;
}

// Sets `best` to the best of `plays` by score, plus `go_out_bonus` for a play
// of all `side_to_move_tiles` tiles, ties going to the larger tiny_move: the
// order MOVE_RECORD_BEST_SMALL and path_move_lists_best_play rank by. Leaves
// `best` untouched when there are no plays.
static void pick_best_playout_play(const SmallMove *plays, int count,
                                   int go_out_bonus, int side_to_move_tiles,
                                   SmallMove *best) {
  int best_adjusted = INT32_MIN;
  const SmallMove *best_play = NULL;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const SmallMove *candidate = &plays[move_idx];
    int adjusted = small_move_get_score(candidate);
    if (small_move_get_tiles_played(candidate) == side_to_move_tiles) {
      adjusted += go_out_bonus;
    }
    if (best_play == NULL || adjusted > best_adjusted ||
        (adjusted == best_adjusted &&
         candidate->tiny_move > best_play->tiny_move)) {
      best_adjusted = adjusted;
      best_play = candidate;
    }
  }
  if (best_play != NULL) {
    *best = *best_play;
  }
}

// The first step of a greedy playout with no stuck tiles, from the incremental
// lists: leaves the best play (the pass when there is none) as the only entry
// of worker->move_list, exactly as the step's MOVE_RECORD_BEST_SMALL generation
// would, and returns 1. `leaf_plays` is the side to move's list when the stuck
// hint has already materialized it, else NULL.
static int playout_first_step_from_lists(EndgameCtxWorker *worker,
                                         const SmallMove *leaf_plays,
                                         int leaf_play_count) {
  const int stm_idx = game_get_player_on_turn_index(worker->game_copy);
  const Rack *opp_rack =
      player_get_rack(game_get_player(worker->game_copy, 1 - stm_idx));
  const Equity go_out_bonus =
      calculate_end_rack_points(opp_rack, game_get_ld(worker->game_copy));
  SmallMove best;
  small_move_set_as_pass(&best);
  if (leaf_plays != NULL) {
    const int stm_tiles =
        player_get_rack(game_get_player(worker->game_copy, stm_idx))
            ->number_of_letters;
    pick_best_playout_play(leaf_plays, leaf_play_count,
                           equity_to_int(go_out_bonus), stm_tiles, &best);
  } else {
    const MoveGenArgs best_args = {
        .best_small_out_bonus = go_out_bonus,
        .best_small_canonical_ties = true,
        .game = worker->game_copy,
        .move_list = worker->move_list,
        .move_record_type = MOVE_RECORD_BEST_SMALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = solver_get_pruned_kwg(worker->solver, stm_idx),
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .lane_shadow_cache = worker->lane_shadow_cache,
    };
    SmallMove found;
    if (path_move_lists_best_play(worker->path_lists, &best_args, &found)) {
      best = found;
    }
  }
  // Looked up only now: generating into the list reorders its entries.
  *worker->move_list->small_moves[0] = best;
  worker->move_list->count = 1;
  return 1;
}

// The key of the worker's board and the opponent's rack in its stuck-tiles
// cache: node_key, which hashes the board, both racks, the side to move and
// the scoreless count, less the solving player's rack, the side to move and
// the scoreless count, salted for the solve. 0, which disables the cache,
// without the transposition table.
static uint64_t stuck_tiles_key(const EndgameCtxWorker *worker,
                                uint64_t node_key) {
  const EndgameCtx *solver = worker->solver;
  if (!solver->transposition_table_optim) {
    return 0;
  }
  const Zobrist *zobrist = solver->transposition_table->zobrist;
  const Game *game = worker->game_copy;
  const Rack *solving_rack =
      player_get_rack(game_get_player(game, solver->solving_player));
  uint64_t key = node_key;
  for (int ml = 0; ml < rack_get_dist_size(solving_rack); ml++) {
    key ^= zobrist->our_rack_table[ml][rack_get_letter(solving_rack, ml)];
  }
  if (game_get_player_on_turn_index(game) != solver->solving_player) {
    key ^= zobrist->their_turn;
  }
  key ^= zobrist->scoreless_turns[game_get_consecutive_scoreless_turns(game)];
  return key ^ worker->stuck_tiles_salt;
}

// compute_opp_stuck_fraction at the worker's position (`node_key`), with the
// opponent's playable tile types at the position before the last move as its
// hint when the worker has them, and keeping this position's for the
// positions after it. Only a search along the incremental lists' path keeps
// them: its prefix stamps tell when a kept answer is for the current path.
static float worker_opp_stuck_fraction(EndgameCtxWorker *worker,
                                       uint64_t node_key,
                                       const SmallMove *opp_plays,
                                       int opp_play_count,
                                       const BoardOccupancy *occupancy,
                                       uint64_t *tiles_played_bv_out) {
  EndgameCtx *solver = worker->solver;
  const int opp_idx = 1 - solver->solving_player;
  const PathMoveLists *lists = worker->path_lists;
  StuckTilesHint hint;
  const StuckTilesHint *hint_or_null = NULL;
  if (lists != NULL && lists->length >= 1) {
    const int before = lists->length - 1;
    const uint64_t stamp = worker->stuck_tiles_stamp[before];
    if (stamp != 0 && stamp == path_move_lists_prefix_stamp(lists, before)) {
      hint.playable_before = worker->stuck_tiles_playable[before];
      hint.lanes = path_move_lists_last_move_lanes(lists);
      hint.lane_hits = path_move_lists_last_move_hits(lists);
      hint_or_null = &hint;
    }
  }
  uint64_t playable = 0;
  const float frac = compute_opp_stuck_fraction(
      worker->game_copy, worker->move_list,
      solver_get_pruned_kwg(solver, opp_idx), opp_idx, &playable, solver,
      opp_plays, opp_play_count, occupancy, worker->stuck_tiles_cache,
      stuck_tiles_key(worker, node_key), hint_or_null);
  // An interrupted check's tile types are incomplete.
  if (lists != NULL && !iterative_deepening_should_stop(solver)) {
    worker->stuck_tiles_playable[lists->length] = playable;
    worker->stuck_tiles_stamp[lists->length] =
        path_move_lists_prefix_stamp(lists, lists->length);
  }
  if (tiles_played_bv_out != NULL) {
    *tiles_played_bv_out = playable;
  }
  return frac;
}

// Greedy playout at depth==0 leaf nodes: generate moves iteratively,
// pick best (with conservation bonus), compute final spread with rack
// adjustments, unplay moves, store in TT. Returns evaluation from
// on_turn's perspective.
// Through iterative-deepening depth 1 (the root sweep and the depth-1 search)
// a leaf plays one greedy move before the rack adjustment instead of playing
// out: at those depths it chooses better. From depth 2 the full playout is
// better. With the bypass, a leaf whose opponent can only pass is exempt (see
// negamax_greedy_leaf_playout).
static inline bool leaf_playout_is_capped(const EndgameCtxWorker *worker) {
  return worker->current_iterative_deepening_depth <= 1;
}

// The same position gets a different leaf value capped and uncapped, and with
// several threads both kinds are in flight at once (helpers start at depth 2
// or deeper while thread 0 runs depth 1). Leaf values found through depth 1
// live under a salted key so a deeper search never reads one, and vice versa.
// A position exempt from the cap is exempt wherever depth 1 meets it, so its
// salted key holds only uncapped values.
static inline uint64_t leaf_tt_key(const EndgameCtxWorker *worker,
                                   uint64_t node_key) {
  return leaf_playout_is_capped(worker) ? node_key ^ CAPPED_LEAF_KEY_SALT
                                        : node_key;
}

static int32_t negamax_greedy_leaf_playout(EndgameCtxWorker *worker,
                                           uint64_t node_key, int on_turn_idx,
                                           int32_t on_turn_spread, PVLine *pv,
                                           bool pv_node, float opp_stuck_frac) {
  // Negamax checks the deadline only every DEPTH_DEADLINE_CHECK_INTERVAL
  // nodes, which at shallow depths, where every node is a playout, can be a
  // large share of a short budget. A clock read is cheap next to a playout.
  if (check_depth_deadline(worker)) {
    return ABDADA_INTERRUPTED;
  }
  int solving_player = worker->solver->solving_player;
  int plies = worker->solver->requested_plies;
  int playout_depth = 0;
  // The incremental lists can follow the playout only from a leaf reached
  // along a pushed path (never from the root itself).
  const bool use_path_lists =
      worker->path_lists != NULL && worker->path_lists->length >= 1;
  // The side to move's derived list at the leaf, when it was materialized
  // for the stuck fraction; the first playout step reuses it.
  const SmallMove *leaf_plays = NULL;
  int leaf_play_count = 0;
  SmallMove pass;
  small_move_set_as_pass(&pass);

  // Recompute opp_stuck_frac from the current position rather than using the
  // parent's value. This ensures position-dependent (not path-dependent)
  // evaluation, which is required for TT consistency across threads and
  // transpositions.
  if (worker->solver->use_heuristics) {
    // compute_opp_stuck_fraction reads the cross-sets.
    update_deferred_moves(worker);
    int opp_idx = 1 - solving_player;
    const Rack *leaf_opp_rack =
        player_get_rack(game_get_player(worker->game_copy, opp_idx));
    // With the opponent on turn, their derived list answers exactly, and a
    // stuck playout's first step needs that same list. The parent's fraction
    // is the hint that the position is stuck: deriving first then costs one
    // list instead of a tiles-played generation plus that list; when the
    // hint is wrong, only a cheap tiles-played generation was skipped.
    if (use_path_lists && opp_stuck_frac > 0.0F && on_turn_idx == opp_idx &&
        rack_get_total_letters(leaf_opp_rack) >= 2) {
      const MoveGenArgs leaf_args = {
          .game = worker->game_copy,
          .move_list = worker->move_list,
          .move_record_type = MOVE_RECORD_ALL_SMALL,
          .move_sort_type = MOVE_SORT_SCORE,
          .override_kwg = solver_get_pruned_kwg(worker->solver, opp_idx),
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
      };
      leaf_play_count =
          path_move_lists_moves_at(worker->path_lists, &leaf_args, &leaf_plays);
    }
    opp_stuck_frac =
        worker_opp_stuck_fraction(worker, node_key, leaf_plays, leaf_play_count,
                                  worker_board_occupancy(worker), NULL);
  }

  // The playout runs to the end of the game. A stuck playout pushes each of
  // its moves onto the incremental lists' path, on top of up to 2 * plies
  // negamax and bypass moves, so it also stops where the path is full.
  int max_playout = EG_MAX_LEAF_PLAYOUT;
  // Depth 1's leaves have the solving player's opponent on turn, so when the
  // opponent can only pass, the capped playout's one move is that pass.
  // Without the bypass, depth 2 plays out from after the pass. With it, the
  // pass spends no ply, and depth 2 searches two of the solving player's
  // moves, a far larger search that a time limit can stop short of. Such a
  // leaf plays out in full instead, as depth 2 does without the bypass.
  if (leaf_playout_is_capped(worker) &&
      !(worker->solver->forced_pass_bypass && opp_stuck_frac >= 1.0F)) {
    max_playout = 1;
  }
  if (use_path_lists) {
    const int path_room =
        PATH_MOVE_LISTS_MAX_PATH - 1 - worker->path_lists->length;
    if (path_room < max_playout) {
      max_playout = path_room;
    }
  }

  // Only a stuck playout derives its lists, so only it extends the path.
  // (opp_stuck_frac is fixed for the whole playout.) A step derives from the
  // same side's list two moves up. Where that list was never built (the
  // second step's, when the leaf's parent cut off on its first reply), the
  // step generates from scratch, which gives the same plays in the same
  // order, and stores its list for the steps that follow.
  const bool follow_path = use_path_lists && opp_stuck_frac > 0.0F;

  // A stuck playout often passes through a position an earlier one reached,
  // the same moves in another order, and from there it plays the same moves.
  // So a stuck playout stores, for each position it passes through, the value
  // of the rest of the playout from there and its length, and a later playout
  // that reaches the position stops there. The rest depends on the stuck
  // fraction found at the leaf, so the key carries it. A playout cut short by
  // max_playout stores nothing, and a stored rest is used only where the
  // playout has room for all of it. A PV node plays its rest out for its
  // line, as it does where its own value is stored. The worker's own small
  // cache holds them: in the transposition table, every step's probe and store
  // missed the processor's caches in a large search, and cost more than the
  // rests saved.
  const bool cache_tails = worker->solver->transposition_table_optim &&
                           opp_stuck_frac > 0.0F && max_playout > 1;
  uint32_t frac_bits;
  memcpy(&frac_bits, &opp_stuck_frac, sizeof(frac_bits));
  const uint64_t tail_salt = PLAYOUT_TAIL_KEY_SALT ^
                             ((uint64_t)frac_bits << 32) ^
                             worker->stuck_tiles_salt;
  uint64_t tail_key = node_key ^ tail_salt;
  if (cache_tails) {
    worker->playout_tail_keys[0] = tail_key;
    worker->playout_tail_spreads[0] =
        on_turn_idx == solving_player ? on_turn_spread : -on_turn_spread;
  }
  bool tail_hit = false;
  int32_t hit_tail_value = 0;
  int hit_tail_length = 0;

  bool playout_interrupted = false;
  while (game_get_game_end_reason(worker->game_copy) == GAME_END_REASON_NONE &&
         playout_depth < max_playout) {
    if (thread_control_search_checkpoint(worker->solver->thread_control) ==
        THREAD_CONTROL_STATUS_USER_INTERRUPT) {
      playout_interrupted = true;
      break;
    }
    update_deferred_moves(worker);

    int stm_idx_p = game_get_player_on_turn_index(worker->game_copy);
    const Rack *stm_rack_p =
        player_get_rack(game_get_player(worker->game_copy, stm_idx_p));
    int nplays;
    // Whether the step's plays are the incremental lists' plays, read in
    // place, and then the pass (see playout_step_play), rather than
    // worker->move_list.
    bool in_place = false;
    const SmallMove *plays = NULL;
    if (stm_rack_p->number_of_letters == 1) {
      // Single-tile fast path: cross-set scan instead of KWG traversal.
      // Its move is still pushed when following the path, without a
      // materialized list; that is safe for the reason generate_stm_plays
      // gives: a side down to one tile never needs its list derived again.
      // generate_single_tile_plays allocs to the arena; copy the result into
      // move_list and immediately pop the arena so the per-node dealloc in
      // abdada_negamax stays correct.
      size_t arena_before = worker->small_move_arena->size;
      generate_single_tile_plays(worker, worker_board_occupancy(worker));
      const SmallMove *arena_sm =
          (const SmallMove *)(worker->small_move_arena->memory + arena_before);
      *worker->move_list->small_moves[0] = *arena_sm;
      arena_dealloc(worker->small_move_arena, sizeof(SmallMove));
      worker->move_list->count = 1;
      nplays = 1;
    } else if (opp_stuck_frac == 0.0F && use_path_lists && playout_depth == 0 &&
               worker->solver->best_reply_shortcuts) {
      // No stuck tiles, first step: the incremental lists already hold most of
      // this position's plays.
      nplays =
          playout_first_step_from_lists(worker, leaf_plays, leaf_play_count);
    } else if (opp_stuck_frac == 0.0F) {
      // No stuck tiles: only the best play is needed, ranked by score plus
      // twice the opponent's rack for going out.
      // MOVE_RECORD_BEST_SMALL prunes during generation via shadow upper
      // bounds, producing a SmallMove directly without full enumeration.
      // With the best-reply shortcuts, ties go the way the incremental lists
      // break them, so a search plays the same playouts with or without them.
      const Rack *opp_rack_p =
          player_get_rack(game_get_player(worker->game_copy, 1 - stm_idx_p));
      const MoveGenArgs pargs = {
          .best_small_out_bonus = calculate_end_rack_points(
              opp_rack_p, game_get_ld(worker->game_copy)),
          .best_small_canonical_ties = worker->solver->best_reply_shortcuts,
          .game = worker->game_copy,
          .move_list = worker->move_list,
          .move_record_type = MOVE_RECORD_BEST_SMALL,
          .move_sort_type = MOVE_SORT_SCORE,
          .override_kwg = solver_get_pruned_kwg(
              worker->solver, game_get_player_on_turn_index(worker->game_copy)),
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .lane_shadow_cache = worker->lane_shadow_cache,
      };
      generate_moves(&pargs);
      nplays = worker->move_list->count;
    } else {
      const MoveGenArgs pargs = {
          .game = worker->game_copy,
          .move_list = worker->move_list,
          .move_record_type = MOVE_RECORD_ALL_SMALL,
          .move_sort_type = MOVE_SORT_SCORE,
          .override_kwg = solver_get_pruned_kwg(
              worker->solver, game_get_player_on_turn_index(worker->game_copy)),
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
      };
      // The playout continues the search path one move at a time, so the
      // incremental lists derive each step's full list from the same side's
      // list two moves up (the leaf's own from the grandparent's). Every
      // playout move is pushed below and popped in the unwind.
      if (follow_path && playout_depth == 0 && leaf_plays != NULL) {
        in_place = true;
        plays = leaf_plays;
        nplays = leaf_play_count + 1;
      } else if (follow_path &&
                 (worker->path_lists->length <= 2 ||
                  path_move_lists_slot_is_current(
                      worker->path_lists, worker->path_lists->length - 2))) {
        in_place = true;
        nplays =
            path_move_lists_moves_at(worker->path_lists, &pargs, &plays) + 1;
      } else {
        generate_moves(&pargs);
        nplays = worker->move_list->count;
        if (follow_path) {
          path_move_lists_set_current(worker->path_lists, worker->move_list);
        }
      }
    }

    if (nplays == 0) {
      break;
    }

    // Check for consecutive passes ending the game
    if (nplays == 1 &&
        small_move_is_pass(playout_step_play(worker->move_list, in_place, plays,
                                             nplays, &pass, 0)) &&
        game_get_consecutive_scoreless_turns(worker->game_copy) >= 1) {
      break; // would be 6 consecutive zeros
    }

    // Pick best move by adjusted score.
    // When opponent has stuck tiles and solving player is on turn,
    // prefer conservation: penalize playing many tiles / high-value tiles.
    int best_idx = 0;
    int playout_on_turn = game_get_player_on_turn_index(worker->game_copy);
    bool conserve = opp_stuck_frac > 0.0F && playout_on_turn == solving_player;

    // Pre-compute voluntary pass penalty for conservation mode.
    // By passing instead of going out, the game heads toward a 6-zero
    // ending where both players lose 1x their rack value, instead of
    // gaining 2x opponent's rack. Cost: (own_rack + opp_rack) * stuck_frac.
    int pass_penalty = 0;
    if (conserve && worker->solver->forced_pass_bypass) {
      const LetterDistribution *ld = game_get_ld(worker->game_copy);
      const Rack *own_rack =
          player_get_rack(game_get_player(worker->game_copy, playout_on_turn));
      const Rack *opp_rack = player_get_rack(
          game_get_player(worker->game_copy, 1 - playout_on_turn));
      pass_penalty =
          (int)((float)(equity_to_int(rack_get_score(ld, own_rack)) +
                        equity_to_int(rack_get_score(ld, opp_rack))) *
                opp_stuck_frac);
    }

    // Going out also earns twice the opponent's rack, which the highest-
    // scoring play can walk past. (Conservation picks keep their own
    // valuation.)
    int go_out_bonus = 0;
    if (!conserve) {
      const Rack *opp_rack = player_get_rack(
          game_get_player(worker->game_copy, 1 - playout_on_turn));
      go_out_bonus = equity_to_int(
          calculate_end_rack_points(opp_rack, game_get_ld(worker->game_copy)));
    }
    const int stm_tiles = stm_rack_p->number_of_letters;

    int best_adj = INT32_MIN;
    for (int j = 0; j < nplays; j++) {
      const SmallMove *sm = playout_step_play(worker->move_list, in_place,
                                              plays, nplays, &pass, j);
      int score = small_move_get_score(sm);
      if (go_out_bonus > 0 && !small_move_is_pass(sm) &&
          small_move_get_tiles_played(sm) == stm_tiles) {
        score += go_out_bonus;
      }
      int adj;
      if (conserve) {
        if (small_move_is_pass(sm)) {
          adj = score - pass_penalty;
        } else {
          int conservation_bonus = compute_conservation_bonus(
              sm, worker->solver->tile_face_values, opp_stuck_frac);
          adj = score - conservation_bonus;
        }
      } else {
        adj = score;
      }
      if (adj > best_adj) {
        best_adj = adj;
        best_idx = j;
      }
    }

    SmallMove best_sm = *playout_step_play(worker->move_list, in_place, plays,
                                           nplays, &pass, best_idx);
    small_move_to_move(worker->move_list->spare_move, &best_sm,
                       game_get_board(worker->game_copy));

    int undo_slot = plies + playout_depth;
    if (follow_path) {
      path_move_lists_push(worker->path_lists,
                           game_get_board(worker->game_copy),
                           worker->move_list->spare_move);
    }
    const int scoreless_before =
        game_get_consecutive_scoreless_turns(worker->game_copy);
    play_move_incremental_deferred(worker->move_list->spare_move,
                                   worker->game_copy,
                                   &worker->move_undos[undo_slot]);
    push_played_move(worker, &worker->move_undos[undo_slot]);

    // Store move in PV
    if (playout_depth < MAX_VARIANT_LENGTH) {
      pv->moves[playout_depth] = best_sm;
    }
    playout_depth++;

    // A position where the game ended has no rest to store.
    if (cache_tails &&
        game_get_game_end_reason(worker->game_copy) == GAME_END_REASON_NONE) {
      tail_key = zobrist_add_move(
          worker->solver->transposition_table->zobrist, tail_key,
          worker->move_list->spare_move,
          player_get_rack(game_get_player(worker->game_copy, playout_on_turn)),
          playout_on_turn == solving_player,
          game_get_consecutive_scoreless_turns(worker->game_copy),
          scoreless_before);
      worker->playout_tail_keys[playout_depth] = tail_key;
      worker->playout_tail_spreads[playout_depth] = equity_to_int(
          player_get_score(game_get_player(worker->game_copy, solving_player)) -
          player_get_score(
              game_get_player(worker->game_copy, 1 - solving_player)));
      const PlayoutTailEntry *tail =
          &worker->playout_tail_cache[tail_key & (PLAYOUT_TAIL_CACHE_SIZE - 1)];
      if (!pv_node && tail->key == tail_key &&
          tail->length <= max_playout - playout_depth) {
        tail_hit = true;
        hit_tail_value = tail->value;
        hit_tail_length = tail->length;
        break;
      }
    }
  }

  // Compute final spread from solving_player's perspective
  const Player *sp = game_get_player(worker->game_copy, solving_player);
  const Player *op = game_get_player(worker->game_copy, 1 - solving_player);
  int32_t greedy_spread =
      equity_to_int(player_get_score(sp) - player_get_score(op));
  const bool game_ended =
      game_get_game_end_reason(worker->game_copy) != GAME_END_REASON_NONE;
  // Whether the playout ran to its natural end, or reached a stored rest that
  // did.
  const bool natural_end =
      tail_hit || game_ended || playout_depth < max_playout;

  if (tail_hit) {
    greedy_spread =
        worker->playout_tail_spreads[playout_depth] + hit_tail_value;
  } else if (!game_ended) {
    // End-of-game rack adjustments if game didn't end naturally
    const LetterDistribution *ld = game_get_ld(worker->game_copy);
    int32_t sp_rack =
        equity_to_int(calculate_end_rack_points(player_get_rack(sp), ld));
    int32_t op_rack =
        equity_to_int(calculate_end_rack_points(player_get_rack(op), ld));
    greedy_spread -= sp_rack;
    greedy_spread += op_rack;
  }

  if (cache_tails && natural_end && !playout_interrupted) {
    const int total_length = playout_depth + hit_tail_length;
    // The position whose rest was found already holds it, and one where the
    // game ended was never keyed.
    const int last_step =
        tail_hit || game_ended ? playout_depth - 1 : playout_depth;
    for (int step = 0; step <= last_step; step++) {
      const uint64_t key = worker->playout_tail_keys[step];
      PlayoutTailEntry *entry =
          &worker->playout_tail_cache[key & (PLAYOUT_TAIL_CACHE_SIZE - 1)];
      entry->key = key;
      entry->value = greedy_spread - worker->playout_tail_spreads[step];
      entry->length = total_length - step;
    }
  }

  pv->num_moves =
      playout_depth < MAX_VARIANT_LENGTH ? playout_depth : MAX_VARIANT_LENGTH;
  pv->negamax_depth = 0;

  // Unplay all playout moves in reverse. Cross-set updates were saved into
  // each move's undo, so the square restore reverts them exactly.
  for (int d = playout_depth - 1; d >= 0; d--) {
    int undo_slot = plies + d;
    unplay_played_move(worker, &worker->move_undos[undo_slot]);
    if (follow_path) {
      path_move_lists_pop(worker->path_lists);
    }
  }

  if (playout_interrupted) {
    return ABDADA_INTERRUPTED;
  }

  // Store greedy playout result in TT at depth 0.
  // Only store if no deeper entry exists (prefer deeper negamax results).
  if (worker->solver->transposition_table_optim) {
    const uint64_t leaf_key = leaf_tt_key(worker, node_key);
    TTEntry existing = transposition_table_lookup(
        worker->solver->transposition_table, leaf_key);
    if (!ttentry_valid(existing) || ttentry_depth(existing) == 0) {
      int32_t greedy_result =
          (on_turn_idx == solving_player) ? greedy_spread : -greedy_spread;
      int16_t tt_score = (int16_t)(greedy_result - on_turn_spread);
      TTEntry entry_to_store = {.score = tt_score,
                                .flag_and_depth = (TT_EXACT << 6),
                                .tiny_move = INVALID_TINY_MOVE};
      transposition_table_store(worker->solver->transposition_table, leaf_key,
                                entry_to_store);
    }
  }

  // Convert from solving_player's perspective to on_turn perspective
  if (on_turn_idx == solving_player) {
    return greedy_spread;
  }
  return -greedy_spread;
}

// The value of a depth-0 node, or of a node where the game has ended, from the
// on-turn player's perspective: the greedy playout when heuristics are on and
// the game continues, otherwise the current spread. The root sweep uses this
// too, so the values it stores in the transposition table are exactly the ones
// depth 1 would compute.
static int32_t negamax_leaf_value(EndgameCtxWorker *worker, uint64_t node_key,
                                  int on_turn_idx, int32_t on_turn_spread,
                                  PVLine *pv, bool pv_node,
                                  float opp_stuck_frac) {
  if (worker->solver->use_heuristics &&
      game_get_game_end_reason(worker->game_copy) == GAME_END_REASON_NONE) {
    return negamax_greedy_leaf_playout(worker, node_key, on_turn_idx,
                                       on_turn_spread, pv, pv_node,
                                       opp_stuck_frac);
  }
  pv->negamax_depth = 0;
  return on_turn_spread;
}

// Compute TT flag (UPPER/LOWER/EXACT) and store entry at end of search.
static void negamax_tt_store(const EndgameCtxWorker *worker, uint64_t node_key,
                             int depth, int32_t best_value, int32_t alpha_orig,
                             int32_t beta, int32_t on_turn_spread,
                             uint64_t best_tiny_move, bool pure) {
  int16_t score = (int16_t)(best_value - on_turn_spread);
  uint8_t flag;
  TTEntry entry_to_store = {.score = score};
  if (best_value <= alpha_orig) {
    flag = TT_UPPER;
  } else if (best_value >= beta) {
    flag = TT_LOWER;
  } else {
    flag = TT_EXACT;
  }
  entry_to_store.flag_and_depth =
      (flag << 6) + (uint8_t)(pure ? TT_PURE_DEPTH : depth);
  entry_to_store.tiny_move = best_tiny_move;
  transposition_table_store(worker->solver->transposition_table, node_key,
                            entry_to_store);
}

// Sorts moves by estimated value, highest first, keeping the order of moves
// with equal values: the order stable_sort gives with
// compare_small_moves_by_estimated_value, which is the only order a stable
// sort by that key can give. Insertion sort for short runs, merged in the
// worker's scratch; the comparisons are inline.
static void sort_small_moves_range(SmallMove *moves, int count,
                                   SmallMove *scratch) {
  if (count <= SMALL_MOVE_INSERTION_SORT_MAX) {
    for (int idx = 1; idx < count; idx++) {
      const SmallMove move = moves[idx];
      const int32_t value = move.metadata.estimated_value;
      int pos = idx;
      while (pos > 0 && moves[pos - 1].metadata.estimated_value < value) {
        moves[pos] = moves[pos - 1];
        pos--;
      }
      moves[pos] = move;
    }
    return;
  }
  const int left_count = count / 2;
  sort_small_moves_range(moves, left_count, scratch);
  sort_small_moves_range(moves + left_count, count - left_count, scratch);
  if (moves[left_count - 1].metadata.estimated_value >=
      moves[left_count].metadata.estimated_value) {
    return;
  }
  memcpy(scratch, moves, sizeof(SmallMove) * (size_t)left_count);
  int left = 0;
  int right = left_count;
  int out = 0;
  while (left < left_count && right < count) {
    if (scratch[left].metadata.estimated_value >=
        moves[right].metadata.estimated_value) {
      moves[out++] = scratch[left++];
    } else {
      moves[out++] = moves[right++];
    }
  }
  while (left < left_count) {
    moves[out++] = scratch[left++];
  }
}

static void sort_small_moves_by_estimate(EndgameCtxWorker *worker,
                                         SmallMove *moves, int count) {
  if (count / 2 + 1 > worker->sort_scratch_capacity) {
    worker->sort_scratch_capacity = count;
    worker->sort_scratch =
        realloc_or_die(worker->sort_scratch, sizeof(SmallMove) * (size_t)count);
  }
  sort_small_moves_range(moves, count, worker->sort_scratch);
}

// Stuck-tile detection, move generation, logging, and estimate assignment
// for non-root nodes (move ordering itself is done lazily by the caller).
// Updates *opp_stuck_frac. Returns move count, or -1 if interrupted.
static int negamax_generate_and_estimate_moves(
    EndgameCtxWorker *worker, uint64_t node_key, int depth, uint64_t tt_move,
    float *opp_stuck_frac, const BoardOccupancy **occupancy_out) {
  int opp_idx = 1 - worker->solver->solving_player;
  uint64_t opp_tiles_bv = 0;
  int nplays;
  // compute_opp_stuck_fraction and movegen read the board; generate_stm_plays
  // then finds nothing left to update.
  update_deferred_moves(worker);
  const BoardOccupancy *occupancy = worker_board_occupancy(worker);
  *occupancy_out = occupancy;
  if (worker->solver->use_heuristics) {
    // Generate first: with the opponent on turn, the list just generated is
    // their complete play list, so its tile types give the stuck fraction
    // without a MOVE_RECORD_TILES_PLAYED generation. (The caller checked the
    // interrupt just before calling.)
    nplays = generate_stm_plays(worker, depth, occupancy);
    const SmallMove *opp_plays = NULL;
    if (game_get_player_on_turn_index(worker->game_copy) == opp_idx) {
      opp_plays = (const SmallMove *)(worker->small_move_arena->memory +
                                      worker->small_move_arena->size -
                                      (sizeof(SmallMove) * (size_t)nplays));
    }
    *opp_stuck_frac = worker_opp_stuck_fraction(
        worker, node_key, opp_plays, nplays, occupancy, &opp_tiles_bv);
    // An interrupt inside compute_opp_stuck_fraction leaves the fraction
    // meaningless. Release this node's plays so the arena is untouched, as
    // the caller expects of -1.
    if (iterative_deepening_should_stop(worker->solver)) {
      arena_dealloc(worker->small_move_arena, nplays * sizeof(SmallMove));
      return -1;
    }
  } else {
    nplays = generate_stm_plays(worker, depth, occupancy);
    *opp_stuck_frac = 0.0F;
  }

  // Log stuck-tile activation once per solve
  if (*opp_stuck_frac > 0.0F) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&worker->solver->stuck_tile_logged,
                                       &expected, 1)) {
      const Rack *opp_rack =
          player_get_rack(game_get_player(worker->game_copy, opp_idx));
      const LetterDistribution *log_ld = game_get_ld(worker->game_copy);
      int ld_size = ld_get_size(log_ld);
      StringBuilder *stuck_sb = string_builder_create();
      string_builder_add_formatted_string(stuck_sb, "Player %d (", opp_idx + 1);
      string_builder_add_rack(stuck_sb, opp_rack, log_ld, false);
      string_builder_add_string(stuck_sb, ") stuck tiles: ");
      for (int ml = 0; ml < ld_size; ml++) {
        int count = rack_get_letter(opp_rack, ml);
        if (count > 0 && !(opp_tiles_bv & ((uint64_t)1 << ml))) {
          for (int k = 0; k < count; k++) {
            string_builder_add_user_visible_letter(stuck_sb, log_ld, ml);
          }
        }
      }
      log_warn("Stuck-tile mode (%.0f%%): %s",
               (double)(*opp_stuck_frac * 100.0F),
               string_builder_peek(stuck_sb));
      string_builder_destroy(stuck_sb);
    }
  }

  assign_estimates(worker, nplays, tt_move, *opp_stuck_frac);
  return nplays;
}

// Checks whether the per-depth deadline has been exceeded and signals all
// workers to stop if so. Marked noinline to keep struct timespec off the hot
// abdada_negamax stack frame — deep searches (25-ply) would otherwise
// overflow the stack under ASAN's enlarged frames.
__attribute__((noinline)) static bool
check_depth_deadline(EndgameCtxWorker *worker) {
  // A spent movegen budget ends the solve as a deadline would: the last
  // completed depth stands.
  if (gen_movegen_budget_spent()) {
    atomic_store(&worker->solver->search_complete, 1);
    return true;
  }
  int64_t deadline_ns = atomic_load_explicit(&worker->solver->depth_deadline_ns,
                                             memory_order_relaxed);
  // External deadline (e.g. PEG's wall-clock cap) takes precedence if it
  // exists and is earlier than the EBF-projected per-depth deadline.
  int64_t external = worker->solver->external_deadline_ns;
  if (external != 0 && (deadline_ns == 0 || external < deadline_ns)) {
    deadline_ns = external;
  }
  if (deadline_ns == 0) {
    return false;
  }
  int64_t now_ns = ctimer_monotonic_ns();
  if (now_ns > deadline_ns) {
    atomic_store(&worker->solver->search_complete, 1);
    return true;
  }
  return false;
}

// Republish worker thread 0's live multi-PV top-K leaderboard with the
// (root_tiny, value, continuation) for one just-completed root move. Keeps
// entries sorted descending by value, deduped by root_tiny, capped at max_k
// (<= MAX_ENDGAME_DISPLAY_PVS). Seqlock writer: bump seq odd, mutate, bump
// even, so endgame_ctx_get_live_top_k_pvs reads a consistent snapshot.
// Atomically copy leaderboard slot `src` onto slot `dst` (relaxed loads/stores;
// the surrounding seqlock release/acquire provides ordering for readers). Only
// the live continuation prefix is copied, which is all a reader observes.
static void topk_copy_slot(EndgameCtxWorker *worker, int dst, int src) {
  atomic_store_explicit(&worker->live_top_k_root_tiny[dst],
                        atomic_load_explicit(&worker->live_top_k_root_tiny[src],
                                             memory_order_relaxed),
                        memory_order_relaxed);
  atomic_store_explicit(&worker->live_top_k_value[dst],
                        atomic_load_explicit(&worker->live_top_k_value[src],
                                             memory_order_relaxed),
                        memory_order_relaxed);
  const int cont_len = atomic_load_explicit(
      &worker->live_top_k_continuation_len[src], memory_order_relaxed);
  atomic_store_explicit(&worker->live_top_k_continuation_len[dst], cont_len,
                        memory_order_relaxed);
  for (int ply_idx = 0; ply_idx < cont_len; ply_idx++) {
    atomic_store_explicit(
        &worker->live_top_k_continuation[dst][ply_idx],
        atomic_load_explicit(&worker->live_top_k_continuation[src][ply_idx],
                             memory_order_relaxed),
        memory_order_relaxed);
  }
}

static void publish_live_top_k_pv(EndgameCtxWorker *worker, uint64_t root_tiny,
                                  int32_t value,
                                  const struct PVLine *continuation,
                                  int max_k) {
  if (max_k < 1) {
    return;
  }
  if (max_k > MAX_ENDGAME_DISPLAY_PVS) {
    max_k = MAX_ENDGAME_DISPLAY_PVS;
  }
  // Begin seqlock write window: bump seq odd.
  const uint64_t seq_before =
      atomic_load_explicit(&worker->live_top_k_seq, memory_order_relaxed);
  atomic_store_explicit(&worker->live_top_k_seq, seq_before + 1,
                        memory_order_release);

  int filled =
      atomic_load_explicit(&worker->live_top_k_filled, memory_order_relaxed);

  // Remove existing entry for same root_tiny so we can re-insert sorted.
  for (int pv_idx = 0; pv_idx < filled; pv_idx++) {
    if (atomic_load_explicit(&worker->live_top_k_root_tiny[pv_idx],
                             memory_order_relaxed) == root_tiny) {
      for (int shift_idx = pv_idx; shift_idx < filled - 1; shift_idx++) {
        topk_copy_slot(worker, shift_idx, shift_idx + 1);
      }
      filled--;
      break;
    }
  }

  // Find insertion position (descending order by value).
  int insert_pos = filled;
  for (int pv_idx = 0; pv_idx < filled; pv_idx++) {
    if (value > atomic_load_explicit(&worker->live_top_k_value[pv_idx],
                                     memory_order_relaxed)) {
      insert_pos = pv_idx;
      break;
    }
  }

  if (insert_pos < max_k) {
    const int last = (filled < max_k) ? filled : (max_k - 1);
    for (int slot_idx = last; slot_idx > insert_pos; slot_idx--) {
      topk_copy_slot(worker, slot_idx, slot_idx - 1);
    }
    atomic_store_explicit(&worker->live_top_k_root_tiny[insert_pos], root_tiny,
                          memory_order_relaxed);
    atomic_store_explicit(&worker->live_top_k_value[insert_pos], value,
                          memory_order_relaxed);
    int cont_len = (continuation != NULL) ? continuation->num_moves : 0;
    if (cont_len > MAX_SEARCH_DEPTH) {
      cont_len = MAX_SEARCH_DEPTH;
    }
    for (int ply_idx = 0; ply_idx < cont_len; ply_idx++) {
      atomic_store_explicit(
          &worker->live_top_k_continuation[insert_pos][ply_idx],
          continuation->moves[ply_idx].tiny_move, memory_order_relaxed);
    }
    atomic_store_explicit(&worker->live_top_k_continuation_len[insert_pos],
                          cont_len, memory_order_relaxed);
    if (filled < max_k) {
      filled++;
    }
    atomic_store_explicit(&worker->live_top_k_filled, filled,
                          memory_order_relaxed);
  }

  // End seqlock write window: bump seq even.
  atomic_store_explicit(&worker->live_top_k_seq, seq_before + 2,
                        memory_order_release);
}

// Plays small_move on the worker's game copy, recording the undo in *undo,
// and returns the child node's zobrist key (0 when the transposition table is
// disabled). As a side effect worker->move_list->spare_move is loaded with the
// Move form of small_move, which callers use for logging and display.
//
// Shared by the negamax root/child move loop and the multi-PV resolve pass so
// the outplay handling stays in one place: outplays use
// play_move_endgame_outplay, which deliberately does NOT empty the rack, so
// the mover's rack still holds the full pre-move rack afterwards.
// zobrist_add_move treats its rack argument as the post-move leftover, which
// for an outplay is empty; passing the stale full rack double-counts the
// played tiles (placeholder = placed + full = 2*placed) and can index the rack
// hash table out of bounds -- e.g. outplaying four S's yields placeholder[S] =
// 8 into an 8-slot (0..RACK_SIZE) row. Use an empty leftover for outplays.
static uint64_t play_small_move_and_hash(EndgameCtxWorker *worker,
                                         const SmallMove *small_move,
                                         uint64_t node_key, MoveUndo *undo) {
  Game *worker_game = worker->game_copy;
  const int on_turn_idx = game_get_player_on_turn_index(worker_game);
  const Player *player_on_turn = game_get_player(worker_game, on_turn_idx);
  const Rack *stm_rack = player_get_rack(player_on_turn);
  const bool is_outplay = small_move_get_tiles_played(small_move) ==
                          rack_get_total_letters(stm_rack);

  small_move_to_move(worker->move_list->spare_move, small_move,
                     game_get_board(worker_game));
  if (worker->path_lists != NULL) {
    path_move_lists_push(worker->path_lists, game_get_board(worker_game),
                         worker->move_list->spare_move);
  }

  Rack outplay_leftover;
  if (is_outplay) {
    rack_set_dist_size_and_reset(&outplay_leftover,
                                 rack_get_dist_size(stm_rack));
  }
  const Rack *move_leftover_rack = is_outplay ? &outplay_leftover : stm_rack;

  const int last_consecutive_scoreless_turns =
      game_get_consecutive_scoreless_turns(worker_game);

  if (is_outplay) {
    play_move_endgame_outplay(worker->move_list->spare_move, worker_game, undo);
  } else {
    // The child often returns from the transposition table without looking
    // at the board, so its anchors and cross-sets wait until something reads
    // them.
    play_move_incremental_deferred(worker->move_list->spare_move, worker_game,
                                   undo);
  }
  push_played_move(worker, undo);

  uint64_t child_key = 0;
  if (worker->solver->transposition_table_optim) {
    child_key =
        zobrist_add_move(worker->solver->transposition_table->zobrist, node_key,
                         worker->move_list->spare_move, move_leftover_rack,
                         on_turn_idx == worker->solver->solving_player,
                         game_get_consecutive_scoreless_turns(worker_game),
                         last_consecutive_scoreless_turns);
  }
  return child_key;
}

// A node one ply above the leaves nearly always cuts off on its first move,
// yet building its move list (and estimating every move) costs far more than
// searching that move. So it first tries a likely refutation found without the
// list: with no stuck tiles, the best play by score, going out first, as the
// move ordering ranks going out first. The incremental lists find it
// (path_move_lists_best_play), or without them a MOVE_RECORD_BEST_SMALL
// generation does; both pick the same play, so the search does not depend on
// which ran. Only if that move does not cut off is the list built. (The node's
// own list slot then stays unbuilt, which is safe only because its children
// are leaves: nothing below derives from it.) Picks that move and
// returns true, or returns false when the node should build its list first: a
// stuck opponent (the ordering then weighs conservation), an interrupt, or no
// play at all.
static bool pick_first_reply_before_list(EndgameCtxWorker *worker,
                                         uint64_t node_key,
                                         float *opp_stuck_frac,
                                         SmallMove *first_reply) {
  update_deferred_moves(worker);
  if (worker->solver->use_heuristics) {
    const float stuck_frac = worker_opp_stuck_fraction(
        worker, node_key, NULL, 0, worker_board_occupancy(worker), NULL);
    if (stuck_frac > 0.0F || iterative_deepening_should_stop(worker->solver)) {
      return false;
    }
  }
  *opp_stuck_frac = 0.0F;
  const int stm_idx = game_get_player_on_turn_index(worker->game_copy);
  const MoveGenArgs best_args = {
      .best_small_out_bonus = int_to_equity(FIRST_REPLY_GOING_OUT_PRIORITY),
      .best_small_canonical_ties = true,
      .game = worker->game_copy,
      .move_list = worker->move_list,
      .move_record_type = MOVE_RECORD_BEST_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = solver_get_pruned_kwg(worker->solver, stm_idx),
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .lane_shadow_cache = worker->lane_shadow_cache,
  };
  if (worker->path_lists != NULL) {
    return path_move_lists_best_play(worker->path_lists, &best_args,
                                     first_reply);
  }
  generate_moves(&best_args);
  const SmallMove *best = worker->move_list->small_moves[0];
  if (small_move_is_pass(best)) {
    return false;
  }
  *first_reply = *best;
  return true;
}

int32_t abdada_negamax(EndgameCtxWorker *worker, uint64_t node_key, int depth,
                       int32_t alpha, int32_t beta, PVLine *pv, bool pv_node,
                       bool exclusive_p, float opp_stuck_frac) {

  assert(pv_node || alpha == beta - 1);

  // Live-progress: count this node visit in the per-thread local counter.
  // The published-to-shared atomic is updated alongside the deadline check
  // below, batching ~DEPTH_DEADLINE_CHECK_INTERVAL increments into one
  // atomic_store. Reader sees an upper-bound-stale view, which is fine for
  // a "is the engine making progress" UI.
  worker->local_nodes_searched++;

  if (iterative_deepening_should_stop(worker->solver)) {
    return ABDADA_INTERRUPTED;
  }

  // Per-depth deadline check: bail mid-depth if we're running over the EBF
  // estimate. Throttled to every DEPTH_DEADLINE_CHECK_INTERVAL nodes per worker
  // to amortize the clock_gettime call. The actual check is in a noinline
  // helper so struct timespec does not land on every abdada_negamax frame —
  // deep searches (e.g. 25-ply) would otherwise overflow the stack.
  if (++worker->nodes_since_deadline_check >= DEPTH_DEADLINE_CHECK_INTERVAL) {
    worker->nodes_since_deadline_check = 0;
    // Flush per-thread node count to the shared atomic at the same
    // cadence as the deadline check, so the live-progress getter sees
    // updates ~once per DEPTH_DEADLINE_CHECK_INTERVAL nodes per worker.
    atomic_store_explicit(&worker->published_nodes_searched,
                          worker->local_nodes_searched, memory_order_relaxed);
    if (check_depth_deadline(worker)) {
      return ABDADA_INTERRUPTED;
    }
  }

  // ABDADA: if exclusive search and another processor is on this node, defer.
  // Active from ABDADA_MIN_DEPTH up. Decided once per node: a helper that
  // arrives mid-node does not change it, so every enter_node below is matched
  // by a leave_node.
  const bool abdada_active = depth >= ABDADA_MIN_DEPTH &&
                             worker->solver->transposition_table_optim &&
                             worker_has_company(worker);
  if (exclusive_p && abdada_active) {
    if (transposition_table_is_busy(worker->solver->transposition_table,
                                    node_key)) {
      return ON_EVALUATION;
    }
  }

  // ABDADA: mark that we're searching this node
  if (abdada_active) {
    transposition_table_enter_node(worker->solver->transposition_table,
                                   node_key);
  }

  int32_t alpha_orig = alpha;
  const uint64_t impure_at_entry = worker->impure_events;

  int on_turn_idx = game_get_player_on_turn_index(worker->game_copy);
  const Player *player_on_turn =
      game_get_player(worker->game_copy, on_turn_idx);
  const Player *other_player =
      game_get_player(worker->game_copy, 1 - on_turn_idx);
  int on_turn_spread = equity_to_int(player_get_score(player_on_turn) -
                                     player_get_score(other_player));
  uint64_t tt_move = INVALID_TINY_MOVE;

  if (worker->solver->transposition_table_optim) {
    // A depth-0 node's entry is its leaf value, stored under leaf_tt_key.
    const uint64_t probe_key =
        depth == 0 ? leaf_tt_key(worker, node_key) : node_key;
    TTEntry tt_entry = transposition_table_lookup(
        worker->solver->transposition_table, probe_key);
    // A pass leaves the board and racks as they were, so a position can recur
    // with fewer plies left: the moves A, Z, B and pass, Z, A, pass, B reach
    // the same position, scoreless count included, the second with two fewer
    // plies left. Helpers searching a shallower depth also meet the entries
    // that threads searching deeper stored. An entry stored deeper than this
    // node holds a deeper search's value, and which of those a search meets
    // depends on what other threads stored first, so only its move is used. A
    // pure entry is the game's value at any depth.
    if (ttentry_valid(tt_entry) && ttentry_depth(tt_entry) > (uint8_t)depth &&
        ttentry_depth(tt_entry) != TT_PURE_DEPTH) {
      tt_move = ttentry_move(tt_entry);
    } else if (ttentry_valid(tt_entry) &&
               ttentry_depth(tt_entry) >= (uint8_t)depth) {
      if (ttentry_depth(tt_entry) != TT_PURE_DEPTH) {
        worker->impure_events++;
      }
      int16_t score = ttentry_score(tt_entry);
      uint8_t flag = ttentry_flag(tt_entry);
      // add spread back in; we subtract it when storing.
      score = (int16_t)((int16_t)on_turn_spread + score);
      if (flag == TT_EXACT) {
        if (!pv_node) {
          // ABDADA: leave node before returning
          if (abdada_active) {
            transposition_table_leave_node(worker->solver->transposition_table,
                                           node_key);
          }
          return score;
        }
      } else if (flag == TT_LOWER) {
        alpha = MAX(alpha, score);
      } else if (flag == TT_UPPER) {
        beta = MIN(beta, score);
      }
      if (alpha >= beta) {
        // The stored bound proves this node cannot improve the caller's
        // window, so return it at PV nodes too. Continuing the search here
        // with alpha >= beta gave every child an inverted window; a child's
        // beta cutoff then also satisfied best_value <= alpha_orig and was
        // stored as an upper bound, and a later PV re-search that trusted
        // that bound reported a wrong root value. A root fail is widened and
        // re-searched by iterative_deepening like any other aspiration fail.
        // ABDADA: leave node before returning
        if (abdada_active) {
          transposition_table_leave_node(worker->solver->transposition_table,
                                         node_key);
        }
        return score;
      }
      assert(alpha < beta);
      // search hash move first
      tt_move = ttentry_move(tt_entry);
    }
  }

  if (depth == 0 ||
      game_get_game_end_reason(worker->game_copy) != GAME_END_REASON_NONE) {
    if (game_get_game_end_reason(worker->game_copy) == GAME_END_REASON_NONE) {
      worker->impure_events++;
    }
    // ABDADA: leave node before returning
    if (abdada_active) {
      transposition_table_leave_node(worker->solver->transposition_table,
                                     node_key);
    }
    return negamax_leaf_value(worker, node_key, on_turn_idx, on_turn_spread, pv,
                              pv_node, opp_stuck_frac);
  }

  PVLine child_pv;
  child_pv.game = worker->game_copy;
  child_pv.num_moves = 0;
  child_pv.negamax_depth = 0;

  // One ply above the leaves, try the likely refutation before building the
  // move list (see pick_first_reply_before_list), unless
  // EndgameArgs.disable_best_reply_shortcuts is set. Only where the ordering
  // has nothing better to offer: no hash move, no early pass, more than one
  // tile (the single-tile shortcuts need the list), and not under thread 0's
  // first root move, whose ply-2 progress counts the list.
  bool first_reply_searched = false;
  // Which squares hold tiles, once this node's moves are generated.
  const BoardOccupancy *occupancy = NULL;
  SmallMove first_reply;
  int32_t first_reply_value = 0;
  if (depth == 1 && worker->solver->best_reply_shortcuts &&
      worker->current_iterative_deepening_depth != depth &&
      tt_move == INVALID_TINY_MOVE &&
      game_get_consecutive_scoreless_turns(worker->game_copy) == 0 &&
      rack_get_total_letters(player_get_rack(player_on_turn)) >= 2 &&
      !(worker->current_iterative_deepening_depth - 1 == depth &&
        worker->ordinal == 0 && worker->in_first_root_move) &&
      pick_first_reply_before_list(worker, node_key, &opp_stuck_frac,
                                   &first_reply)) {
    const int undo_index = worker->solver->requested_plies - depth;
    atomic_store_explicit(&worker->current_line[undo_index],
                          first_reply.tiny_move, memory_order_relaxed);
    atomic_store_explicit(&worker->current_line_len, undo_index + 1,
                          memory_order_release);
    const uint64_t child_key = play_small_move_and_hash(
        worker, &first_reply, node_key, &worker->move_undos[undo_index]);
    const int32_t value =
        abdada_negamax(worker, child_key, depth - 1, -beta, -alpha, &child_pv,
                       pv_node, false, opp_stuck_frac);
    unplay_played_move(worker, &worker->move_undos[undo_index]);
    if (worker->path_lists != NULL) {
      path_move_lists_pop(worker->path_lists);
    }
    atomic_store_explicit(&worker->current_line_len, undo_index,
                          memory_order_release);
    if (value == ABDADA_INTERRUPTED) {
      return ABDADA_INTERRUPTED;
    }
    first_reply_value = -value;
    first_reply_searched = true;
    if (first_reply_value >= beta) {
      pvline_update(pv, &child_pv, &first_reply,
                    first_reply_value - worker->solver->initial_spread);
      pv->negamax_depth = child_pv.negamax_depth + 1;
      if (worker->solver->transposition_table_optim) {
        negamax_tt_store(worker, node_key, depth, first_reply_value, alpha_orig,
                         beta, on_turn_spread, first_reply.tiny_move,
                         worker->impure_events == impure_at_entry);
      }
      return first_reply_value;
    }
  }

  int nplays;
  bool arena_alloced = false;
  if (worker->current_iterative_deepening_depth != depth) {
    // Check interrupt before entering expensive movegen.  A thread that just
    // passed the check at the top of this function may have had the interrupt
    // fire during the TT-lookup / spread-calculation window above; catching it
    // here avoids running two full movegens unnecessarily.
    if (iterative_deepening_should_stop(worker->solver)) {
      if (abdada_active) {
        transposition_table_leave_node(worker->solver->transposition_table,
                                       node_key);
      }
      return ABDADA_INTERRUPTED;
    }
    nplays = negamax_generate_and_estimate_moves(
        worker, node_key, depth, tt_move, &opp_stuck_frac, &occupancy);
    if (nplays == -1) {
      // Interrupted during compute_opp_stuck_fraction
      if (abdada_active) {
        transposition_table_leave_node(worker->solver->transposition_table,
                                       node_key);
      }
      return ABDADA_INTERRUPTED;
    }
    arena_alloced = true;
  } else {
    // Use initial moves. They already have been sorted by estimated value.
    nplays = worker->n_initial_moves;
  }

  // Forced-pass fast path: when the only legal move is pass,
  // play it without consuming a depth ply. Since a forced pass is
  // deterministic (no branching), it shouldn't cost search depth.
  // Guard: only when previous move scored (consecutive_scoreless_turns == 0)
  // to avoid non-termination in mutual-pass endgames.
  if (worker->solver->forced_pass_bypass && arena_alloced && nplays == 1 &&
      game_get_consecutive_scoreless_turns(worker->game_copy) == 0) {
    size_t fp_offset = worker->small_move_arena->size - sizeof(SmallMove);
    const SmallMove *only_move =
        (const SmallMove *)(worker->small_move_arena->memory + fp_offset);
    if (small_move_is_pass(only_move)) {
      // Save move data before arena may be reallocated during recursion
      SmallMove pass_move = *only_move;
      const Board *board = game_get_board(worker->game_copy);
      small_move_to_move(worker->move_list->spare_move, &pass_move, board);

      int last_consecutive_scoreless_turns =
          game_get_consecutive_scoreless_turns(worker->game_copy);

      if (worker->path_lists != NULL) {
        path_move_lists_push(worker->path_lists, board,
                             worker->move_list->spare_move);
      }
      play_move_incremental(worker->move_list->spare_move, worker->game_copy,
                            &worker->pass_undos[depth]);
      push_played_move(worker, &worker->pass_undos[depth]);

      uint64_t child_key = 0;
      if (worker->solver->transposition_table_optim) {
        const Rack *stm_rack = player_get_rack(player_on_turn);
        child_key = zobrist_add_move(
            worker->solver->transposition_table->zobrist, node_key,
            worker->move_list->spare_move, stm_rack,
            on_turn_idx == worker->solver->solving_player,
            game_get_consecutive_scoreless_turns(worker->game_copy),
            last_consecutive_scoreless_turns);
      }

      // Recurse at SAME depth (forced pass doesn't consume depth)
      child_pv.num_moves = 0;
      int32_t value = abdada_negamax(worker, child_key, depth, -beta, -alpha,
                                     &child_pv, pv_node, false, opp_stuck_frac);

      unplay_played_move(worker, &worker->pass_undos[depth]);
      if (worker->path_lists != NULL) {
        path_move_lists_pop(worker->path_lists);
      }

      arena_dealloc(worker->small_move_arena, sizeof(SmallMove));

      if (abdada_active) {
        transposition_table_leave_node(worker->solver->transposition_table,
                                       node_key);
      }

      if (value == ABDADA_INTERRUPTED) {
        return ABDADA_INTERRUPTED;
      }

      int32_t pass_best = -value;
      pvline_update(pv, &child_pv, &pass_move,
                    pass_best - worker->solver->initial_spread);
      pv->negamax_depth = child_pv.negamax_depth + 1;

      if (worker->solver->transposition_table_optim) {
        negamax_tt_store(worker, node_key, depth, pass_best, alpha_orig, beta,
                         on_turn_spread, pass_move.tiny_move,
                         worker->impure_events == impure_at_entry);
      }

      return pass_best;
    }
  }

  int32_t best_value = -LARGE_VALUE;
  uint64_t best_tiny_move = INVALID_TINY_MOVE;
  if (first_reply_searched) {
    // The first reply did not cut off: it stands as the best so far, and the
    // list's copy of it is skipped below.
    best_value = first_reply_value;
    best_tiny_move = first_reply.tiny_move;
    pvline_update(pv, &child_pv, &first_reply,
                  best_value - worker->solver->initial_spread);
    pv->negamax_depth = child_pv.negamax_depth + 1;
    alpha = MAX(alpha, best_value);
    pvline_clear(&child_pv);
  }
  size_t arena_offset =
      worker->small_move_arena->size - (sizeof(SmallMove) * nplays);

  // Analytical leaf shortcut for single-tile outplay.
  // When stm has exactly 1 tile and the only legal move is a non-pass, it
  // plays that tile and ends the game immediately. Skip the board mutation
  // cycle (small_move_to_move + play_move + child recursion + unplay).
  if (arena_alloced && nplays == 1) {
    const SmallMove *only_sm =
        (const SmallMove *)(worker->small_move_arena->memory + arena_offset);
    const Rack *stm_rack_a = player_get_rack(player_on_turn);
    if (!small_move_is_pass(only_sm) &&
        rack_get_total_letters(stm_rack_a) == 1) {
      const LetterDistribution *ld = game_get_ld(worker->game_copy);
      int32_t move_score = (int32_t)small_move_get_score(only_sm);
      int32_t opp_bonus = equity_to_int(
          calculate_end_rack_points(player_get_rack(other_player), ld));
      int32_t leaf_value = on_turn_spread + move_score + opp_bonus;

      pvline_update(pv, &child_pv, only_sm,
                    leaf_value - worker->solver->initial_spread);
      pv->negamax_depth = 1;

      if (worker->solver->transposition_table_optim) {
        negamax_tt_store(worker, node_key, depth, leaf_value, alpha_orig, beta,
                         on_turn_spread, only_sm->tiny_move,
                         worker->impure_events == impure_at_entry);
      }

      arena_dealloc(worker->small_move_arena, sizeof(SmallMove));

      if (abdada_active) {
        transposition_table_leave_node(worker->solver->transposition_table,
                                       node_key);
      }

      return leaf_value;
    }
  }

  // Multi-PV: track top-K values at root to widen alpha
  const bool is_root = (worker->current_iterative_deepening_depth == depth);
  const bool is_ply2 =
      (worker->current_iterative_deepening_depth - 1 == depth) &&
      worker->ordinal == 0 && worker->in_first_root_move;
  if (is_ply2) {
    atomic_store(&worker->solver->ply2_moves_total, nplays);
    atomic_store(&worker->solver->ply2_moves_completed, 0);
  }
  const int multi_pv_k = worker->solver->num_top_moves;
  const bool multi_pv = is_root && multi_pv_k > 1;
  // Multi-PV, actual-move extraction, and reporting callbacks expose values
  // for multiple root moves, so
  // those callers need full-width searches at the root. A top-1 solve without
  // either callback only needs the best move: later roots can use the normal
  // PVS null-window probe and are re-searched at full width only if they
  // improve alpha.
  const bool exact_all_root_moves =
      is_root && solver_needs_exact_root_values(worker->solver);
  // Sized for the live multi-PV leaderboard breadth (up to
  // MAX_ENDGAME_DISPLAY_PVS root moves), not the per-line depth. num_top_moves
  // is clamped to MAX_ENDGAME_DISPLAY_PVS in endgame_ctx_reset so topk_insert
  // (called with k == multi_pv_k) never writes past this array.
  int32_t topk_values[MAX_ENDGAME_DISPLAY_PVS];
  int topk_n = 0;
  // See adopt_unfinished_depth_best. Its first root move must be the last
  // completed depth's best, which an actual move pinned to the front is not,
  // and the first-win and initial-window windows answer other questions.
  const bool track_unfinished_best =
      is_root && !multi_pv && worker->solver->actual_move == NULL &&
      !worker->solver->first_win_optim && !worker->solver->initial_window_optim;
  bool root_move_completed = false;

  // ABDADA: track deferred moves for second phase
  // Use stack allocation when possible to avoid malloc overhead
  bool deferred_stack[MAX_DEFERRED_STACK];
  bool *deferred = NULL;
  bool deferred_heap_allocated = false;
  // Deferrals are tracked only where the children can be deferred.
  bool use_abdada = abdada_active && depth - 1 >= ABDADA_MIN_DEPTH;
  if (use_abdada) {
    if (nplays <= MAX_DEFERRED_STACK) {
      deferred = deferred_stack;
    } else {
      deferred = malloc_or_die(sizeof(bool) * nplays);
      deferred_heap_allocated = true;
    }
    for (int i = 0; i < nplays; i++) {
      deferred[i] = false;
    }
  }

  // See keep_one_bound. A move that leaves one tile at or below alpha needs
  // no search; the opponent's reply is searched (depth >= 2).
  KeepOneBound keep_one;
  const bool keep_one_active =
      arena_alloced && depth >= 2 && !is_root &&
      rack_get_total_letters(player_get_rack(player_on_turn)) >= 2;
  if (keep_one_active) {
    keep_one_bound_init(&keep_one, worker->game_copy, on_turn_spread,
                        occupancy);
  }

  // ABDADA two-phase iteration:
  // Pass 0: search all moves with exclusion (defer busy nodes).
  // Pass 1+: retry only deferred moves without exclusion.
  int pass = 0;
  bool all_done = false;
  // Freshly generated moves are unsorted (assign_estimates does not sort).
  // Order them lazily: selection-pick the best remaining move into slot idx
  // for the first LAZY_SELECTION_LIMIT indices, then sort the tail once if
  // the node gets that far without a cutoff. Most nodes cut off within the
  // first few moves and never pay for a full sort. Root move lists
  // (!arena_alloced) arrive pre-sorted.
  bool tail_sorted = !arena_alloced;
  while (!all_done) {
    all_done = true;

    for (int idx = 0; idx < nplays; idx++) {
      // After the first pass, skip moves that aren't deferred
      // (they were already successfully searched)
      if (pass > 0 && deferred != NULL && !deferred[idx]) {
        continue;
      }

      // Lazy move ordering, pass 0 only: later passes revisit positions
      // recorded in deferred[], so the array must not be reordered then.
      if (!tail_sorted && pass == 0) {
        SmallMove *moves_base =
            (SmallMove *)(worker->small_move_arena->memory + arena_offset);
        if (idx < LAZY_SELECTION_LIMIT) {
          int best_est_idx = idx;
          int32_t best_est = small_move_get_estimated_value(&moves_base[idx]);
          for (int scan = idx + 1; scan < nplays; scan++) {
            const int32_t est =
                small_move_get_estimated_value(&moves_base[scan]);
            if (est > best_est) {
              best_est = est;
              best_est_idx = scan;
            }
          }
          if (best_est_idx != idx) {
            SmallMove tmp = moves_base[idx];
            moves_base[idx] = moves_base[best_est_idx];
            moves_base[best_est_idx] = tmp;
          }
        } else {
          sort_small_moves_by_estimate(worker, moves_base + idx, nplays - idx);
          tail_sorted = true;
        }
      }

      // ABDADA: determine if this move should be searched exclusively
      // First move (idx == 0) is never exclusive
      // In first phase, other moves are exclusive
      // In retry phase (deferred[idx] == true), moves are not exclusive
      bool child_exclusive = use_abdada && (idx > 0) && !deferred[idx];

      size_t element_offset = arena_offset + idx * sizeof(SmallMove);
      SmallMove *small_move =
          (SmallMove *)(worker->small_move_arena->memory + element_offset);
      if (first_reply_searched &&
          small_move->tiny_move == first_reply.tiny_move) {
        continue;
      }
      if (keep_one_active && !small_move_is_pass(small_move) &&
          small_move_get_tiles_played(small_move) == keep_one.rack_size - 1) {
        const int32_t bound = keep_one_bound(&keep_one, small_move, alpha);
        if (bound <= alpha) {
          if (deferred != NULL) {
            deferred[idx] = false;
          }
          best_value = MAX(best_value, bound);
          continue;
        }
      }

      // Track whether thread 0 is inside root move #1's subtree
      if (is_root && worker->ordinal == 0 && pass == 0) {
        worker->in_first_root_move = (idx == 0);
      }

      // Calculate undo index for incremental backup
      int undo_index = worker->solver->requested_plies - depth;

      // Live-progress: publish this move into the per-thread "current line"
      // buffer before recursing so a polling reader can see what the engine
      // is currently exploring even during a long single-root subtree where
      // no other signal updates. The slot store is a relaxed atomic; the
      // length store uses release ordering so the reader (acquire on length)
      // sees the tiny_move write.
      atomic_store_explicit(&worker->current_line[undo_index],
                            small_move->tiny_move, memory_order_relaxed);
      atomic_store_explicit(&worker->current_line_len, undo_index + 1,
                            memory_order_release);

      const uint64_t child_key = play_small_move_and_hash(
          worker, small_move, node_key, &worker->move_undos[undo_index]);

      // Per-root-move aspiration: at root after depth 1, each move gets its
      // own aspiration window centered on its estimated_value from the previous
      // ID iteration. This gives accurate values for all root moves (needed for
      // multi-PV) while still benefiting from narrow windows.
      const bool use_root_aspiration =
          exact_all_root_moves && depth >= 2 &&
          worker->solver->iterative_deepening_optim &&
          !worker->solver->first_win_optim &&
          !worker->solver->initial_window_optim;

      int32_t value = 0;
      if (use_root_aspiration) {
        int32_t est = small_move_get_estimated_value(small_move);
        int32_t window = ASPIRATION_WINDOW;
        int32_t move_alpha = MAX(alpha, est - window);
        int32_t move_beta = MIN(beta, est + window);
        // Ensure valid window
        if (move_alpha >= move_beta) {
          move_alpha = alpha;
          move_beta = beta;
        }

        while (true) {
          value = abdada_negamax(worker, child_key, depth - 1, -move_beta,
                                 -move_alpha, &child_pv, pv_node,
                                 child_exclusive, opp_stuck_frac);
          if (value == ON_EVALUATION || value == ABDADA_INTERRUPTED) {
            break;
          }
          if (-value <= move_alpha && move_alpha > alpha) {
            // Aspiration fail-low: widen alpha toward global alpha
            window *= 2;
            move_alpha =
                (window >= LARGE_VALUE / 2) ? alpha : MAX(alpha, est - window);
            pvline_clear(&child_pv);
          } else if (-value >= move_beta && move_beta < beta) {
            // Aspiration fail-high: widen beta toward global beta
            window *= 2;
            move_beta =
                (window >= LARGE_VALUE / 2) ? beta : MIN(beta, est + window);
            pvline_clear(&child_pv);
          } else {
            break;
          }
        }
      } else if ((idx == 0 && !first_reply_searched) ||
                 !worker->solver->negascout_optim || exact_all_root_moves) {
        value =
            abdada_negamax(worker, child_key, depth - 1, -beta, -alpha,
                           &child_pv, pv_node, child_exclusive, opp_stuck_frac);
      } else {
        value =
            abdada_negamax(worker, child_key, depth - 1, -alpha - 1, -alpha,
                           &child_pv, false, child_exclusive, opp_stuck_frac);
        if (value != ABDADA_INTERRUPTED && value != ON_EVALUATION &&
            alpha < -value && -value < beta) {
          // re-search with wider window (not exclusive since we need the value)
          value = abdada_negamax(worker, child_key, depth - 1, -beta, -alpha,
                                 &child_pv, pv_node, false, opp_stuck_frac);
        }
      }
      unplay_played_move(worker, &worker->move_undos[undo_index]);
      if (worker->path_lists != NULL) {
        path_move_lists_pop(worker->path_lists);
      }
      // Cross-sets need no recompute here: any lazy cross-set update in the
      // child's subtree was saved into this undo (or a descendant's undo that
      // was already restored), so the square restore reverted them exactly.

      // Live-progress: pop the line back to the parent's depth. The entry at
      // undo_index is left as-is (overwritten by the next sibling's push).
      // Release pairs with the reader's acquire on length.
      atomic_store_explicit(&worker->current_line_len, undo_index,
                            memory_order_release);

      if (value == ABDADA_INTERRUPTED) {
        all_done = true;
        best_value = ABDADA_INTERRUPTED;
        break;
      }

      // ABDADA: check if move was deferred
      if (value == ON_EVALUATION) {
        if (deferred != NULL) {
          deferred[idx] = true;
        }
        all_done = false;
        continue; // Skip to next move
      }

      // Mark as not deferred (in case we're in second phase)
      if (deferred != NULL) {
        deferred[idx] = false;
      }

      // Re-assign small_move. Its pointer location may have changed after all
      // the calls to negamax and possible reallocations in the
      // small_move_arena.
      small_move =
          (SmallMove *)(worker->small_move_arena->memory + element_offset);
      // alpha is at least every earlier root move's value, so a later root
      // move that comes back above it is better than all of them.
      const bool beats_earlier_roots =
          track_unfinished_best && root_move_completed && -value > alpha;
      root_move_completed = true;
      if (-value > best_value) {
        best_value = -value;
        best_tiny_move = small_move->tiny_move;
        pvline_update(pv, &child_pv, small_move,
                      best_value - worker->solver->initial_spread);
        pv->negamax_depth = child_pv.negamax_depth + 1;
        if (beats_earlier_roots) {
          worker->unfinished_best_pv = *pv;
        }
        // Live PV publish (root only): a polling reader sees the engine's
        // evolving best line refine through each depth as root moves get
        // evaluated. Seqlock pattern: bump seq odd to signal "writing", write
        // moves+value+length, bump seq even to signal "consistent". Reader
        // retries on odd / mismatched seq.
        if (is_root) {
          int new_len = pv->num_moves;
          if (new_len > MAX_SEARCH_DEPTH) {
            new_len = MAX_SEARCH_DEPTH;
          }
          const uint64_t seq_before =
              atomic_load_explicit(&worker->live_pv_seq, memory_order_relaxed);
          atomic_store_explicit(&worker->live_pv_seq, seq_before + 1,
                                memory_order_release);
          for (int ply_idx = 0; ply_idx < new_len; ply_idx++) {
            atomic_store_explicit(&worker->live_pv_tiny_moves[ply_idx],
                                  pv->moves[ply_idx].tiny_move,
                                  memory_order_relaxed);
          }
          atomic_store_explicit(&worker->live_pv_length, new_len,
                                memory_order_relaxed);
          atomic_store_explicit(&worker->live_pv_value,
                                best_value - worker->solver->initial_spread,
                                memory_order_relaxed);
          atomic_store_explicit(&worker->live_pv_seq, seq_before + 2,
                                memory_order_release);
        }
      }
      if (is_root) {
        // At the very top depth, set the estimated value of the small move,
        // for the next iterative deepening iteration.
        small_move_set_estimated_value(small_move, -value);
        // Track root move progress (thread 0 only, for external polling).
        // Count on any pass so ABDADA deferred moves that complete on pass 1+
        // are not missed.
        if (worker->ordinal == 0) {
          atomic_fetch_add(&worker->solver->root_moves_completed, 1);
          // Spread-adjusted value, same sign convention as PVLine.score.
          const int32_t reported_value =
              -value - worker->solver->initial_spread;
          if (worker->solver->per_root_move_callback) {
            worker->solver->per_root_move_callback(
                depth, idx, small_move, reported_value,
                worker->solver->per_root_move_callback_data);
          }
          // Live multi-PV: republish the top-K leaderboard with this root's
          // evaluation. Covers all root completions (including ABDADA pass-2
          // deferred re-evaluations), so the displayed leaderboard always
          // reflects the most recent value seen for each root move at the
          // current depth.
          publish_live_top_k_pv(worker, small_move->tiny_move, reported_value,
                                &child_pv, worker->solver->num_top_moves);
        }
      }
      if (is_ply2) {
        atomic_fetch_add(&worker->solver->ply2_moves_completed, 1);
      }
      if (multi_pv) {
        // Multi-PV: insert value into sorted top-K array and set alpha to
        // the Kth-best value so top K moves all get full-width searches.
        int32_t kth = topk_insert(topk_values, &topk_n, multi_pv_k, -value);
        alpha = MAX(alpha_orig, kth);
      } else {
        alpha = MAX(alpha, best_value);
      }
      if (best_value >= beta) {
        // beta cut-off
        all_done = true;
        break;
      }
      // clear the child node's pv for the next child node
      pvline_clear(&child_pv);
    }

    // Single-threaded mode: only one pass needed
    if (!use_abdada) {
      all_done = true;
    }

    pass++;

    // ABDADA: yield when we still have deferred moves so other threads
    // can make progress.
    if (!all_done) {
      compat_sched_yield();
    }
  }

  // Clean up deferred array (only if heap-allocated)
  if (deferred_heap_allocated) {
    free(deferred);
  }

  if (worker->solver->transposition_table_optim &&
      best_value != ABDADA_INTERRUPTED) {
    negamax_tt_store(worker, node_key, depth, best_value, alpha_orig, beta,
                     on_turn_spread, best_tiny_move,
                     worker->impure_events == impure_at_entry);
  }

  if (arena_alloced) {
    arena_dealloc(worker->small_move_arena, nplays * sizeof(SmallMove));
  }

  // ABDADA: leave node before returning
  if (abdada_active) {
    transposition_table_leave_node(worker->solver->transposition_table,
                                   node_key);
  }

  return best_value;
}

// Depth-0 fallback: before the IDS loop runs depth 1, greedy-evaluate the
// root moves in thread 0's static-estimate order and publish the best as a
// depth-0 result. If the IDS is interrupted before depth 1 completes even
// once, the caller gets the best greedy play among the moves the sweep
// reached instead of the default "no result / pass / value=0".
//
// Every worker takes part, claiming moves from a shared index, so a short
// budget reaches as many of the top moves as possible; each worker offers its
// best line and the results keep the highest. Each move is played exactly as
// the depth-1 search plays a root move (same undo slot, same path push) and
// valued by the same leaf evaluation (negamax_leaf_value), so a playout is
// stored under the child's key: depth 1 then finds it in the transposition
// table instead of playing it out again.
//
// first_win_optim only needs win/loss, not the best move, and its depth-1
// search is cheap, so sweeping all root candidates (often 1000+ at a 7-tile
// endgame root) is almost pure overhead. Cap it to the top few moves by
// estimate (the moves are already sorted) so the interrupt fallback is still
// present but ~100x cheaper. The result is unchanged whenever IDS completes
// (the common case), since depth-1+ overwrites the depth-0 value.
static void root_greedy_sweep(EndgameCtxWorker *worker, uint64_t root_key,
                              int initial_move_count) {
  EndgameCtx *solver = worker->solver;
  if (worker->ordinal == 0) {
    // first_win_fallback_moves: 0 = use FIRST_WIN_D0_FALLBACK_MOVES default,
    // <0 = skip the sweep, >0 = explicit cap.
    int fw_fallback_cap = FIRST_WIN_D0_FALLBACK_MOVES;
    if (solver->first_win_fallback_moves > 0) {
      fw_fallback_cap = solver->first_win_fallback_moves;
    } else if (solver->first_win_fallback_moves < 0) {
      fw_fallback_cap = 0; // skip the sweep entirely
    }
    const int count = solver->first_win_optim
                          ? MIN(initial_move_count, fw_fallback_cap)
                          : initial_move_count;
    if (count > solver->root_sweep_capacity) {
      free(solver->root_sweep_moves);
      solver->root_sweep_moves =
          malloc_or_die(sizeof(SmallMove) * (size_t)count);
      solver->root_sweep_capacity = count;
    }
    if (count > 0) {
      memcpy(solver->root_sweep_moves, worker->small_move_arena->memory,
             sizeof(SmallMove) * (size_t)count);
    }
    atomic_store_explicit(&solver->root_sweep_count, count,
                          memory_order_release);
  }
  // Other workers generate their own roots concurrently; wait for thread 0's
  // order, which is only a root generation away.
  int count;
  while ((count = atomic_load_explicit(&solver->root_sweep_count,
                                       memory_order_acquire)) < 0) {
    if (iterative_deepening_should_stop(solver)) {
      return;
    }
    compat_sched_yield();
  }

  const int undo_index = solver->requested_plies - 1;
  int32_t best_root_value = -LARGE_VALUE;
  bool have_root_best = false;
  PVLine d0_pv;
  d0_pv.game = worker->game_copy;
  d0_pv.num_moves = 0;
  d0_pv.negamax_depth = 0;
  // Each playout can take tens of microseconds and no worker is in negamax
  // yet, so check the caller's deadline per move here.
  while (!iterative_deepening_should_stop(solver) &&
         !check_depth_deadline(worker)) {
    const int move_idx = atomic_fetch_add(&solver->root_sweep_next, 1);
    if (move_idx >= count) {
      break;
    }
    const SmallMove sm = solver->root_sweep_moves[move_idx];
    const uint64_t child_key = play_small_move_and_hash(
        worker, &sm, root_key, &worker->move_undos[undo_index]);
    // Leaf value from the perspective of the post-move player on turn.
    const int post_on_turn = game_get_player_on_turn_index(worker->game_copy);
    const int32_t post_on_turn_spread = equity_to_int(
        player_get_score(game_get_player(worker->game_copy, post_on_turn)) -
        player_get_score(game_get_player(worker->game_copy, 1 - post_on_turn)));
    PVLine child_pv;
    child_pv.game = worker->game_copy;
    child_pv.num_moves = 0;
    child_pv.negamax_depth = 0;
    const int32_t leaf_val =
        negamax_leaf_value(worker, child_key, post_on_turn, post_on_turn_spread,
                           &child_pv, /*pv_node=*/true, 0.0F);
    unplay_played_move(worker, &worker->move_undos[undo_index]);
    if (worker->path_lists != NULL) {
      path_move_lists_pop(worker->path_lists);
    }
    if (leaf_val == ABDADA_INTERRUPTED) {
      break;
    }
    // Negamax convention: the root mover's value for this move = -leaf_val.
    const int32_t root_val = -leaf_val;
    if (root_val > best_root_value) {
      best_root_value = root_val;
      d0_pv.moves[0] = sm;
      for (int j = 0; j < child_pv.num_moves && j + 1 < MAX_VARIANT_LENGTH;
           j++) {
        d0_pv.moves[j + 1] = child_pv.moves[j];
      }
      d0_pv.num_moves = (child_pv.num_moves + 1 < MAX_VARIANT_LENGTH)
                            ? child_pv.num_moves + 1
                            : MAX_VARIANT_LENGTH;
      have_root_best = true;
    }
  }
  if (have_root_best) {
    d0_pv.score = best_root_value - solver->initial_spread;
    endgame_results_offer_best_pvline(solver->results, &d0_pv, d0_pv.score, 0);
  }
}

static bool iterative_deepening_should_stop(EndgameCtx *solver) {
  return atomic_load(&solver->search_complete) != 0 ||
         thread_control_search_checkpoint(solver->thread_control) ==
             THREAD_CONTROL_STATUS_USER_INTERRUPT;
}

// Zobrist hash of the root position as the search sees it: solving player to
// move, no move played yet. Returns 0 when the TT is disabled.
static uint64_t solver_root_hash_key(const EndgameCtx *solver,
                                     const Game *game_copy) {
  if (!solver->transposition_table_optim) {
    return 0;
  }
  const Player *solving_player =
      game_get_player(game_copy, solver->solving_player);
  const Player *other_player =
      game_get_player(game_copy, 1 - solver->solving_player);
  return zobrist_calculate_hash(
      solver->transposition_table->zobrist, game_get_board(game_copy),
      player_get_rack(solving_player), player_get_rack(other_player), false,
      game_get_consecutive_scoreless_turns(game_copy));
}

// Prepare solver->ext_game (lazy init or reset) then extend pv_line from TT.
// Consolidates the repeated "prepare ext_game, then call pvline_extend_from_tt"
// pattern used at all solver call sites.
static inline void solver_pvline_extend_from_tt(PVLine *pv_line,
                                                EndgameCtx *solver,
                                                const Game *source_game) {
  // The extension scores moves from the copied board's cross-scores.
  assert(board_get_cross_sets_valid(game_get_board(source_game)));
  if (!solver->ext_game) {
    solver->ext_game = game_duplicate(source_game);
  } else {
    game_copy(solver->ext_game, source_game);
  }
  game_set_endgame_solving_mode(solver->ext_game);
  pvline_extend_from_tt(pv_line, solver->ext_game, solver->transposition_table,
                        solver->solving_player, solver->requested_plies);
}

// Build top-K ranked PVLines from root SmallMoves (with TT extension) and
// invoke per_ply_callback. Display/callback plumbing only.
static void build_ranked_pvs_and_notify(EndgameCtxWorker *worker, int depth,
                                        int32_t pv_value,
                                        const PVLine *extended_pv,
                                        const SmallMove *initial_moves,
                                        int initial_move_count) {
  int n_ranked = initial_move_count < MAX_RANKED_CALLBACK_PVS
                     ? initial_move_count
                     : MAX_RANKED_CALLBACK_PVS;
  PVLine ranked_pvs[MAX_RANKED_CALLBACK_PVS];
  // Seed each ranked PV from its root SmallMove first, while the arena is
  // stable. solver_pvline_extend_from_tt below runs a greedy playout that can
  // grow (realloc) small_move_arena, so it must not run while we still need to
  // read initial_moves — otherwise a later iteration would index a freed
  // buffer. Splitting the loops keeps all initial_moves reads before any
  // possible realloc.
  for (int r = 0; r < n_ranked; r++) {
    PVLine *rpv = &ranked_pvs[r];
    rpv->moves[0] = initial_moves[r];
    rpv->num_moves = 1;
    rpv->score = small_move_get_estimated_value(&initial_moves[r]) -
                 worker->solver->initial_spread;
    rpv->negamax_depth = 1;
    rpv->game = NULL;
  }
  if (worker->solver->transposition_table_optim) {
    for (int r = 0; r < n_ranked; r++) {
      solver_pvline_extend_from_tt(&ranked_pvs[r], worker->solver,
                                   worker->game_copy);
    }
  }

  worker->solver->per_ply_callback(depth, pv_value, extended_pv,
                                   worker->game_copy, ranked_pvs, n_ranked,
                                   worker->solver->per_ply_callback_data);
}

// Only the main worker calls the per-ply callback, and a helper that completes
// the target depth first ends the solve with the main worker partway through
// it, so the callback never heard of the deepest depth searched, often the
// target. Called once every worker is joined, this reports the deepest
// completed depth if the main worker did not.
static void notify_unreported_deepest_depth(EndgameCtx *solver, int best_thread,
                                            int best_depth) {
  if (solver->per_ply_callback == NULL ||
      best_depth <= solver->workers[0]->completed_depth) {
    return;
  }
  EndgameCtxWorker *best_worker = solver->workers[best_thread];
  PVLine extended_pv = best_worker->best_pv;
  if (solver->transposition_table_optim) {
    solver_pvline_extend_from_tt(&extended_pv, solver, best_worker->game_copy);
  }
  build_ranked_pvs_and_notify(
      best_worker, best_depth, best_worker->best_pv_value, &extended_pv,
      (const SmallMove *)best_worker->small_move_arena->memory,
      best_worker->n_initial_moves);
}

// If the solver has a caller-specified "actual move" (EndgameArgs.actual_move),
// find it among the just-(re)sorted root moves and swap it to index 0.
// The root move loop in abdada_negamax always gives index 0 a full,
// unnarrowed [alpha, beta] window (nothing has run yet to raise alpha), the
// same property multi-PV's top-K alpha capping gives the top-ranked moves.
// Keeping the actual move pinned to index 0 through every IDS depth is what
// lets extract_actual_move_pvline read an exact value for it later, without
// a second solve. Root moves are re-sorted by estimated value after every
// completed depth (see iterative_deepening), so this must be called again
// after each re-sort, not just once.
static void force_actual_move_to_front(const EndgameCtxWorker *worker,
                                       SmallMove *root_moves, int n_root) {
  const EndgameCtx *solver = worker->solver;
  if (!solver->actual_move || n_root <= 1) {
    return;
  }
  const Board *board = game_get_board(worker->game_copy);
  Move candidate;
  for (int idx = 0; idx < n_root; idx++) {
    small_move_to_move(&candidate, &root_moves[idx], board);
    if (endgame_moves_match(&candidate, solver->actual_move)) {
      if (idx != 0) {
        SmallMove tmp = root_moves[0];
        root_moves[0] = root_moves[idx];
        root_moves[idx] = tmp;
      }
      return;
    }
  }
}

// Under a deadline, most of a solve's time goes to the depth the deadline cuts
// short. That depth searches the last completed depth's best root move first,
// so a root move that then comes back above alpha is better at the deeper
// depth, and the solve returns it with its line and value. The reported depth
// stays the last completed one: callers read it to tell whether the value is
// exact, and this one comes from a depth not every root move finished.
static void adopt_unfinished_depth_best(const EndgameCtxWorker *worker) {
  if (worker->unfinished_best_pv.num_moves == 0 ||
      worker->completed_depth < 1) {
    return;
  }
  endgame_results_replace_best_pvline(
      worker->solver->results, &worker->unfinished_best_pv,
      worker->unfinished_best_pv.score, worker->completed_depth);
}

void iterative_deepening(EndgameCtxWorker *worker, int plies) {

  int32_t alpha = -LARGE_VALUE;
  int32_t beta = LARGE_VALUE;
  int32_t prev_value = 0;
  Timer ids_timer;
  ctimer_start(&ids_timer);
  double prev_depth_time = 0.0;
  double prev_prev_depth_time = 0.0;
  double ema_ebf_per_ply = -1.0; // -1 = not yet seeded
  // Soft-limit stability tracking: record the pv_value the first time elapsed
  // crosses soft_time_limit.  On each subsequent crossing, stop only if the
  // value hasn't changed — result is stable and we can bank the remaining time.
  // INT32_MIN is the sentinel meaning "not yet crossed the soft limit".
  int32_t soft_limit_pv_value = INT32_MIN;
  // Single-thread multi-PV/per-root reporting already gives every root move a
  // private aspiration window in abdada_negamax.  Top-1 searches use PVS at
  // the root, so a narrow window around the preceding IDS value can prune the
  // whole tree without sacrificing an exact result (failures are widened and
  // re-searched below).
  const bool use_aspiration = worker->solver->threads > 1 ||
                              (worker->solver->num_top_moves == 1 &&
                               !solver_needs_exact_root_values(worker->solver));

  if (worker->solver->first_win_optim) {
    // search a very small window centered around 0; we're just trying to find
    // something that surpasses it. This is useful to find "any win", and for
    // a pre-endgame solver.
    alpha = -1;
    beta = 1;
  } else if (worker->solver->initial_window_optim) {
    // Caller-provided fixed window: the search proves how the exact value
    // relates to [initial_alpha, initial_beta] instead of computing it.
    alpha = worker->solver->initial_window_alpha;
    beta = worker->solver->initial_window_beta;
  }
  assert(worker->small_move_arena->size == 0); // make sure arena is empty.

  const uint64_t initial_hash_key =
      solver_root_hash_key(worker->solver, worker->game_copy);

  // Half the threads use stuck-tile-aware root ordering (build chains +
  // conservation), the other half use score-based ordering for diversity.
  float initial_opp_stuck_frac = (worker->ordinal % 2 == 0)
                                     ? worker->solver->initial_opp_stuck_frac
                                     : 0.0F;

  if (worker->path_lists != NULL) {
    path_move_lists_reset(worker->path_lists);
  }
  int initial_move_count =
      generate_stm_plays(worker, worker->solver->requested_plies, NULL);
  if (worker->path_lists != NULL) {
    seed_opponent_root_list(worker);
  }
  // Arena pointer better have started at 0, since it was empty.
  assign_estimates_and_sort(worker, initial_move_count, INVALID_TINY_MOVE,
                            initial_opp_stuck_frac);
  worker->n_initial_moves = initial_move_count;
  assert((size_t)worker->small_move_arena->size ==
         initial_move_count * sizeof(SmallMove));
  force_actual_move_to_front(worker,
                             (SmallMove *)worker->small_move_arena->memory,
                             initial_move_count);

  // Publish the d=0 root-move list to clients before any depth begins.
  // root_moves_total is bumped here (rather than only at depth-loop entry) so
  // the polled progress atomics agree with the callback's view of the
  // candidate count even before depth-1 starts. Main worker (ordinal 0)
  // only; the arena holds the sorted moves contiguously from offset 0.
  if (worker->ordinal == 0) {
    atomic_store(&worker->solver->root_moves_total, initial_move_count);
    if (worker->solver->before_search_callback) {
      const SmallMove *initial_moves =
          (const SmallMove *)worker->small_move_arena->memory;
      worker->solver->before_search_callback(
          worker->game_copy, initial_moves, initial_move_count,
          worker->solver->initial_spread, worker->solver->solving_player,
          worker->solver->before_search_callback_data);
    }
  }

  const bool steps_by_two = solver_steps_depth_by_two(worker->solver, plies);
  worker->current_iterative_deepening_depth = 1;
  int start = 1;
  if (!worker->solver->iterative_deepening_optim) {
    start = plies;
  }

  root_greedy_sweep(worker, initial_hash_key, initial_move_count);

  // ABDADA depth jitter: spread threads across different starting depths
  // so they don't all compete on the same shallow levels simultaneously.
  // Thread 0 always starts at depth 1 (main thread, provides per-ply callback).
  // Other threads start at depth 1 + (index % depth_spread), clamped to plies.
  // That includes helpers lent to a single-threaded solve (endgame_add_worker),
  // which have ordinals from 1 up although threads stays 1.
  // plies > 1 guards the `% (plies - 1)` below: a 1-ply solve has only depth 1,
  // so there is no range to spread across (and `% 0` is a divide-by-zero that
  // traps as SIGFPE on x86).
  if (worker->ordinal > 0 && plies > 1) {
    // Spread across up to 4 different starting depths to reduce contention.
    // With 16 threads: 1 at d1 (thread 0), ~4 at d2, ~4 at d3, ~4 at d4, ~3 at
    // d5
    int depth_offset = 1 + (worker->ordinal % MIN(4, plies - 1));
    start = MIN(depth_offset, plies);
  }

  for (int ply = start; ply <= plies; ply++) {
    if (steps_by_two && iterative_deepening_skips_ply(ply, plies)) {
      continue;
    }
    // Check if another thread has completed the full search
    if (iterative_deepening_should_stop(worker->solver)) {
      break;
    }

    worker->current_iterative_deepening_depth = ply;
    worker->unfinished_best_pv.num_moves = 0;
    // Update root move progress counters (main worker, ordinal 0, only)
    if (worker->ordinal == 0) {
      atomic_store(&worker->solver->current_depth, ply);
      atomic_store(&worker->solver->root_moves_completed, 0);
      atomic_store(&worker->solver->root_moves_total, worker->n_initial_moves);
      atomic_store(&worker->solver->ply2_moves_completed, 0);
      atomic_store(&worker->solver->ply2_moves_total, 0);
      // Live multi-PV: clear the top-K leaderboard so it reflects the new
      // depth's evaluations only. Brief seqlock window; a reader sees
      // filled=0 momentarily, then watches entries fill in as roots complete.
      const uint64_t topk_seq_before =
          atomic_load_explicit(&worker->live_top_k_seq, memory_order_relaxed);
      atomic_store_explicit(&worker->live_top_k_seq, topk_seq_before + 1,
                            memory_order_release);
      atomic_store_explicit(&worker->live_top_k_filled, 0,
                            memory_order_relaxed);
      atomic_store_explicit(&worker->live_top_k_seq, topk_seq_before + 2,
                            memory_order_release);
      worker->in_first_root_move = false;
    }
    double depth_start_time = ctimer_elapsed_seconds(&ids_timer);
    PVLine pv;
    pv.game = worker->game_copy;
    pv.num_moves = 0;
    pv.negamax_depth = 0;

    int32_t val;

    // Track whether this depth's search completed validly
    bool search_valid = true;

    // Use aspiration windows once this worker has searched at least one
    // prior depth — otherwise prev_value is uninitialized and the window
    // is bogus. With ABDADA depth-jitter, threads with thread_index > 0
    // can start at depth > 1, so checking `ply > 1` is insufficient.
    uint64_t impure_before_root = 0;
    if (use_aspiration && ply > start && !worker->solver->first_win_optim &&
        !worker->solver->initial_window_optim) {
      int32_t window = ASPIRATION_WINDOW;
      alpha = prev_value - window;
      beta = prev_value + window;

      // Search with narrow window, widen on fail-high/fail-low
      while (true) {
        // Check if another thread completed
        if (iterative_deepening_should_stop(worker->solver)) {
          search_valid = false;
          break;
        }

        impure_before_root = worker->impure_events;
        val = abdada_negamax(worker, initial_hash_key, ply, alpha, beta, &pv,
                             true, false, initial_opp_stuck_frac);

        if (val <= alpha) {
          // Fail-low: widen alpha
          alpha = (window >= LARGE_VALUE / 2) ? -LARGE_VALUE
                                              : prev_value - window * 2;
          window *= 2;
        } else if (val >= beta) {
          // Fail-high: widen beta
          beta = (window >= LARGE_VALUE / 2) ? LARGE_VALUE
                                             : prev_value + window * 2;
          window *= 2;
        } else {
          // Value is within window, we're done
          break;
        }

        // Reset PV for re-search
        pv.num_moves = 0;
      }
    } else {
      // Full window search for depth 1 or when aspiration disabled
      impure_before_root = worker->impure_events;
      val = abdada_negamax(worker, initial_hash_key, ply, alpha, beta, &pv,
                           true, false, initial_opp_stuck_frac);
    }

    // An interrupted depth stands only through a root move it proved better.
    bool interrupted = !search_valid || val == ABDADA_INTERRUPTED;
    if (interrupted) {
      adopt_unfinished_depth_best(worker);
      break;
    }

    // No heuristic leaf and no impure TT hit anywhere in this root search: the
    // value is the game's exact minimax value, so deeper iterations would
    // return the same thing.
    const bool root_pure = worker->impure_events == impure_before_root;
    prev_value = val;

    // sort initial moves by valuation for next time.
    SmallMove *initial_moves = (SmallMove *)(worker->small_move_arena->memory);
    stable_sort(initial_moves, initial_move_count, sizeof(SmallMove),
                compare_small_moves_by_estimated_value);
    force_actual_move_to_front(worker, initial_moves, initial_move_count);

    // Store result in worker's local tracking
    int32_t pv_value = val - worker->solver->initial_spread;
    worker->best_pv_value = pv_value;
    pv.score = pv_value;
    worker->best_pv = pv;
    worker->completed_depth = ply;

    // A pure root search proves the value at every depth, so it is reported
    // at the requested depth.
    endgame_results_set_best_pvline(worker->solver->results, &pv, pv_value,
                                    root_pure ? plies : ply);

    // Call per-ply callback (only this solver's main worker, ordinal 0, to
    // avoid race conditions).
    if (worker->ordinal == 0 && worker->solver->per_ply_callback) {
      // Extend PV from TT + greedy playout for display
      PVLine extended_pv = pv;
      if (worker->solver->transposition_table_optim) {
        solver_pvline_extend_from_tt(&extended_pv, worker->solver,
                                     worker->game_copy);
      }

      build_ranked_pvs_and_notify(worker, ply, pv_value, &extended_pv,
                                  initial_moves, initial_move_count);
    }

    // EBF-based time management: decide whether to start the next depth.
    // Only thread 0 checks (other threads follow via search_complete signal).
    // Require a minimum depth before applying any time management so the solver
    // always produces a reasonably deep answer before EBF/soft limit can stop
    // it — prevents shallow-depth artifacts (e.g. spurious PASS at d3).
    // min_depth=4: the d1/d3 EBF ratio is unreliable because the cross-set
    // precheck prunes very heavily at shallow depths, making d1 anomalously
    // fast. Starting at d4 (which uses the d2/d4 ratio) gives a stable base.
    const int min_depth_for_time_mgmt = 4;
    // EBF time management is active only when soft_time_limit > 0.
    // Callers using only hard_time_limit rely on the external timer thread
    // and check_depth_deadline instead; soft_time_limit = 0 disables this
    // block intentionally (e.g. baseline time_mode=0 via endgame_time.c).
    if (worker->ordinal == 0 && worker->solver->soft_time_limit > 0) {
      double elapsed = ctimer_elapsed_seconds(&ids_timer);
      double this_depth_time = elapsed - depth_start_time;
      if (ply >= min_depth_for_time_mgmt) {
        if (elapsed >= worker->solver->soft_time_limit) {
          if (pv_value == soft_limit_pv_value) {
            // Result unchanged since last soft-limit crossing — stable.
            // Bank the remaining time for future turns.
            atomic_store(&worker->solver->search_complete, 1);
            break;
          }
          // First crossing or result still changing: record value and fall
          // through to the hard-limit check so the next depth can still run
          // if the EBF estimate says it fits.
          soft_limit_pv_value = pv_value;
        }
        // Estimate time for next depth using 2-ply EBF: sqrt(t[d]/t[d-2]).
        // More stable than 1-ply EBF across odd/even ply alternation.
        // Apply EMA smoothing: the cross-set precheck produces irregular search
        // trees at shallow depths (heavy pruning early, less so later), making
        // instantaneous EBF estimates volatile. EMA blends the current sample
        // with the historical estimate, smoothing out transient spikes.
        if (prev_prev_depth_time > 0.01 && this_depth_time > 0.01) {
          double ebf2 = this_depth_time / prev_prev_depth_time;
          double ebf_per_ply = sqrt(ebf2);
          const double ema_alpha = 0.4;
          if (ema_ebf_per_ply < 0.0) {
            ema_ebf_per_ply = ebf_per_ply; // seed on first valid sample
          } else {
            ema_ebf_per_ply =
                ema_alpha * ebf_per_ply + (1.0 - ema_alpha) * ema_ebf_per_ply;
          }
          double estimated_next = this_depth_time * ema_ebf_per_ply;
          if (elapsed + estimated_next > worker->solver->hard_time_limit) {
            // Next depth would likely exceed hard limit — stop now
            atomic_store(&worker->solver->search_complete, 1);
            break;
          }
          // Mid-depth bail: set a per-depth deadline so workers abort if a
          // depth runs far beyond its EBF estimate.  Only activate in the
          // final stretch (elapsed >= 75% of hard limit).  Below that
          // threshold the solver still has many depths ahead of it and the
          // EBF estimate is likely noisy at shallow depths; bailing early
          // would drop back to a shallower result unnecessarily.  Above 75%,
          // we are near the last depth we will attempt, so a runaway depth
          // directly starves subsequent turns.  Clear any stale deadline when
          // we are below the threshold so workers don't fire spuriously.
          if (elapsed >= 0.75 * worker->solver->hard_time_limit) {
            int64_t now_ns = ctimer_monotonic_ns();
            int64_t budget_ns = (int64_t)(estimated_next * 1.5e9);
            atomic_store(&worker->solver->depth_deadline_ns,
                         now_ns + budget_ns);
          } else {
            atomic_store(&worker->solver->depth_deadline_ns, 0);
          }
        }
      }
      prev_prev_depth_time = prev_depth_time;
      prev_depth_time = this_depth_time;
    }

    // Signal other threads to stop when we complete the full search
    if (ply == plies || root_pure) {
      atomic_store(&worker->solver->search_complete, 1);
    }
  }

  // Final flush: this worker has stopped searching, so publish its exact
  // node count. The periodic in-search flush only fires every
  // DEPTH_DEADLINE_CHECK_INTERVAL nodes, so without this the tail of every
  // worker (and all of a very short solve) would be permanently
  // under-reported by up to DEPTH_DEADLINE_CHECK_INTERVAL-1 nodes. All
  // exit paths of the loop above fall through to here.
  atomic_store_explicit(&worker->published_nodes_searched,
                        worker->local_nodes_searched, memory_order_relaxed);
}

void *solver_worker_start(void *uncasted_solver_worker) {
  EndgameCtxWorker *solver_worker = (EndgameCtxWorker *)uncasted_solver_worker;
  const EndgameCtx *solver = solver_worker->solver;
  (void)gen_set_movegen_budget(solver->movegen_budget);
  iterative_deepening(solver_worker, solver->requested_plies);
  (void)gen_set_movegen_budget(NULL);
  return NULL;
}

// Closes a pass opened by display_pass_begin, leaving the search stopped as
// the main search left it.
static void display_pass_end(EndgameCtx *solver) {
  atomic_store(&solver->depth_deadline_ns, 0);
  atomic_store(&solver->search_complete, 1);
}

// Opens a pass that searches again after the main search, to solve the lines
// the display shows. The main search set search_complete to signal the worker
// threads to stop; clear it so this pass is not immediately interrupted. A
// user interrupt (and PEG's external_deadline_ns) still stops it.
//
// Under a time limit, the pass gets only what is left of the budget: a solve
// that stopped because the next depth would not fit must not then overrun the
// clock solving display lines. Whatever is unsolved when the deadline fires
// falls back to TT extension. Unlimited solves (the plain endgame command's
// default) run the pass to completion. Returns false, with the search left
// stopped, when no time is left.
static bool display_pass_begin(EndgameCtx *solver) {
  int64_t deadline_ns = 0;
  if (solver->hard_time_limit > 0) {
    // Read the timer itself, not the cached seconds_elapsed: that field is
    // only refreshed when something renders status, so a scripted solve that
    // never polls would see 0 here and hand this pass a fresh full budget.
    const double remaining =
        solver->hard_time_limit -
        endgame_results_get_timer_elapsed_seconds(solver->results);
    if (remaining <= 0) {
      display_pass_end(solver);
      return false;
    }
    deadline_ns = ctimer_monotonic_ns() + (int64_t)(remaining * 1e9);
  }
  atomic_store(&solver->depth_deadline_ns, deadline_ns);
  atomic_store(&solver->search_complete, 0);
  return true;
}

// Whether the search played move, the next move of a line it searched, as a
// forced pass: with EndgameArgs.forced_pass_bypass, a pass below the root by
// a side with no other play, after a move that scored, which the search plays
// without spending a ply. The worker's game is at the move's position, which
// is not the root.
static bool pv_move_is_forced_pass(const EndgameCtx *solver,
                                   EndgameCtxWorker *worker,
                                   const SmallMove *move, int plies_left) {
  if (!solver->forced_pass_bypass || !small_move_is_pass(move) ||
      game_get_consecutive_scoreless_turns(worker->game_copy) != 0) {
    return false;
  }
  const int nplays = generate_stm_plays(worker, plies_left, NULL);
  arena_dealloc(worker->small_move_arena, nplays * sizeof(SmallMove));
  return nplays == 1;
}

// The search's line can stop short of the depth it was solved to. A node on
// the line whose window a transposition-table bound narrowed can take its best
// move from a null-window probe that it does not re-search, and the probe's
// line ends at the first node it answers from the TT. Extending such a line
// from the TT would mark moves the search proved as unsearched. Instead,
// searches from where the line stops with the plies that remain, and again
// from wherever that search's line stops, so the line is searched to depth
// plies, or to the end of the game, and never deeper. A search that is
// interrupted, or whose value disagrees with the line's, leaves the rest of
// the line to TT extension. A forced pass (see pv_move_is_forced_pass) spends
// no ply here, as in the search. The worker's game must be at the root
// position.
static void complete_pv_line(const EndgameCtx *solver, EndgameCtxWorker *worker,
                             int depth, PVLine *pv) {
  if (!solver->transposition_table_optim || pv->negamax_depth == 0) {
    return;
  }
  // No node of these searches is the root, and none is ply-2 progress for the
  // (already finished) first root move.
  worker->current_iterative_deepening_depth = depth;
  worker->in_first_root_move = false;
  // A node's incremental move list derives from its parent's, and the walk
  // down the line builds none, so these searches generate their moves in full.
  PathMoveLists *path_lists = worker->path_lists;
  worker->path_lists = NULL;

  // The solving player's final spread, as the line's value.
  const int32_t line_value = pv->score + solver->initial_spread;
  const int first_undo_index = solver->requested_plies - depth;
  uint64_t node_key = solver_root_hash_key(solver, worker->game_copy);
  MoveUndo *played_undos[MAX_VARIANT_LENGTH];
  int num_played = 0;
  int plies_used = 0;
  while (true) {
    while (num_played < pv->negamax_depth && plies_used < depth) {
      const SmallMove *move = &pv->moves[num_played];
      const int plies_left = depth - plies_used;
      // The search plays the move at a node with d plies left into
      // move_undos[requested_plies - d], and a forced pass, which leaves d as
      // it is, into pass_undos[d]. Played into the same undos, the line's
      // moves leave a search from its end the undos below theirs.
      MoveUndo *undo = NULL;
      if (num_played > 0 &&
          pv_move_is_forced_pass(solver, worker, move, plies_left)) {
        undo = &worker->pass_undos[plies_left];
      } else {
        undo = &worker->move_undos[first_undo_index + plies_used];
        plies_used++;
      }
      node_key = play_small_move_and_hash(worker, move, node_key, undo);
      played_undos[num_played] = undo;
      num_played++;
    }
    // A line of forced passes can fill the PV before it spends its plies.
    if (plies_used >= depth || num_played >= MAX_VARIANT_LENGTH ||
        game_get_game_end_reason(worker->game_copy) != GAME_END_REASON_NONE) {
      break;
    }
    PVLine rest_pv;
    rest_pv.game = worker->game_copy;
    rest_pv.num_moves = 0;
    rest_pv.negamax_depth = 0;
    const int32_t value =
        abdada_negamax(worker, node_key, depth - plies_used, -LARGE_VALUE,
                       LARGE_VALUE, &rest_pv, true, false, 0.0F);
    if (value == ABDADA_INTERRUPTED || value == ON_EVALUATION) {
      break;
    }
    const bool solving_player_on_turn =
        game_get_player_on_turn_index(worker->game_copy) ==
        solver->solving_player;
    if ((solving_player_on_turn ? value : -value) != line_value ||
        rest_pv.negamax_depth == 0) {
      break;
    }
    int rest_moves = rest_pv.num_moves;
    if (rest_moves > MAX_VARIANT_LENGTH - num_played) {
      rest_moves = MAX_VARIANT_LENGTH - num_played;
    }
    memcpy(&pv->moves[num_played], rest_pv.moves,
           sizeof(SmallMove) * (size_t)rest_moves);
    pv->num_moves = num_played + rest_moves;
    pv->negamax_depth = MIN(num_played + rest_pv.negamax_depth, pv->num_moves);
  }
  while (num_played > 0) {
    num_played--;
    unplay_played_move(worker, played_undos[num_played]);
  }
  worker->path_lists = path_lists;
}

// Re-searches a single root move to full depth with a full (-LARGE_VALUE,
// LARGE_VALUE) window and records the exact principal variation beneath it.
// The main search only tracks a full PV for the one best root move; every
// other line would otherwise be the root move plus a transposition-table
// probe and a greedy playout, which is not a solved variation. Because the
// TT is warm from the main search, this re-search is cheap: only the PV
// chain is re-expanded, every other node cuts off on a TT hit.
//
// Returns the root move's exact negamax value (spread-unadjusted, so callers
// can compare with SmallMove estimated values), or ABDADA_INTERRUPTED if the
// search was stopped. On success, out_pv holds the full line with
// negamax_depth set to the number of exactly searched moves.
static int32_t resolve_root_move_pv(const EndgameCtx *solver,
                                    EndgameCtxWorker *worker,
                                    const SmallMove *root_move, int depth,
                                    uint64_t root_key, PVLine *out_pv) {
  Game *worker_game = worker->game_copy;
  const int undo_index = solver->requested_plies - depth;
  const uint64_t child_key = play_small_move_and_hash(
      worker, root_move, root_key, &worker->move_undos[undo_index]);

  PVLine child_pv;
  child_pv.game = worker_game;
  child_pv.num_moves = 0;
  child_pv.negamax_depth = 0;
  const int32_t value =
      abdada_negamax(worker, child_key, depth - 1, -LARGE_VALUE, LARGE_VALUE,
                     &child_pv, true, false, 0.0F);
  unplay_played_move(worker, &worker->move_undos[undo_index]);
  if (worker->path_lists != NULL) {
    path_move_lists_pop(worker->path_lists);
  }

  if (value == ABDADA_INTERRUPTED || value == ON_EVALUATION) {
    return ABDADA_INTERRUPTED;
  }
  const int32_t root_value = -value;
  pvline_update(out_pv, &child_pv, root_move,
                root_value - solver->initial_spread);
  out_pv->negamax_depth = child_pv.negamax_depth + 1;
  out_pv->game = NULL;
  complete_pv_line(solver, worker, depth, out_pv);
  return root_value;
}

// Inserts a resolved PV line into the descending-by-score leaderboard
// multi_pvs[0..num_lines-1]. Slot 0 starts out holding the search's own best
// line and normally stays there: the sift-up below only swaps on a strictly
// greater score, so a resolved line that merely ties it cannot displace it.
// A strictly greater resolved score does displace it, which is possible
// because results BEST is only overwritten on a strictly deeper depth while
// this pass re-searches against the final (possibly deeper-seeded) TT; the
// display must stay sorted rather than show line 2 beating line 1.
// *filled is the number of occupied slots, including slot 0.
static void insert_resolved_pv(PVLine *multi_pvs, int *filled, int num_lines,
                               const PVLine *resolved) {
  if (*filled < num_lines) {
    multi_pvs[*filled] = *resolved;
    (*filled)++;
  } else if (resolved->score > multi_pvs[num_lines - 1].score) {
    multi_pvs[num_lines - 1] = *resolved;
  } else {
    return;
  }
  for (int slot_idx = *filled - 1;
       slot_idx > 0 &&
       multi_pvs[slot_idx].score > multi_pvs[slot_idx - 1].score;
       slot_idx--) {
    const PVLine tmp = multi_pvs[slot_idx];
    multi_pvs[slot_idx] = multi_pvs[slot_idx - 1];
    multi_pvs[slot_idx - 1] = tmp;
  }
}

// Fills multi_pvs[1..num_lines-1] with exactly solved lines by re-searching
// root candidates one at a time, in estimated-value order, with a full window.
//
// Candidates outside the top K only ever received a narrowed window during
// the main search, so their estimated values are (nearly always) upper
// bounds. Resolving continues past K for as long as the next candidate's
// estimate beats the worst exact value on the board, which makes the
// displayed set, and not just each displayed line, reliable. It is not a
// proof: a fail-soft estimate can be cut off by a TT entry stored from
// another thread's window, so it is an upper bound in practice rather than
// rigorously, and a candidate scanned after an aborted final depth carries a
// value from that partial depth.
//
// Requires root_moves[1..num_root_moves-1] to be sorted by estimated value
// descending (extract_multi_pvs re-sorts them), because the scan stops at the
// first candidate whose estimate cannot beat the board.
//
// Returns the number of filled slots, and sets *next_candidate to the first
// root index that was never resolved (so an interrupted pass can fall back to
// TT extension for the remaining slots).
static int resolve_multi_pv_lines(EndgameCtx *solver,
                                  EndgameCtxWorker *best_worker, int depth,
                                  PVLine *multi_pvs, int num_lines,
                                  int num_root_moves, int *next_candidate) {
  const uint64_t root_key =
      solver_root_hash_key(solver, best_worker->game_copy);
  if (!display_pass_begin(solver)) {
    *next_candidate = 1;
    return 1;
  }
  best_worker->current_iterative_deepening_depth = depth;
  // The search is over; stop this pass's children from being counted as ply-2
  // progress for the (already finished) first root move.
  best_worker->in_first_root_move = false;

  int filled = 1;
  int candidate_idx = 1;
  PVLine resolved;
  while (candidate_idx < num_root_moves) {
    if (filled >= num_lines) {
      const SmallMove *root_moves =
          (const SmallMove *)best_worker->small_move_arena->memory;
      const int32_t estimate =
          small_move_get_estimated_value(&root_moves[candidate_idx]) -
          solver->initial_spread;
      if (estimate <= multi_pvs[num_lines - 1].score) {
        break;
      }
    }
    // Copy the candidate by value: the re-search allocates from (and can
    // realloc) the arena the root moves live in.
    const SmallMove candidate =
        ((const SmallMove *)
             best_worker->small_move_arena->memory)[candidate_idx];
    const int32_t value = resolve_root_move_pv(solver, best_worker, &candidate,
                                               depth, root_key, &resolved);
    if (value == ABDADA_INTERRUPTED) {
      break;
    }
    // Record the exact value so the final root ordering reflects it.
    small_move_set_estimated_value(
        &((SmallMove *)best_worker->small_move_arena->memory)[candidate_idx],
        value);
    // Depth-limited solves end the exact line short of the game end; extend
    // for display exactly as the best line is extended. negamax_depth is
    // preserved, so the display separator still marks the solved boundary.
    if (solver->transposition_table) {
      solver_pvline_extend_from_tt(&resolved, solver, best_worker->game_copy);
    }
    insert_resolved_pv(multi_pvs, &filled, num_lines, &resolved);
    candidate_idx++;
  }

  // The worker's post-search node flush already happened, so publish again to
  // include the nodes this pass searched.
  atomic_store_explicit(&best_worker->published_nodes_searched,
                        best_worker->local_nodes_searched,
                        memory_order_relaxed);
  display_pass_end(solver);
  *next_candidate = candidate_idx;
  return filled;
}

// Format and log all final PV lines: move-by-move replay, game-end
// annotations (rack points, 6 zeros), win/loss/tie summary.
// Read root SmallMoves from best thread's arena, swap PV move to front,
// build PVLines with TT extension for non-best root moves. Returns number
// of PVs filled. multi_pvs[0] must already be set by caller.
static int extract_multi_pvs(EndgameCtx *solver, EndgameCtxWorker *best_worker,
                             const Game *game, PVLine *multi_pvs, int num_top,
                             int depth) {
  const int num_root_moves = best_worker->n_initial_moves;
  const int num_lines = (num_top < num_root_moves) ? num_top : num_root_moves;
  SmallMove *root_moves = (SmallMove *)best_worker->small_move_arena->memory;

  // Root estimated values can be fail-soft bounds. In top-one PVS mode,
  // later roots that fail low retain upper bounds for next-iteration ordering.
  // This extractor is only called for num_top_moves > 1, which selects
  // solver_needs_exact_root_values and excludes that top-one PVS path.
  // Multi-PV cutoffs and interrupted depths can still leave bounds or mixed
  // depths; the sorting and full-window resolution below handle those cases.

  // Ensure the displayed best move is at root_moves[0] to avoid duplicates.
  // qsort is not stable, so tied values may place it elsewhere.
  const uint64_t pv_tiny = multi_pvs[0].num_moves > 0
                               ? multi_pvs[0].moves[0].tiny_move
                               : INVALID_TINY_MOVE;
  if (pv_tiny != INVALID_TINY_MOVE && root_moves[0].tiny_move != pv_tiny) {
    for (int root_idx = 1; root_idx < num_root_moves; root_idx++) {
      if (root_moves[root_idx].tiny_move == pv_tiny) {
        const SmallMove tmp = root_moves[0];
        root_moves[0] = root_moves[root_idx];
        root_moves[root_idx] = tmp;
        break;
      }
    }
  }

  // Re-sort everything below the pinned best move. iterative_deepening only
  // sorts at the end of a *completed* depth, so an aborted final depth leaves
  // the array holding a mix of values from two depths in no particular order;
  // the swap above can also strand a high-valued move mid-array, and
  // force_actual_move_to_front deliberately keeps a possibly low-valued move
  // near the front. Both the resolve pass (which stops at the first candidate
  // that cannot beat the board) and the fallback below assume descending
  // order, so restore it here.
  if (num_root_moves > 2) {
    stable_sort(root_moves + 1, (size_t)(num_root_moves - 1), sizeof(SmallMove),
                compare_small_moves_by_estimated_value);
  }

  // Solve every displayed line to the end (or to the search depth) instead of
  // showing a one-move line finished by a greedy playout. Skipped for the
  // bound-only search modes, whose root values are deliberately not exact,
  // when no depth was completed, and when the transposition table is off
  // (-ttfraction 0): without a warm TT to cut off the off-PV subtrees, every
  // line would be a full cold re-search, which is far too expensive.
  int filled = 1;
  int next_candidate = 1;
  const bool resolve_exactly = depth > 0 && solver->transposition_table_optim &&
                               !solver->first_win_optim &&
                               !solver->initial_window_optim;
  if (resolve_exactly) {
    filled = resolve_multi_pv_lines(solver, best_worker, depth, multi_pvs,
                                    num_lines, num_root_moves, &next_candidate);
  }

  // Fallback for any slot the exact pass did not reach (interrupted, or
  // disabled above): one root move plus a TT probe and greedy playout.
  for (int slot_idx = filled;
       slot_idx < num_lines && next_candidate < num_root_moves;
       slot_idx++, next_candidate++) {
    PVLine *pv = &multi_pvs[slot_idx];
    root_moves = (SmallMove *)best_worker->small_move_arena->memory;
    pv->moves[0] = root_moves[next_candidate];
    pv->num_moves = 1;
    pv->score = small_move_get_estimated_value(&root_moves[next_candidate]) -
                solver->initial_spread;
    pv->negamax_depth = 1;
    pv->game = NULL;

    if (solver->transposition_table) {
      solver_pvline_extend_from_tt(pv, solver, game);
    }
    filled = slot_idx + 1;
  }
  return filled;
}

// Finds solver->actual_move among best_worker's root moves and builds a
// one-move PVLine (TT-extended, same as extract_multi_pvs's non-best
// entries) for it. Its estimated_value is exact because
// force_actual_move_to_front kept it at root index 0 through every IDS
// depth in every worker, so it always received a full, unnarrowed window.
// Returns true and fills *out_pv if found; false (leaving *out_pv untouched)
// if solver->actual_move is NULL or wasn't found among the root moves.
static bool extract_actual_move_pvline(EndgameCtx *solver,
                                       EndgameCtxWorker *best_worker,
                                       const Game *game, PVLine *out_pv) {
  if (!solver->actual_move) {
    return false;
  }
  int n_root = best_worker->n_initial_moves;
  SmallMove *root_moves = (SmallMove *)best_worker->small_move_arena->memory;
  const Board *board = game_get_board(game);
  Move candidate;
  for (int idx = 0; idx < n_root; idx++) {
    small_move_to_move(&candidate, &root_moves[idx], board);
    if (!endgame_moves_match(&candidate, solver->actual_move)) {
      continue;
    }
    out_pv->moves[0] = root_moves[idx];
    out_pv->num_moves = 1;
    out_pv->score = small_move_get_estimated_value(&root_moves[idx]) -
                    solver->initial_spread;
    out_pv->negamax_depth = 1;
    out_pv->game = NULL;
    if (solver->transposition_table) {
      solver_pvline_extend_from_tt(out_pv, solver, game);
    }
    return true;
  }
  return false;
}

// Destroys the workers lent to a finished inline solve and keeps the main one.
// Each PEG worker owns a context and the pool lends a core to whichever
// context's leaf runs long, so keeping every context's lent workers would hold
// contexts x pool-size workers (about 10 MiB each: move list, undo stack,
// incremental lists, game) although only pool-size of them ever run at once.
// The next lend creates a fresh worker. Called after the helpers are joined
// with the injection window shut, so no add or live-view read can reach them.
static void endgame_ctx_release_lent_workers(EndgameCtx *solver) {
  const int cap = atomic_load(&solver->cap_workers);
  if (cap <= solver->threads) {
    return;
  }
  cpthread_mutex_lock(&solver->add_mutex);
  atomic_store(&solver->live_workers, solver->threads);
  atomic_store(&solver->cap_workers, solver->threads);
  for (int worker_idx = solver->threads; worker_idx < cap; worker_idx++) {
    solver_worker_destroy(solver->workers[worker_idx]);
    solver->workers[worker_idx] = NULL;
  }
  cpthread_mutex_unlock(&solver->add_mutex);
}

// Single-threaded endgame solve that runs in the calling thread (no
// cpthread_create). Safe for use from concurrent PEG decomp threads.
void endgame_solve_inline(EndgameCtx **ctx, const EndgameArgs *endgame_args,
                          EndgameResults *results) {
  assert(ctx);
  assert(results);
  // Same precondition as endgame_solve. This entry point takes no ErrorStack
  // (it's the hot path PEG drives on known-empty-bag leaves), so a non-empty
  // bag is a caller bug rather than a recoverable input error.
  assert(bag_get_letters(game_get_bag(endgame_args->game)) == 0);
  if (*ctx == NULL) {
    *ctx = endgame_ctx_create();
  }
  EndgameCtx *solver = *ctx;

  endgame_ctx_reset(solver, results, endgame_args);
  // The inline main worker runs in the calling thread, so the base worker count
  // is always 1 regardless of the caller's num_threads — only injected helpers
  // (ordinals > 0) are spawned. Force threads/live_workers to 1 so injection
  // ordinals and join ranges stay consistent (otherwise helpers would start at
  // ordinal == num_threads, leaving gaps).
  solver->threads = 1;
  atomic_store(&solver->live_workers, 1);

  uint64_t base_seed = endgame_args->seed != 0
                           ? endgame_args->seed
                           : (uint64_t)ctime_get_current_time();
  endgame_ctx_prepare_workers(solver, base_seed);

  // Suppress stuck-tile log to avoid localtime thread safety issues.
  atomic_store(&solver->stuck_tile_logged, 1);

  // Open the injection window so helpers can be added mid-search via
  // endgame_add_worker while this thread runs the main worker (ordinal 0). The
  // calling thread is the main worker, so unlike endgame_solve there is no
  // spawned thread for ordinal 0 — only injected helpers (ordinals > 0) are
  // joined.
  if (solver->max_workers > solver->threads) {
    cpthread_mutex_lock(&solver->add_mutex);
    atomic_store(&solver->window_open_ns, ctimer_monotonic_ns());
    atomic_store(&solver->adding_closed, 0);
    cpthread_mutex_unlock(&solver->add_mutex);
  }

  // Run IDS directly in the calling thread.
  iterative_deepening(solver->workers[0], solver->requested_plies);

  // Shut the window and join any injected helpers. live_workers counts the
  // in-thread main worker (solver->threads, normally 1) plus any added workers,
  // so the spawned pthreads are [solver->threads, live_workers).
  cpthread_mutex_lock(&solver->add_mutex);
  atomic_store(&solver->adding_closed, 1);
  const int total_workers = atomic_load(&solver->live_workers);
  cpthread_mutex_unlock(&solver->add_mutex);
  for (int idx = solver->threads; idx < total_workers; idx++) {
    cpthread_join(solver->worker_ids[idx]);
  }
  endgame_ctx_release_lent_workers(solver);

  const PVLine *best_pv =
      endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  solver->principal_variation = *best_pv;

  // Finalize results like endgame_solve does: stop the timer, record terminal
  // status, and drop the TT reference that endgame_ctx_reset stored in the
  // results' pvline-extend args. Without the last step a post-solve
  // string_builder_endgame_results would dereference the ctx's TT, which is a
  // use-after-free once the caller destroys the ctx (and its owned TT).
  endgame_results_stop_ctimer(results);
  if (thread_control_get_status(endgame_args->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    endgame_results_set_status(results, ENDGAME_RESULT_STATUS_INTERRUPTED);
  } else {
    endgame_results_set_status(results, ENDGAME_RESULT_STATUS_FINISHED);
  }
  endgame_results_set_pvline_extend_args(
      results, NULL, endgame_results_get_solving_player(results),
      endgame_results_get_max_depth(results));
}

void endgame_solve(EndgameCtx **ctx, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack) {
  assert(ctx);
  const int bag_size = bag_get_letters(game_get_bag(endgame_args->game));
  if (bag_size != 0) {
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string(
                         "bag must be empty to solve an endgame, but have %d "
                         "letters in the bag",
                         bag_size));
    return;
  }

  if (*ctx == NULL) {
    *ctx = endgame_ctx_create();
  }
  EndgameCtx *solver = *ctx;
  endgame_ctx_reset(solver, results, endgame_args);
  solver->movegen_budget = gen_get_movegen_budget();

  // Set base seed for ABDADA jitter
  endgame_ctx_prepare_workers(solver, endgame_args->seed);

  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    cpthread_create(&solver->worker_ids[thread_index], solver_worker_start,
                    solver->workers[thread_index]);
  }

  // Open the injection window now that the initial workers exist and the solve
  // state is fully published. endgame_add_worker may now grow the worker count.
  if (solver->max_workers > solver->threads) {
    cpthread_mutex_lock(&solver->add_mutex);
    atomic_store(&solver->window_open_ns, ctimer_monotonic_ns());
    atomic_store(&solver->adding_closed, 0);
    cpthread_mutex_unlock(&solver->add_mutex);
  }

  // Wait for the initial workers. The main worker (ordinal 0) drives
  // completion, so once these exit the search has finished (or
  // interrupted/timed out).
  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    cpthread_join(solver->worker_ids[thread_index]);
  }

  // Shut the injection window and join any workers added mid-search. Under the
  // lock so no add is half-spawned when we snapshot the count; late workers
  // also exit on search_complete, so they are done or finishing.
  cpthread_mutex_lock(&solver->add_mutex);
  atomic_store(&solver->adding_closed, 1);
  const int total_workers = atomic_load(&solver->live_workers);
  cpthread_mutex_unlock(&solver->add_mutex);
  for (int thread_index = solver->threads; thread_index < total_workers;
       thread_index++) {
    cpthread_join(solver->worker_ids[thread_index]);
  }

  endgame_results_stop_ctimer(results);

  if (thread_control_get_status(endgame_args->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    endgame_results_set_status(results, ENDGAME_RESULT_STATUS_INTERRUPTED);
  } else {
    endgame_results_set_status(results, ENDGAME_RESULT_STATUS_FINISHED);
  }

  // Find the thread that completed the deepest search for its root move
  // arena. Used both for multi-PV display extraction below and for reading
  // back an exact value for actual_move, since only the deepest-completed
  // worker's root array reflects the final, most authoritative search. All
  // workers are joined, so no lock is needed to read their state.
  int best_thread = 0;
  int best_depth = solver->workers[0]->completed_depth;
  const int live = atomic_load(&solver->live_workers);
  for (int thread_index = 1; thread_index < live; thread_index++) {
    int thread_depth = solver->workers[thread_index]->completed_depth;
    if (thread_depth > best_depth) {
      best_depth = thread_depth;
      best_thread = thread_index;
    }
  }
  notify_unreported_deepest_depth(solver, best_thread, best_depth);

  // Build the display lines into a local buffer, NOT into the results, and
  // without the DISPLAY lock held. The multi-PV resolve pass inside
  // extract_multi_pvs is real negamax work (unbounded for an untimed solve),
  // and status is already FINISHED, so holding the lock across it would block
  // a polling UI or API thread for the whole pass -- indistinguishable from a
  // hang. Only the publish below needs the lock.
  PVLine *local_pvs = NULL;
  int num_pvs = 0;
  if (endgame_args->enable_pv_display) {
    local_pvs = malloc_or_die(sizeof(PVLine) * (size_t)solver->num_top_moves);
    // Get the best PV from the search results (tracked via
    // endgame_results_set_best_pvline during the search).
    local_pvs[0] = *endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
    if (display_pass_begin(solver)) {
      EndgameCtxWorker *best_worker = solver->workers[best_thread];
      complete_pv_line(solver, best_worker,
                       endgame_results_get_depth(results, ENDGAME_RESULT_BEST),
                       &local_pvs[0]);
      // Publish again to include the nodes this pass searched.
      atomic_store_explicit(&best_worker->published_nodes_searched,
                            best_worker->local_nodes_searched,
                            memory_order_relaxed);
      display_pass_end(solver);
    }
    // TT-extend the best PV for display.
    solver_pvline_extend_from_tt(&local_pvs[0], solver, endgame_args->game);
    num_pvs = 1;
    if (solver->num_top_moves > 1) {
      // Extract top-K root moves from best thread's arena.
      num_pvs = extract_multi_pvs(solver, solver->workers[best_thread],
                                  endgame_args->game, local_pvs,
                                  solver->num_top_moves, best_depth);
    }
  }

  PVLine actual_pv;
  const bool has_actual_pv = extract_actual_move_pvline(
      solver, solver->workers[best_thread], endgame_args->game, &actual_pv);

  // Hold the DISPLAY lock because concurrent status/shendgame calls read these
  // fields under it.
  endgame_results_lock(results, ENDGAME_RESULT_DISPLAY);
  if (local_pvs) {
    endgame_results_ensure_pvs_capacity(results, solver->num_top_moves);
    PVLine *multi_pvs = endgame_results_get_multi_pvs(results);
    memcpy(multi_pvs, local_pvs, sizeof(PVLine) * (size_t)num_pvs);
    endgame_results_set_num_pvs(results, num_pvs);
  }

  if (has_actual_pv) {
    endgame_results_set_actual_pvline(results, &actual_pv, actual_pv.score,
                                      best_depth);
  }

  // The stored multi_pvs are already fully extended; null out the TT reference
  // so post-solve calls to string_builder_endgame_results don't touch a
  // potentially freed pointer after endgame_ctx_destroy.
  endgame_results_set_pvline_extend_args(
      results, NULL, endgame_results_get_solving_player(results),
      endgame_results_get_max_depth(results));
  endgame_results_unlock(results, ENDGAME_RESULT_DISPLAY);
  free(local_pvs);
}

int endgame_live_workers(const EndgameCtx *ctx) {
  return atomic_load(&ctx->live_workers);
}

bool endgame_injecting(const EndgameCtx *ctx) {
  return atomic_load(&ctx->adding_closed) == 0;
}

int64_t endgame_window_open_ns(const EndgameCtx *ctx) {
  return atomic_load(&ctx->window_open_ns);
}

bool endgame_add_worker(EndgameCtx *solver) {
  cpthread_mutex_lock(&solver->add_mutex);
  // Decline if the window is shut (between solves, or the join has begun) or
  // the max_workers ceiling (== arr_cap) is reached.
  if (atomic_load(&solver->adding_closed)) {
    cpthread_mutex_unlock(&solver->add_mutex);
    return false;
  }
  const int ordinal = atomic_load(&solver->live_workers);
  if (ordinal >= solver->arr_cap) {
    cpthread_mutex_unlock(&solver->add_mutex);
    return false;
  }

  // Create the worker struct on first use at this ordinal; reuse it on later
  // solves. Either way, stamp this solve's ordinal. The worker's MoveGen cache
  // slot is assigned on demand by get_movegen() when it first generates moves.
  if (ordinal >= atomic_load(&solver->cap_workers)) {
    solver->workers[ordinal] = endgame_ctx_create_worker(
        solver, ordinal, solver->worker_base_seed, solver->game);
    // Release so a live-view accessor that later reads this cap (acquire) also
    // sees the fully-created worker struct stored just above.
    atomic_store(&solver->cap_workers, ordinal + 1);
  } else {
    solver->workers[ordinal]->ordinal = ordinal;
  }
  // Prep the late worker's game from the read-only root inputs (solver->game +
  // pruned KWGs + fresh cross-sets) — NOT from worker 0, whose game_copy is
  // mid-traversal. Seeds the worker PRNG by its ordinal.
  endgame_ctx_reset_worker_and_game(solver, solver->worker_base_seed, ordinal);
  cpthread_create(&solver->worker_ids[ordinal], solver_worker_start,
                  solver->workers[ordinal]);
  atomic_store(&solver->live_workers, ordinal + 1);
  cpthread_mutex_unlock(&solver->add_mutex);
  return true;
}
