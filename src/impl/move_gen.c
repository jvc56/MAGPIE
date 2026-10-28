#include "move_gen.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../def/bit_rack_defs.h"
#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/klv_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../def/wordsmog_defs.h"
#include "../ent/anchor.h"
#include "../ent/bag.h"
#include "../ent/best_move_carry.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/kwg_alpha.h"
#include "../ent/lane_shadow_cache.h"
#include "../ent/leave_map.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/pat.h"
#include "../ent/pat_eval.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/rack_info_table.h"
#include "../ent/static_eval.h"
#include "../ent/thread_control.h"
#include "../ent/wmp.h"
#include "../ent/word_info_table.h"
#include "../util/io_util.h"
#include "wmp_move_gen.h"
#include "wordsmog_arrangement.h"
#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See WMP_ENTRY_UNRESOLVED in wmp_move_gen.h. Only its address is used.
const WMPEntry wmp_entry_unresolved_sentinel = {0};

#define INITIAL_LAST_ANCHOR_COL (BOARD_DIM)

// The odd multiplier of gen_hash_lane's steps (MurmurHash3's fmix64).
static const uint64_t LANE_HASH_MULTIPLIER = 0xFF51AFD7ED558CCDULL;
static_assert(2 * BOARD_DIM <= 64,
              "gen_seed_carried_best_move keeps a bit per lane in a uint64_t");

// Cache move generators since destroying
// and recreating a movegen for
// every request to generate moves would
// be expensive. The infer and sim functions
// don't have this problem since they are
// only called once per command.
// Pool of reusable MoveGens. cached_gens[slot] is allocated lazily and kept
// for the whole process (freed only at shutdown) so the expensive MoveGen
// allocation is amortized across solves; slot_in_use[slot] marks whether a
// live thread currently owns that slot. See get_movegen below.
static MoveGen *cached_gens[MAX_THREADS];
static bool slot_in_use[MAX_THREADS];
static cpthread_mutex_t cache_mutex = CPTHREAD_MUTEX_INITIALIZER;
// The move generations counted on gens gen_destroy_cache has freed, so the
// process's count never goes back (gen_get_movegen_count). Under cache_mutex.
static uint64_t freed_gens_movegen_count = 0;
// The slots live threads hold now, and the most they have held at once since
// gen_reset_slots_high_water (gen_get_slots_high_water). Under cache_mutex.
static int slots_in_use_count = 0;
static int slots_in_use_high_water = 0;
// How many things want generations accounted for: a running movegen limit,
// each thread with a movegen budget set, each counting session
// (gen_start_counting_movegens). While it is 0 a generation is not counted,
// so a run that uses none of them pays nothing for them.
static _Atomic int movegen_accounting_users = 0;

static void gen_add_accounting_users(int delta) {
  if (delta != 0) {
    atomic_fetch_add_explicit(&movegen_accounting_users, delta,
                              memory_order_relaxed);
  }
}

static void wordsmog_pat_refine_destroy(WordsmogPATRefine *refine);

void generator_destroy(MoveGen *gen) {
  if (!gen) {
    return;
  }
  wordsmog_pat_refine_destroy(gen->wordsmog_pat);
  free(gen);
}

// MoveGen pool keyed by a per-thread slot. Each live pthread acquires a
// distinct free slot on its first generate_moves and holds it for the thread's
// lifetime, so two threads can never share a MoveGen and corrupt each other's
// state.
//
// A slot's MoveGen is allocated lazily and reused for the whole process, so the
// expensive allocation is amortized across solves. The pthread key's destructor
// only *releases the slot* on thread exit (it does not free the gen), so a
// future thread reuses
// both the slot and its already-allocated gen — bounded memory, no per-thread
// reallocation, and no leak. Gens are freed when gen_destroy_cache runs
// (typically at shutdown or after CLI commands complete).
//
// A reused gen carries stale per-call scratch, which is fine: generate_moves
// resets the state it reads each call — the same reuse the old cross-solve
// slot sharing relied on.
static cpthread_key_t gen_key;
static cpthread_once_t gen_key_once = CPTHREAD_ONCE_INIT;

// Thread-exit destructor: return this thread's slot to the pool (keep the gen).
static void gen_release_slot(void *ptr) {
  if (ptr == NULL) {
    return;
  }
  // The next thread to take this gen must not inherit a budget that belonged
  // to a move this thread was searching for, and may since be gone.
  if (((MoveGen *)ptr)->movegen_budget != NULL) {
    gen_add_accounting_users(-1);
  }
  ((MoveGen *)ptr)->movegen_budget = NULL;
  ((MoveGen *)ptr)->budget_uncharged = 0;
  cpthread_mutex_lock(&cache_mutex);
  for (int i = 0; i < MAX_THREADS; i++) {
    if (cached_gens[i] == ptr) {
      slot_in_use[i] = false;
      slots_in_use_count--;
      break;
    }
  }
  cpthread_mutex_unlock(&cache_mutex);
}

static void gen_key_init(void) {
  cpthread_key_create(&gen_key, gen_release_slot);
}

MoveGen *get_movegen(void) {
  cpthread_once(&gen_key_once, gen_key_init);
  MoveGen *gen = cpthread_getspecific(gen_key);
  if (gen != NULL) {
    return gen;
  }
  // First touch on this thread: acquire a free slot, reusing its gen if one was
  // allocated by an earlier thread that has since released the slot.
  cpthread_mutex_lock(&cache_mutex);
  int slot = -1;
  for (int i = 0; i < MAX_THREADS; i++) {
    if (!slot_in_use[i]) {
      slot = i;
      slot_in_use[i] = true;
      break;
    }
  }
  if (slot < 0) {
    cpthread_mutex_unlock(&cache_mutex);
    log_fatal("movegen pool exhausted: more than %d concurrent threads",
              MAX_THREADS);
  }
  slots_in_use_count++;
  if (slots_in_use_count > slots_in_use_high_water) {
    slots_in_use_high_water = slots_in_use_count;
  }
  gen = cached_gens[slot];
  if (gen == NULL) {
    gen = calloc_or_die(1, sizeof(MoveGen));
    cached_gens[slot] = gen;
  }
  cpthread_mutex_unlock(&cache_mutex);
  cpthread_setspecific(gen_key, gen);
  return gen;
}

void gen_destroy_cache(void) {
  // May run mid-process, not only at shutdown: caches_destroy() calls this
  // after a CLI command completes (and again at exit). It runs on a single
  // thread after every worker has been joined, so no slot-release destructor
  // can race it.
  cpthread_mutex_lock(&cache_mutex);
  for (int i = 0; i < (MAX_THREADS); i++) {
    if (cached_gens[i]) {
      freed_gens_movegen_count += cached_gens[i]->movegen_count;
    }
    generator_destroy(cached_gens[i]);
    cached_gens[i] = NULL;
    slot_in_use[i] = false;
  }
  slots_in_use_count = 0;
  cpthread_mutex_unlock(&cache_mutex);
  // The calling thread's key still points at the gen we just freed (key
  // destructors don't fire for a thread that keeps running), so clear it.
  // Otherwise the next get_movegen on this thread takes the fast path and
  // returns that dangling pointer instead of acquiring a fresh slot/gen.
  cpthread_once(&gen_key_once, gen_key_init);
  cpthread_setspecific(gen_key, NULL);
}

// The running command's movegen limit: set and cleared with no thread
// generating moves, so plain variables, read by the threads the command
// starts after it is set. The count and whether it was reached are shared
// while the command runs.
#define MOVEGEN_LIMIT_BATCH 64
static uint64_t movegen_limit = 0;
static ThreadControl *movegen_limit_thread_control = NULL;
static _Atomic uint64_t movegen_limit_charged = 0;
static atomic_bool movegen_limit_reached = false;

void gen_start_movegen_limit(uint64_t limit, ThreadControl *thread_control) {
  // A generator's uncharged count from an earlier command is not this one's.
  cpthread_mutex_lock(&cache_mutex);
  for (int i = 0; i < MAX_THREADS; i++) {
    if (cached_gens[i]) {
      cached_gens[i]->limit_uncharged = 0;
    }
  }
  cpthread_mutex_unlock(&cache_mutex);
  atomic_store(&movegen_limit_charged, 0);
  atomic_store(&movegen_limit_reached, false);
  movegen_limit_thread_control = thread_control;
  movegen_limit = limit;
  if (limit > 0) {
    gen_add_accounting_users(1);
  }
}

bool gen_stop_movegen_limit(void) {
  if (movegen_limit > 0) {
    gen_add_accounting_users(-1);
  }
  movegen_limit = 0;
  movegen_limit_thread_control = NULL;
  return atomic_load(&movegen_limit_reached);
}

void gen_start_counting_movegens(void) { gen_add_accounting_users(1); }

void gen_stop_counting_movegens(void) { gen_add_accounting_users(-1); }

void movegen_budget_init(MovegenBudget *budget, uint64_t limit) {
  budget->limit = limit;
  atomic_store(&budget->spent, 0);
}

uint64_t movegen_budget_get_spent(const MovegenBudget *budget) {
  return atomic_load(&budget->spent);
}

// Generations a thread charges its move budget at a time: small, since a
// budget may be a few thousand generations, but enough that the shared count
// is not touched on every one.
#define MOVEGEN_BUDGET_BATCH 16

static void gen_charge_movegen_budget(MoveGen *gen) {
  if (gen->movegen_budget && gen->budget_uncharged > 0) {
    atomic_fetch_add(&gen->movegen_budget->spent, gen->budget_uncharged);
  }
  gen->budget_uncharged = 0;
}

MovegenBudget *gen_set_movegen_budget(MovegenBudget *budget) {
  MoveGen *gen = get_movegen();
  gen_charge_movegen_budget(gen);
  MovegenBudget *previous = gen->movegen_budget;
  gen->movegen_budget = budget;
  gen_add_accounting_users((budget != NULL) - (previous != NULL));
  return previous;
}

MovegenBudget *gen_get_movegen_budget(void) {
  return get_movegen()->movegen_budget;
}

bool gen_movegen_budget_spent(void) {
  const MovegenBudget *budget = get_movegen()->movegen_budget;
  return budget && budget->limit > 0 &&
         atomic_load(&budget->spent) >= budget->limit;
}

// Counts one generation, and against the running command's limit and the
// move's budget, if any. Off the hot path: see gen_count_movegen.
__attribute__((noinline)) static void gen_account_movegen(MoveGen *gen) {
  gen->movegen_count++;
  if (gen->movegen_budget && ++gen->budget_uncharged >= MOVEGEN_BUDGET_BATCH) {
    gen_charge_movegen_budget(gen);
  }
  if (movegen_limit == 0 || ++gen->limit_uncharged < MOVEGEN_LIMIT_BATCH) {
    return;
  }
  const uint64_t charged =
      atomic_fetch_add(&movegen_limit_charged, gen->limit_uncharged) +
      gen->limit_uncharged;
  gen->limit_uncharged = 0;
  if (charged >= movegen_limit &&
      !atomic_exchange(&movegen_limit_reached, true) &&
      movegen_limit_thread_control) {
    thread_control_set_status(movegen_limit_thread_control,
                              THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
}

// The one check every generation pays: with no limit, budget or count
// running, a relaxed load and a branch never taken.
static inline void gen_count_movegen(MoveGen *gen) {
  if (atomic_load_explicit(&movegen_accounting_users, memory_order_relaxed) !=
      0) {
    gen_account_movegen(gen);
  }
}

uint64_t gen_get_movegen_count(void) {
  // A gen's slot is released when its thread exits, but the gen and its count
  // stay in cached_gens for the next thread, so summing every allocated gen
  // counts the work of threads that have come and gone as well.
  cpthread_mutex_lock(&cache_mutex);
  uint64_t total = freed_gens_movegen_count;
  for (int i = 0; i < MAX_THREADS; i++) {
    if (cached_gens[i]) {
      total += cached_gens[i]->movegen_count;
    }
  }
  cpthread_mutex_unlock(&cache_mutex);
  return total;
}

int gen_get_slots_high_water(void) {
  cpthread_mutex_lock(&cache_mutex);
  const int high_water = slots_in_use_high_water;
  cpthread_mutex_unlock(&cache_mutex);
  return high_water;
}

void gen_reset_slots_high_water(void) {
  cpthread_mutex_lock(&cache_mutex);
  slots_in_use_high_water = slots_in_use_count;
  cpthread_mutex_unlock(&cache_mutex);
}

// Cache getter functions

static inline MachineLetter gen_cache_get_letter(const MoveGen *gen, int col) {
  return square_get_letter(&gen->row_squares[col]);
}

static inline bool gen_cache_get_is_anchor(const MoveGen *gen, int col) {
  return square_get_anchor(&gen->row_squares[col]);
}

static inline int gen_cache_is_empty(const MoveGen *gen, int col) {
  return gen_cache_get_letter(gen, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

static inline bool gen_cache_get_is_cross_word(const MoveGen *gen, int col) {
  return square_get_is_cross_word(&gen->row_squares[col]);
}

static inline BonusSquare gen_cache_get_bonus_square(const MoveGen *gen,
                                                     int col) {
  return square_get_bonus_square(&gen->row_squares[col]);
}

static inline uint64_t gen_cache_get_cross_set(const MoveGen *gen, int col) {
  return square_get_cross_set(&gen->row_squares[col]);
}

static inline Equity gen_cache_get_cross_score(const MoveGen *gen, int col) {
  return square_get_cross_score(&gen->row_squares[col]);
}

static inline uint64_t gen_cache_get_left_extension_set(const MoveGen *gen,
                                                        int col) {
  return square_get_left_extension_set(&gen->row_squares[col]);
}

static inline uint64_t gen_cache_get_right_extension_set(const MoveGen *gen,
                                                         int col) {
  return square_get_right_extension_set(&gen->row_squares[col]);
}

static inline Equity gen_get_static_equity(const MoveGen *gen,
                                           const Move *move) {
  return static_eval_get_move_equity_with_leave_value(
      &gen->ld, move, &gen->player_rack, &gen->opponent_rack,
      gen->opening_move_word_penalties, gen->opening_move_letter_penalties,
      pat_eval_context_get_active_classes(&gen->pat_eval_ctx),
      &gen->pat_eval_ctx, gen->board_number_of_tiles_played,
      gen->number_of_tiles_in_bag,
      leave_map_get_current_value(&gen->leave_map));
}

// Best-move recording computes the PAT term lazily: a candidate whose
// equity without it, plus an upper bound on the term, is already strictly
// below the best move's full equity cannot become the best move, so the
// per-move lane rescans run only while the candidate can still contend
// (see gen_pat_contender_equity). The stored equity of a skipped candidate
// is an overestimate used only in a comparison it strictly loses.
static inline bool gen_pat_is_active(const MoveGen *gen) {
  return gen->pat_eval_ctx.weights != NULL;
}

// Static equity without the PAT term. Passes the mover's real active PAT
// classes, so this and gen_get_static_equity differ only by that term.
static inline Equity gen_get_static_equity_without_pat(const MoveGen *gen,
                                                       const Move *move) {
  return static_eval_get_move_equity_with_leave_value(
      &gen->ld, move, &gen->player_rack, &gen->opponent_rack,
      gen->opening_move_word_penalties, gen->opening_move_letter_penalties,
      pat_eval_context_get_active_classes(&gen->pat_eval_ctx), NULL,
      gen->board_number_of_tiles_played, gen->number_of_tiles_in_bag,
      leave_map_get_current_value(&gen->leave_map));
}

static inline const Move *gen_get_readonly_best_move(const MoveGen *gen) {
  return &gen->best_move_and_current_move[gen->best_move_index];
}

// The equity a best-move candidate is recorded with when PAT is active: its
// equity without the term plus the term, which is exact unless the
// candidate cannot reach the best move's equity. Then it is an overestimate
// that still strictly loses the compare (see pat_eval_move_penalty_capped).
static inline Equity gen_pat_contender_equity(const MoveGen *gen,
                                              const Move *move,
                                              const Rack *leave,
                                              Equity equity_without_pat) {
  const Equity best_equity = move_get_equity(gen_get_readonly_best_move(gen));
  Equity floor = EQUITY_MIN_VALUE;
  if (best_equity != EQUITY_INITIAL_VALUE) {
    // The smallest term with which the candidate still reaches the best.
    int64_t needed = (int64_t)best_equity - equity_without_pat;
    if (needed < EQUITY_MIN_VALUE) {
      needed = EQUITY_MIN_VALUE;
    } else if (needed > EQUITY_MAX_VALUE) {
      needed = EQUITY_MAX_VALUE;
    }
    floor = (Equity)needed;
  }
  return equity_without_pat +
         pat_eval_move_penalty_capped(&gen->pat_eval_ctx, move, leave, floor);
}

// The PAT bound, utility correction included, for a WMP anchor's plays:
// the lane's bound on the defense term, and the largest correction a play
// of tiles_to_play tiles scoring at most highest_possible_score can get
// (see pat_eval_utility_bound_for_play). The position-wide correction bound
// assumes the highest-scoring play of any length, which almost no anchor
// can make. Zero when the term is off.
static inline Equity gen_wmp_anchor_pat_bound(const MoveGen *gen, int dir,
                                              int lane, const Anchor *anchor) {
  if (!gen_pat_is_active(gen)) {
    return 0;
  }
  return pat_eval_lane_defense_bound(&gen->pat_eval_ctx, dir, lane) +
         pat_eval_utility_bound_for_play(&gen->pat_eval_ctx,
                                         anchor->tiles_to_play,
                                         anchor->highest_possible_score);
}

static inline Move *gen_get_best_move(MoveGen *gen) {
  return &gen->best_move_and_current_move[gen->best_move_index];
}

static inline Move *gen_get_current_move(MoveGen *gen) {
  return &gen->best_move_and_current_move[gen->best_move_index ^ 1];
}

static inline void gen_switch_best_move_and_current_move(MoveGen *gen) {
  gen->best_move_index ^= 1;
}

static inline void set_play_for_record(Move *move, game_event_t move_type,
                                       int leftstrip, int rightstrip,
                                       Equity score, int start_row,
                                       int start_col, int tiles_played, int dir,
                                       MachineLetter strip[]) {
  move_set_all_except_equity(move, strip, leftstrip, rightstrip, score,
                             start_row, start_col, tiles_played, dir,
                             move_type);
  if (board_is_dir_vertical(dir)) {
    move_set_row_start(move, start_col);
    move_set_col_start(move, start_row);
  }
}

static inline Equity get_move_equity_for_sort_type(const MoveGen *gen,
                                                   const Move *move,
                                                   Equity score) {
  switch (gen->move_sort_type) {
  case MOVE_SORT_EQUITY:
    return gen_get_static_equity(gen, move);
  case MOVE_SORT_SCORE:
    return score;
  default:
    // TODO(olaugh): make move_sort_type an enum and remove default case
    log_fatal("unhandled move sort type: %d", gen->move_sort_type);
  }
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#else
  return score;
#endif
}

static inline void set_small_play_for_record(SmallMove *move,
                                             game_event_t move_type,
                                             int leftstrip, int rightstrip,
                                             Equity score, int start_row,
                                             int start_col, int tiles_played,
                                             int dir, MachineLetter strip[]) {

  small_move_set_all(move, strip, leftstrip, rightstrip, score, start_row,
                     start_col, tiles_played, board_is_dir_vertical(dir),
                     move_type);
}

static inline Equity gen_get_cutoff_equity_or_score(const MoveGen *gen) {
  return gen->cutoff_equity_or_score;
}

// The cutoff never falls between resets: the WMP subrack mask in wordmap_gen
// drops a subrack whose leave bound is below the cutoff when it builds the
// mask, and relies on the cutoff still being at least that high when the walk
// reaches the subrack.
static inline void gen_update_cutoff_equity_or_score(MoveGen *gen) {
  const Equity previous_cutoff = gen->cutoff_equity_or_score;
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_ALL_SMALL:
  case MOVE_RECORD_TILES_PLAYED:
    log_fatal("gen_get_cutoff_equity_or_score called with "
              "MOVE_RECORD_ALL or "
              "MOVE_RECORD_ALL_SMALL");
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
    __builtin_unreachable();
#else
    return;
#endif
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:
    gen->cutoff_equity_or_score =
        gen->best_move_equity_or_score - gen->eq_margin_movegen;
    break;
  case MOVE_RECORD_BEST:;
    const Move *move = gen_get_readonly_best_move(gen);
    gen->cutoff_equity_or_score = (gen->move_sort_type == MOVE_SORT_EQUITY)
                                      ? move_get_equity(move)
                                      : move_get_score(move);
    break;
  case MOVE_RECORD_BEST_SMALL:
    // No Move object; best_move_equity_or_score is already the best score.
    gen->cutoff_equity_or_score = gen->best_move_equity_or_score;
    break;
  }
  assert(gen->cutoff_equity_or_score >= previous_cutoff);
  // previous_cutoff is read only by the assert above (compiled out in
  // release).
  (void)previous_cutoff;
}

static inline void
gen_insert_spare_move_within_x_equity_of_best(MoveGen *gen,
                                              Equity move_equity_or_score) {
  if (gen->best_move_equity_or_score == EQUITY_INITIAL_VALUE) {
    gen->best_move_equity_or_score = move_equity_or_score;
    gen_update_cutoff_equity_or_score(gen);
    move_list_insert_spare_move(gen->move_list, move_equity_or_score);
    return;
  }
  Equity cutoff_equity_or_score = gen_get_cutoff_equity_or_score(gen);
  if (move_equity_or_score < cutoff_equity_or_score) {
    return;
  }
  move_list_insert_spare_move(gen->move_list, move_equity_or_score);
  if (move_equity_or_score > gen->best_move_equity_or_score) {
    // The current move is better than the best move, so update the best move to
    // the current move
    gen->best_move_equity_or_score = move_equity_or_score;
    // Update the cutoff now that the best possible equity or score has improved
    gen_update_cutoff_equity_or_score(gen);
    cutoff_equity_or_score = gen_get_cutoff_equity_or_score(gen);
    while (move_list_get_count(gen->move_list) > 0 &&
           move_list_peek_equity(gen->move_list) < cutoff_equity_or_score) {
      move_list_pop_move(gen->move_list);
    }
  }
}

static inline void update_best_move_or_insert_into_movelist(
    MoveGen *gen, int leftstrip, int rightstrip, game_event_t move_type,
    Equity score, int start_row, int start_col, int tiles_played, int dir,
    MachineLetter strip[]) {
  bool need_to_update_best_move_equity_or_score = false;
  Equity move_equity_or_score = 0;
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:;
    Move *move = move_list_get_spare_move(gen->move_list);
    set_play_for_record(move, move_type, leftstrip, rightstrip, score,
                        start_row, start_col, tiles_played, dir, strip);
    move_equity_or_score = get_move_equity_for_sort_type(gen, move, score);
    if (gen->move_record_type == MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST) {
      // This updates the cutoff move internally so no update will be pending
      // afterward.
      gen_insert_spare_move_within_x_equity_of_best(gen, move_equity_or_score);
    } else {
      move_list_insert_spare_move(gen->move_list, move_equity_or_score);
    }
    break;
  case MOVE_RECORD_BEST:;
    Move *current_move = gen_get_current_move(gen);
    set_play_for_record(current_move, move_type, leftstrip, rightstrip, score,
                        start_row, start_col, tiles_played, dir, strip);
    if (gen->move_sort_type == MOVE_SORT_EQUITY && gen_pat_is_active(gen) &&
        !gen->stop_on_threshold) {
      const Equity equity_without_pat =
          gen_get_static_equity_without_pat(gen, current_move);
      move_equity_or_score = gen_pat_contender_equity(
          gen, current_move, &gen->player_rack, equity_without_pat);
    } else {
      move_equity_or_score =
          get_move_equity_for_sort_type(gen, current_move, score);
    }
    move_set_equity(current_move, move_equity_or_score);
    // A best move carried in from an earlier generation (see
    // gen_seed_carried_best_move) is found again; only a better move
    // replaces it.
    if (compare_moves(current_move, gen_get_readonly_best_move(gen), true) ==
        1) {
      need_to_update_best_move_equity_or_score = true;
      gen_switch_best_move_and_current_move(gen);
    }
    break;
  case MOVE_RECORD_ALL_SMALL:;
    SmallMove *small_move = small_move_list_get_spare_move(gen->move_list);
    set_small_play_for_record(small_move, move_type, leftstrip, rightstrip,
                              score, start_row, start_col, tiles_played, dir,
                              strip);
    // small_move doesn't use equity.
    move_list_insert_spare_small_move(gen->move_list);
    break;
  case MOVE_RECORD_TILES_PLAYED:
    // Only record which tile types from the rack appear in valid moves.
    for (int i = leftstrip; i <= rightstrip; i++) {
      MachineLetter tile = strip[i];
      if (tile == PLAYED_THROUGH_MARKER) {
        continue;
      }
      uint8_t ml_val = get_is_blanked(tile) ? 0 : tile;
      // NOLINTNEXTLINE(clang-analyzer-core.BitwiseShift)
      gen->tiles_played_bv |= ((uint64_t)1 << ml_val);
    }
    // Early exit when all rack tile types have been seen
    if ((gen->tiles_played_bv & gen->target_tiles_bv) == gen->target_tiles_bv) {
      gen->threshold_exceeded = true;
    }
    break;
  case MOVE_RECORD_BEST_SMALL:;
    SmallMove *best_sm = small_move_list_get_spare_move(gen->move_list);
    set_small_play_for_record(best_sm, move_type, leftstrip, rightstrip, score,
                              start_row, start_col, tiles_played, dir, strip);
    move_equity_or_score = score;
    if (tiles_played == gen->best_small_out_rack_size) {
      move_equity_or_score += gen->best_small_out_bonus;
    }
    // With canonical ties, equal values go to the larger tiny_move, so the
    // winner does not depend on the order in which anchors are searched. A tie
    // leaves the cutoff unchanged, and the shadow pruning is strict (bound <
    // cutoff), so an equal-valued rival is never pruned. Otherwise the first
    // play found keeps the tie.
    if (move_equity_or_score > gen->best_move_equity_or_score ||
        (move_equity_or_score == gen->best_move_equity_or_score &&
         gen->best_small_canonical_ties &&
         best_sm->tiny_move > gen->move_list->small_moves[0]->tiny_move)) {
      need_to_update_best_move_equity_or_score = true;
      gen->best_move_equity_or_score = move_equity_or_score;
      // Swap spare_small_move into small_moves[0].
      SmallMove *swap = gen->move_list->small_moves[0];
      gen->move_list->small_moves[0] = gen->move_list->spare_small_move;
      gen->move_list->spare_small_move = swap;
      gen->move_list->count = 1;
    }
    break;
  }

  if (need_to_update_best_move_equity_or_score) {
    gen_update_cutoff_equity_or_score(gen);
  }

  if (gen->stop_on_threshold &&
      move_equity_or_score > gen->target_equity_cutoff) {
    gen->threshold_exceeded = true;
  }
}

static inline void record_tile_placement_move(MoveGen *gen, int leftstrip,
                                              int rightstrip,
                                              int main_word_score,
                                              int word_multiplier,
                                              Equity cross_score) {
  const int start_row = gen->current_row_index;
  const int start_col = leftstrip;
  const int tiles_played = gen->tiles_played;

  Equity bingo_bonus = 0;
  if (tiles_played == RACK_SIZE) {
    bingo_bonus = gen->bingo_bonus;
  }

  const Equity score =
      main_word_score * word_multiplier + cross_score + bingo_bonus;

  update_best_move_or_insert_into_movelist(
      gen, leftstrip, rightstrip, GAME_EVENT_TILE_PLACEMENT_MOVE, score,
      start_row, start_col, tiles_played, gen->dir, gen->strip);
}

static inline bool better_play_has_been_found(const MoveGen *gen,
                                              Equity highest_possible_value) {
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_ALL_SMALL:
  case MOVE_RECORD_TILES_PLAYED:
    return false;
    break;
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:
    if (gen->best_move_equity_or_score == EQUITY_INITIAL_VALUE) {
      return false;
    }
    break;
  case MOVE_RECORD_BEST_SMALL:
  case MOVE_RECORD_BEST:
    break;
  }
  Equity cutoff = gen_get_cutoff_equity_or_score(gen);
  if (cutoff == EQUITY_MAX_VALUE) {
    return false;
  }
  return highest_possible_value < cutoff;
}

static inline void record_exchange(MoveGen *gen) {
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_ALL_SMALL:
  case MOVE_RECORD_TILES_PLAYED:
    break;
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:
  case MOVE_RECORD_BEST:
    if (gen->move_sort_type == MOVE_SORT_EQUITY) {
      // An exchange's equity is its leave value plus the (<= 0, exactly
      // known) PAT baseline; the cutoff already includes the best
      // move's defense term, so compare like with like or every exchange
      // within the baseline of the cutoff gets needlessly recorded.
      const Equity leave_value =
          leave_map_get_current_value(&gen->leave_map) +
          pat_eval_non_placement_penalty(&gen->pat_eval_ctx);
      if (better_play_has_been_found(gen, leave_value)) {
        return;
      }
    }
    break;
  case MOVE_RECORD_BEST_SMALL:
    // BEST_SMALL only supports MOVE_SORT_SCORE; no exchange pruning needed.
    break;
  }

  int tiles_exchanged = 0;

  const uint16_t rack_dist_size = rack_get_dist_size(&gen->player_rack);

  for (uint16_t ml = 0; ml < rack_dist_size; ml++) {
    const uint16_t num_this = rack_get_letter(&gen->player_rack, ml);
    for (uint16_t i = 0; i < num_this; i++) {
      gen->exchange_strip[tiles_exchanged] = ml;
      tiles_exchanged++;
    }
  }

  update_best_move_or_insert_into_movelist(
      gen, 0, tiles_exchanged, GAME_EVENT_EXCHANGE, 0, 0, 0, tiles_exchanged,
      BOARD_HORIZONTAL_DIRECTION, gen->exchange_strip);
}

// Record the single best exchange from the precomputed RIT strip.
// Sets up leave_map so gen_get_static_equity reads the correct leave value.
static void record_best_exchange_from_table(MoveGen *gen) {
  int tiles_exchanged = 0;
  const MachineLetter *strip = rack_info_table_entry_get_best_exchange_strip(
      gen->rit_entry, &tiles_exchanged);
  if (tiles_exchanged == 0) {
    return;
  }
  const int leave_size = RACK_SIZE - tiles_exchanged;
  const Equity leave_value = gen->best_leaves[leave_size];
  // See record_exchange for why the defense baseline is included.
  if (better_play_has_been_found(
          gen,
          leave_value + pat_eval_non_placement_penalty(&gen->pat_eval_ctx))) {
    return;
  }
  // Temporarily set leave_map so gen_get_static_equity reads the correct
  // leave value. Save and restore index 0's value.
  const int saved_index = leave_map_get_current_index(&gen->leave_map);
  const Equity saved_value = gen->leave_map.leave_values[0];
  leave_map_set_current_index(&gen->leave_map, 0);
  gen->leave_map.leave_values[0] = leave_value;

  memcpy(gen->exchange_strip, strip, tiles_exchanged * sizeof(MachineLetter));
  update_best_move_or_insert_into_movelist(
      gen, 0, tiles_exchanged, GAME_EVENT_EXCHANGE, 0, 0, 0, tiles_exchanged,
      BOARD_HORIZONTAL_DIRECTION, gen->exchange_strip);

  // Restore leave_map state.
  gen->leave_map.leave_values[0] = saved_value;
  leave_map_set_current_index(&gen->leave_map, saved_index);
}

// Look up leave values for all subsets of the player's rack and if add_exchange
// is true, record exchange moves for them. KLV indices are retained to speed up
// lookup of leaves with common lexicographical "prefixes".
void generate_exchange_moves(MoveGen *gen, Rack *leave, uint32_t node_index,
                             uint32_t word_index, MachineLetter ml,
                             bool add_exchange) {
  const int ld_size = ld_get_size(&gen->ld);
  while (ml < ld_size && rack_get_letter(&gen->player_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    const int number_of_letters_on_rack =
        rack_get_total_letters(&gen->player_rack);
    if (number_of_letters_on_rack > 0) {
      Equity value = 0;
      if (word_index != KLV_UNFOUND_INDEX) {
        value = klv_get_indexed_leave_value(gen->klv, word_index - 1);
      }
      leave_map_set_current_value(&gen->leave_map, value);
      if (value > gen->best_leaves[rack_get_total_letters(leave)]) {
        gen->best_leaves[rack_get_total_letters(leave)] = value;
      }
      if (add_exchange) {
        record_exchange(gen);
      }
    }
  } else {
    generate_exchange_moves(gen, leave, node_index, word_index, ml + 1,
                            add_exchange);
    const uint16_t num_this = rack_get_letter(&gen->player_rack, ml);
    for (uint16_t i = 0; i < num_this; i++) {
      rack_add_letter(leave, ml);
      leave_map_take_letter_and_update_complement_index(&gen->leave_map,
                                                        &gen->player_rack, ml);
      uint32_t sibling_word_index;
      node_index = increment_node_to_ml(gen->klv, node_index, word_index,
                                        &sibling_word_index, ml);
      word_index = sibling_word_index;
      uint32_t child_word_index;
      node_index =
          follow_arc(gen->klv, node_index, word_index, &child_word_index);
      word_index = child_word_index;
      generate_exchange_moves(gen, leave, node_index, word_index, ml + 1,
                              add_exchange);
    }

    rack_take_letters(leave, ml, num_this);
    for (int i = 0; i < num_this; i++) {
      leave_map_add_letter_and_update_complement_index(&gen->leave_map,
                                                       &gen->player_rack, ml);
    }
  }
}

// Variant of generate_exchange_moves used when gen->rack_info_table is
// populated. Enumerates the same canonical subsets as generate_exchange_moves
// (so leave_map.current_index moves through the same sequence of values and
// best_leaves is updated in the same way), but skips the KLV/KWG traversal
// entirely: leave values have already been memcpy'd into
// leave_map.leave_values from the RIT, so leave_map_get_current_value
// returns the right value at each leaf.
static void generate_exchange_moves_from_table(MoveGen *gen, Rack *leave,
                                               MachineLetter ml,
                                               bool add_exchange) {
  const int ld_size = ld_get_size(&gen->ld);
  while (ml < ld_size && rack_get_letter(&gen->player_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    const int number_of_letters_on_rack =
        rack_get_total_letters(&gen->player_rack);
    if (number_of_letters_on_rack > 0) {
      const Equity value = leave_map_get_current_value(&gen->leave_map);
      if (value > gen->best_leaves[rack_get_total_letters(leave)]) {
        gen->best_leaves[rack_get_total_letters(leave)] = value;
      }
      if (add_exchange) {
        record_exchange(gen);
      }
    }
  } else {
    generate_exchange_moves_from_table(gen, leave, ml + 1, add_exchange);
    const uint16_t num_this = rack_get_letter(&gen->player_rack, ml);
    for (uint16_t i = 0; i < num_this; i++) {
      rack_add_letter(leave, ml);
      leave_map_take_letter_and_update_complement_index(&gen->leave_map,
                                                        &gen->player_rack, ml);
      generate_exchange_moves_from_table(gen, leave, ml + 1, add_exchange);
    }
    rack_take_letters(leave, ml, num_this);
    for (int i = 0; i < num_this; i++) {
      leave_map_add_letter_and_update_complement_index(&gen->leave_map,
                                                       &gen->player_rack, ml);
    }
  }
}

static inline void set_play_for_record_wmp(MoveGen *gen, Move *move,
                                           int start_col, Equity score) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  move_set_all_except_equity(
      move, gen->playthrough_marked, 0, wgen->word_length - 1, score,
      gen->current_row_index, start_col, gen->max_tiles_to_play, gen->dir,
      GAME_EVENT_TILE_PLACEMENT_MOVE);
  if (board_is_dir_vertical(gen->dir)) {
    move_set_row_start(move, start_col);
    move_set_col_start(move, gen->current_row_index);
  }
}

static inline Equity get_move_equity_for_sort_type_wmp(MoveGen *gen,
                                                       const Move *move,
                                                       Equity leave_value) {
  switch (gen->move_sort_type) {
  case MOVE_SORT_EQUITY:
    return static_eval_get_move_equity_with_leave_value(
        &gen->ld, move, &gen->leave, &gen->opponent_rack,
        gen->opening_move_word_penalties, gen->opening_move_letter_penalties,
        pat_eval_context_get_active_classes(&gen->pat_eval_ctx),
        &gen->pat_eval_ctx, gen->board_number_of_tiles_played,
        gen->number_of_tiles_in_bag, leave_value);
  case MOVE_SORT_SCORE:
    return move_get_score(move);
  default:
    // TODO(olaugh): make move_sort_type an enum and remove default case
    log_fatal("unhandled move sort type: %d", gen->move_sort_type);
  }
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#else
  return move_get_score(move);
#endif
}

// On a nonempty board, static equity depends only on the score, leave, racks,
// and number of tiles played. Compute it before writing a full Move so bounded
// record modes can discard a losing candidate without copying its strip.
// Opening placement penalties inspect the Move and use the regular path
// instead.
//
// The tile count is gen->max_tiles_to_play, which wordmap_gen sets to the
// anchor's tiles_to_play: every word for a size-k subrack plays exactly k
// tiles, so it is what set_play_for_record_wmp writes into the Move. If a WMP
// anchor ever emitted plays of varying length, the pre-endgame adjustment
// (bag_plus_rack_size < PEG_ADJUST_VALUES_LENGTH) would be the only place
// this went wrong, and it would do so silently.
static inline bool get_wmp_equity_without_move(const MoveGen *gen, Equity score,
                                               Equity leave_value,
                                               Equity *equity) {
  switch (gen->move_sort_type) {
  case MOVE_SORT_SCORE:
    *equity = score;
    return true;
  case MOVE_SORT_EQUITY:
    if (gen->board_number_of_tiles_played == 0) {
      return false;
    }
    *equity = static_eval_get_nonopening_move_equity(
        &gen->ld, &gen->leave, &gen->opponent_rack, gen->max_tiles_to_play,
        gen->number_of_tiles_in_bag, score, leave_value);
    return true;
  default:
    log_fatal("unhandled move sort type: %d", gen->move_sort_type);
  }
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#else
  return false;
#endif
}

static inline void
update_best_move_or_insert_into_movelist_wmp(MoveGen *gen, int start_col,
                                             Equity score, Equity leave_value) {
  bool need_to_update_best_move_equity_or_score = false;
  Equity move_equity_or_score = 0;
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST: {
    Equity precomputed_equity = 0;
    const bool has_precomputed_equity = get_wmp_equity_without_move(
        gen, score, leave_value, &precomputed_equity);
    if (has_precomputed_equity && !gen->stop_on_threshold) {
      // The defense term is <= 0 except for the utility correction, whose
      // largest possible value keeps these upper-bound skips sound.
      const Equity equity_bound =
          precomputed_equity + pat_eval_utility_bound(&gen->pat_eval_ctx);
      if (gen->move_record_type == MOVE_RECORD_ALL &&
          move_list_get_count(gen->move_list) ==
              move_list_get_capacity(gen->move_list) &&
          equity_bound < move_list_peek_equity(gen->move_list)) {
        return;
      }
      if (gen->move_record_type == MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST &&
          gen->best_move_equity_or_score != EQUITY_INITIAL_VALUE &&
          equity_bound < gen_get_cutoff_equity_or_score(gen)) {
        return;
      }
    }
    Move *move = move_list_get_spare_move(gen->move_list);
    set_play_for_record_wmp(gen, move, start_col, score);
    if (has_precomputed_equity) {
      // precomputed_equity is score plus leave only (see
      // get_wmp_equity_without_move); the defense term still has to be
      // added, exactly as get_move_equity_for_sort_type_wmp would. It is
      // <= 0, so the list-full and cutoff skips above, which compared the
      // precomputed value, remain sound upper-bound skips.
      move_equity_or_score = precomputed_equity;
      if (gen->move_sort_type == MOVE_SORT_EQUITY && gen_pat_is_active(gen)) {
        move_equity_or_score +=
            pat_eval_move_penalty(&gen->pat_eval_ctx, move, &gen->leave);
      }
    } else {
      move_equity_or_score =
          get_move_equity_for_sort_type_wmp(gen, move, leave_value);
    }
    if (gen->move_record_type == MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST) {
      // This updates the cutoff move internally so no update will be pending
      // afterward.
      gen_insert_spare_move_within_x_equity_of_best(gen, move_equity_or_score);
    } else {
      move_list_insert_spare_move(gen->move_list, move_equity_or_score);
    }
    break;
  }
  case MOVE_RECORD_BEST: {
    Equity precomputed_equity = 0;
    const bool has_precomputed_equity = get_wmp_equity_without_move(
        gen, score, leave_value, &precomputed_equity);
    if (has_precomputed_equity &&
        precomputed_equity + pat_eval_utility_bound(&gen->pat_eval_ctx) <
            move_get_equity(gen_get_readonly_best_move(gen))) {
      return;
    }
    Move *current_move = gen_get_current_move(gen);
    set_play_for_record_wmp(gen, current_move, start_col, score);
    if (gen->move_sort_type == MOVE_SORT_EQUITY && gen_pat_is_active(gen) &&
        !gen->stop_on_threshold) {
      // See the lazy defense-term comment on gen_pat_is_active.
      // precomputed_equity (when available) already equals what a fresh
      // static_eval_get_move_equity_with_leave_value(..., pat_eval_ctx=NULL,
      // ...) call would return here: WMP-generated plays that reach this
      // point always have board_number_of_tiles_played > 0 (see
      // get_wmp_equity_without_move), so the opening-adjustment term is
      // always zero and the two computations coincide.
      const Equity equity_without_pat =
          has_precomputed_equity
              ? precomputed_equity
              : static_eval_get_move_equity_with_leave_value(
                    &gen->ld, current_move, &gen->leave, &gen->opponent_rack,
                    gen->opening_move_word_penalties,
                    gen->opening_move_letter_penalties,
                    pat_eval_context_get_active_classes(&gen->pat_eval_ctx),
                    NULL, gen->board_number_of_tiles_played,
                    gen->number_of_tiles_in_bag, leave_value);
      move_equity_or_score = gen_pat_contender_equity(
          gen, current_move, &gen->leave, equity_without_pat);
    } else {
      move_equity_or_score = has_precomputed_equity
                                 ? precomputed_equity
                                 : get_move_equity_for_sort_type_wmp(
                                       gen, current_move, leave_value);
    }
    move_set_equity(current_move, move_equity_or_score);
    // A best move carried in from an earlier generation (see
    // gen_seed_carried_best_move) is found again; only a better move
    // replaces it.
    if (compare_moves(current_move, gen_get_readonly_best_move(gen), true) ==
        1) {
      need_to_update_best_move_equity_or_score = true;
      gen_switch_best_move_and_current_move(gen);
    }
    break;
  }
  case MOVE_RECORD_ALL_SMALL:
  case MOVE_RECORD_TILES_PLAYED:
  case MOVE_RECORD_BEST_SMALL:
    log_fatal("update_best_move_or_insert_into_movelist_wmp called with "
              "MOVE_RECORD_ALL_SMALL, MOVE_RECORD_TILES_PLAYED, or "
              "MOVE_RECORD_BEST_SMALL");
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
    __builtin_unreachable();
#else
    return;
#endif
  }
  if (need_to_update_best_move_equity_or_score) {
    gen_update_cutoff_equity_or_score(gen);
  }

  if (gen->stop_on_threshold &&
      move_equity_or_score > gen->target_equity_cutoff) {
    gen->threshold_exceeded = true;
  }
}

// The score of the play gen->playthrough_marked spells from start_col.
static inline __attribute__((always_inline)) Equity
wmp_play_score(const MoveGen *gen, int start_col) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  const Equity bingo_bonus =
      gen->max_tiles_to_play == RACK_SIZE ? gen->bingo_bonus : 0;
  Equity played_score_total = 0;
  Equity playthrough_score_total = 0;
  // from already played letters hooked
  Equity hooked_cross_total = 0;
  // from newly played tiles counting both ways due to crossing
  Equity played_cross_total = 0;
  int word_multiplier = 1;
  for (int letter_idx = 0; letter_idx < wgen->word_length; letter_idx++) {
    const int board_col = letter_idx + start_col;
    MachineLetter ml = gen->playthrough_marked[letter_idx];
    if (ml != PLAYED_THROUGH_MARKER) {
      const Equity tile_score = gen->tile_scores[ml];
      const BonusSquare bonus_square =
          gen_cache_get_bonus_square(gen, board_col);
      const int this_word_multiplier =
          bonus_square_get_word_multiplier(bonus_square);
      const Equity hooked_cross_score =
          gen_cache_get_cross_score(gen, board_col) * this_word_multiplier;
      hooked_cross_total += hooked_cross_score;
      const int is_cross_word =
          gen_cache_get_is_cross_word(gen, board_col) ? 1 : 0;
      const int letter_multiplier =
          bonus_square_get_letter_multiplier(bonus_square);
      played_cross_total +=
          is_cross_word * tile_score * this_word_multiplier * letter_multiplier;
      played_score_total += tile_score * letter_multiplier;
      word_multiplier *= this_word_multiplier;
    } else {
      ml = gen_cache_get_letter(gen, board_col);
      playthrough_score_total += gen->tile_scores[ml];
    }
  }
  return (played_score_total + playthrough_score_total) * word_multiplier +
         hooked_cross_total + played_cross_total + bingo_bonus;
}

void record_wmp_play(MoveGen *gen, int start_col, Equity leave_value) {
  update_best_move_or_insert_into_movelist_wmp(
      gen, start_col, wmp_play_score(gen, start_col), leave_value);
}

void get_blank_possibilities(const MoveGen *gen,
                             const BitRack *nonplaythrough_tiles,
                             int current_pos, bool *can_be_unblanked,
                             bool *can_be_blanked) {
  const MachineLetter target_letter = gen->playthrough_marked[current_pos];
  const int count_of_target_letter_in_subrack =
      bit_rack_get_letter(nonplaythrough_tiles, target_letter);

  int count_before_current_pos = 0;
  int count_at_or_after_current_pos = 0;
  for (int pos = 0; pos < gen->wmp_move_gen.word_length; pos++) {
    const MachineLetter playthrough_letter = gen->playthrough_marked[pos];
    if (playthrough_letter == PLAYED_THROUGH_MARKER ||
        get_is_blanked(playthrough_letter)) {
      // Previously placed board tiles and already-blanked letters do not
      // consume non-blank tiles from the subrack.
      continue;
    }
    if (playthrough_letter != target_letter) {
      continue;
    }
    if (pos < current_pos) {
      count_before_current_pos++;
    } else {
      count_at_or_after_current_pos++;
    }
  }

  const int total_nonblank_in_word =
      count_before_current_pos + count_at_or_after_current_pos;

  // We can place a non-blank tile here if we have one left on the subrack
  // after accounting for the ones required before this position.
  const int target_letter_remaining_on_rack =
      count_of_target_letter_in_subrack - count_before_current_pos;
  *can_be_unblanked = target_letter_remaining_on_rack > 0;

  // We can (and must) use a blank for this letter if the number of non-blank
  // tiles required by the word exceeds what is available on the subrack.
  *can_be_blanked = total_nonblank_in_word > count_of_target_letter_in_subrack;
}

void record_wmp_plays_for_word(MoveGen *gen, int subrack_idx, int start_col,
                               int blanks_so_far, int pos) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  const BitRack *nonplaythrough_tiles =
      wmp_move_gen_get_nonplaythrough_subrack(wgen, subrack_idx);
  const int num_blanks =
      bit_rack_get_letter(nonplaythrough_tiles, BLANK_MACHINE_LETTER);
  if (num_blanks == blanks_so_far) {
    record_wmp_play(gen, start_col, wgen->leave_value);
    return;
  }
  assert(pos < wgen->word_length);
  const MachineLetter ml = gen->playthrough_marked[pos];
  if (ml == PLAYED_THROUGH_MARKER) {
    record_wmp_plays_for_word(gen, subrack_idx, start_col, blanks_so_far,
                              pos + 1);
    return;
  }
  bool can_be_unblanked;
  bool can_be_blanked;
  get_blank_possibilities(gen, nonplaythrough_tiles, pos, &can_be_unblanked,
                          &can_be_blanked);
  if (can_be_unblanked) {
    record_wmp_plays_for_word(gen, subrack_idx, start_col, blanks_so_far,
                              pos + 1);
  }
  if (can_be_blanked) {
    gen->playthrough_marked[pos] = get_blanked_machine_letter(ml);
    record_wmp_plays_for_word(gen, subrack_idx, start_col, blanks_so_far + 1,
                              pos + 1);
    gen->playthrough_marked[pos] = ml;
  }
}

// Index of the lowest set bit of a nonzero bitset, following
// get_single_bit_index's guarded-intrinsic pattern. Undefined for zero, so
// callers loop on `bitset != 0`.
static inline int lowest_set_bit_index(uint32_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctz)
  return __builtin_ctz(bitset);
#else
  int index = 0;
  while ((bitset & 1U) == 0) {
    bitset >>= 1U;
    index++;
  }
  return index;
#endif
}

// lowest_set_bit_index for a 64-bit bitset.
static inline int lowest_set_bit_index_64(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(bitset);
#else
  int index = 0;
  while ((bitset & 1U) == 0) {
    bitset >>= 1U;
    index++;
  }
  return index;
#endif
}

bool wordmap_gen_check_playthrough_and_crosses(MoveGen *gen, int word_idx,
                                               int start_col,
                                               uint32_t playthrough_positions) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  const MachineLetter *word = wmp_move_gen_get_word(wgen, word_idx);
  if (playthrough_positions != 0) {
    // WMP words are anagrams of the rack plus the playthrough tiles, but most
    // anagrams do not put those tiles at the board's fixed positions. Check
    // every fixed position before spending time on cross sets at empty
    // squares. The positions were collected while building the playthrough
    // BitRack, so this phase visits only actual board tiles.
    uint32_t positions = playthrough_positions;
    while (positions != 0) {
      const int board_col = lowest_set_bit_index(positions);
      const int letter_idx = board_col - start_col;
      assert(board_col < BOARD_DIM);
      assert(board_col >= 0);
      assert(letter_idx >= 0);
      assert(letter_idx < wgen->word_length);
      const MachineLetter board_letter =
          get_unblanked_machine_letter(gen_cache_get_letter(gen, board_col));
      assert(board_letter != ALPHABET_EMPTY_SQUARE_MARKER);
      assert(
          !bonus_square_is_brick(gen_cache_get_bonus_square(gen, board_col)));
      if (board_letter != word[letter_idx]) {
        return false;
      }
      gen->playthrough_marked[letter_idx] = PLAYED_THROUGH_MARKER;
      positions &= positions - 1;
    }
    for (int letter_idx = 0; letter_idx < wgen->word_length; letter_idx++) {
      const int board_col = start_col + letter_idx;
      if ((playthrough_positions & (1U << board_col)) != 0) {
        continue;
      }
      const MachineLetter word_letter = word[letter_idx];
      if (!board_is_letter_allowed_in_cross_set(
              gen_cache_get_cross_set(gen, board_col), word_letter)) {
        return false;
      }
      gen->playthrough_marked[letter_idx] = word_letter;
    }
    return true;
  }
  // A zero mask means this anchor has no fixed letters. Check only crosses.
  for (int letter_idx = 0; letter_idx < wgen->word_length; letter_idx++) {
    assert(start_col + letter_idx >= 0);
    assert(start_col + letter_idx < BOARD_DIM);
    assert(gen_cache_is_empty(gen, start_col + letter_idx));
    const MachineLetter word_letter = word[letter_idx];
    if (!board_is_letter_allowed_in_cross_set(
            gen_cache_get_cross_set(gen, start_col + letter_idx),
            word_letter)) {
      return false;
    }
    gen->playthrough_marked[letter_idx] = word_letter;
  }
  return true;
}

// The anchor's score bound plus this subrack's leave bounds the equity of
// every play the subrack can make, so under equity sort a subrack whose bound
// cannot reach the cutoff is skipped. Under score sort the cutoff is a score,
// which the anchor-level check already bounded; adding a leave there would
// skip subracks that still hold plays above the cutoff. This is an equity
// upper bound independent of the anchor's highest_possible_score, so any
// positive equity term added to the real evaluation must be added here too:
// anchor_pat_bound (see gen_wmp_anchor_pat_bound) folds in the PAT term's
// contribution, which includes the largest utility correction the anchor's
// plays can get and so can be positive; the opening placement_adjustment
// (always <= 0) is soundly
// omitted; see static_eval_get_shadow_equity. The candidate mask applies the
// same bound (see wordmap_gen_candidate_subracks).
static inline __attribute__((always_inline)) bool
wordmap_gen_leave_prunes_subrack(const MoveGen *gen, const Anchor *anchor,
                                 int subrack_idx, Equity anchor_pat_bound) {
  if (!gen->wmp_prune_subracks_by_leave) {
    return false;
  }
  const Equity leave_value =
      wmp_move_gen_get_leave_value(&gen->wmp_move_gen, subrack_idx);
  return better_play_has_been_found(
      gen, leave_value + anchor->highest_possible_score + anchor_pat_bound);
}

// With the bag empty, a play is priced by its actual leave, not a leave value:
// sets gen->leave to the rack less the subrack, and the leave value to 0.
static inline __attribute__((always_inline)) void
wordmap_gen_set_empty_bag_leave(MoveGen *gen, const Anchor *anchor,
                                int subrack_idx) {
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  wgen->leave_value = 0;
  for (int ml = 0; ml < ld_get_size(&gen->ld); ml++) {
    const int leave_num_ml =
        rack_get_letter(&gen->player_rack, ml) -
        bit_rack_get_letter(
            wmp_move_gen_get_nonplaythrough_subrack(wgen, subrack_idx), ml);
    rack_set_letter(&gen->leave, ml, leave_num_ml);
  }
  rack_set_total_letters(&gen->leave,
                         rack_get_total_letters(&gen->player_rack) -
                             anchor->tiles_to_play);
}

// Probes the WMP for one canonical subrack of the current anchor and records
// every play its words allow. Returns true once the recording threshold has
// been exceeded, so the caller stops scanning subracks.
//
// Its one call site is the subrack walk in wordmap_gen, which inlines into
// generate_moves. Keep it the only one: a second inlined copy there costs
// several percent of sim throughput, and with two call sites and no attribute
// clang keeps it out of line, so the walk makes a call per subrack (about -6%
// with WIT off).
static inline __attribute__((always_inline)) bool
wordmap_gen_record_subrack(MoveGen *gen, const Anchor *anchor, int subrack_idx,
                           bool lazy, Equity anchor_pat_bound) {
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  // record_wmp_play prices every play below with wgen->leave_value. Only
  // equity sort with tiles in the bag reads the subrack's leave, and that is
  // when the leave prune is on; an empty bag sets it to 0 below, and score
  // sort ignores it.
  if (gen->wmp_prune_subracks_by_leave) {
    wgen->leave_value = wmp_move_gen_get_leave_value(wgen, subrack_idx);
  }
  if (wordmap_gen_leave_prunes_subrack(gen, anchor, subrack_idx,
                                       anchor_pat_bound)) {
    return false;
  }
  if (!wmp_move_gen_get_subrack_words(wgen, subrack_idx, lazy,
                                      &gen->blank_word_cache)) {
    return false;
  }
  if (gen->number_of_tiles_in_bag == 0) {
    wordmap_gen_set_empty_bag_leave(gen, anchor, subrack_idx);
  }
  for (int word_idx = 0; word_idx < wgen->num_words; word_idx++) {
    for (int start_col = anchor->leftmost_start_col;
         start_col <= anchor->rightmost_start_col; start_col++) {
      if (wordmap_gen_check_playthrough_and_crosses(
              gen, word_idx, start_col, wgen->playthrough_positions)) {
        record_wmp_plays_for_word(gen, subrack_idx, start_col, 0, 0);
        if (gen->threshold_exceeded) {
          return true;
        }
      }
    }
  }
  return false;
}

// The candidate mask for subracks first_idx onward, from
// wmp_move_gen_get_candidate_subracks with the leave prune as it stands now.
//
// better_play_has_been_found(gen, bound) is exactly bound < cutoff when it
// holds for EQUITY_MIN_VALUE, and otherwise false for every bound a leave plus
// a score can reach, so nothing is dropped. Dropping a subrack whose bound is
// already below the cutoff is safe because the cutoff never falls while plays
// are recorded: gen_update_cutoff_equity_or_score takes it from the best move,
// which is only replaced by a move that compare_moves ranks higher and so has
// at least its equity (MOVE_RECORD_BEST), or from best_move_equity_or_score,
// which only grows (MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST). That function
// asserts it. MOVE_RECORD_BEST_SMALL is score-sort only, so the prune is never
// on there. The check also stays active once it is, since a real move's equity
// never reaches EQUITY_MAX_VALUE. So wordmap_gen_leave_prunes_subrack would
// skip the subrack when the walk reaches it.
static inline __attribute__((always_inline)) uint64_t
wordmap_gen_candidate_subracks(const MoveGen *gen, const Anchor *anchor,
                               int first_idx, int count, bool lazy,
                               Equity anchor_pat_bound,
                               uint64_t *filter_misses) {
  const bool prune_by_leave = gen->wmp_prune_subracks_by_leave &&
                              better_play_has_been_found(gen, EQUITY_MIN_VALUE);
  // The PAT bound joins the anchor's score bound, exactly as in
  // wordmap_gen_leave_prunes_subrack; dropping it would be unsound, since
  // it can be positive.
  return wmp_move_gen_get_candidate_subracks(
      &gen->wmp_move_gen, first_idx, count, lazy, prune_by_leave,
      anchor->highest_possible_score + anchor_pat_bound,
      gen_get_cutoff_equity_or_score(gen), filter_misses);
}

// Bit i of the result is set unless subrack first_idx + i holds a WIT
// forbidden letter. Out of line: with WIT off the forbidden set is always
// empty and this never runs.
static __attribute__((noinline)) uint64_t wordmap_gen_allowed_subracks(
    const WMPMoveGen *wgen, int first_idx, int count,
    uint64_t forbidden_subrack_low, uint64_t forbidden_subrack_high) {
  uint64_t allowed = 0;
  for (int bit_idx = 0; bit_idx < count; bit_idx++) {
    const BitRack *subrack =
        wmp_move_gen_get_nonplaythrough_subrack(wgen, first_idx + bit_idx);
    allowed |= (uint64_t)(!bit_rack_intersects_mask(
                   subrack, forbidden_subrack_low, forbidden_subrack_high))
               << bit_idx;
  }
  return allowed;
}

// Resolves the filter misses in `misses`, bits counted from subrack
// first_idx, as wordmap_gen_record_subrack would on reaching each one: unless
// the leave check skips it, its lookup returns NULL and that is stored. Out
// of line to keep the walk in generate_moves small; it has work only while a
// lazily resolved rack still has unresolved subracks of the anchor's size.
static __attribute__((noinline)) void
wordmap_gen_resolve_filter_misses(MoveGen *gen, const Anchor *anchor,
                                  int first_idx, uint64_t misses,
                                  Equity anchor_pat_bound) {
  while (misses != 0) {
    const int subrack_idx = first_idx + wmp_move_gen_get_lowest_set_bit(misses);
    misses &= misses - 1;
    if (!wordmap_gen_leave_prunes_subrack(gen, anchor, subrack_idx,
                                          anchor_pat_bound)) {
      wmp_move_gen_resolve_filter_miss(&gen->wmp_move_gen, subrack_idx);
    }
  }
}

// Clears the lowest set bit, so a loop can visit each set bit in turn:
// subtracting one flips that bit and everything below it, and the AND keeps
// only the bits above it.
static inline uint32_t clear_lowest_set_bit(uint32_t bitset) {
  return bitset & (bitset - 1U);
}

static inline __attribute__((always_inline)) void
wordmap_gen(MoveGen *gen, const Anchor *anchor, bool lazy) {
  assert(gen != NULL);
  assert(anchor != NULL);
  // Every play recorded for this anchor uses exactly this many rack tiles;
  // get_wmp_equity_without_move relies on that to price a candidate before
  // its Move exists.
  gen->max_tiles_to_play = anchor->tiles_to_play;
  assert(anchor->tiles_to_play <= rack_get_total_letters(&gen->player_rack));
  WMPMoveGen *wgen = &gen->wmp_move_gen;

  // Inline bingo fast path: for nonplaythrough full-rack plays with
  // precomputed bingo words, skip subrack enumeration and WMP lookup.
  // Uses record_wmp_plays_for_word with subrack_idx=0 (the only subrack
  // for a full-rack play) so blank assignment is handled correctly.
  if (gen->rit_entry != NULL && anchor->tiles_to_play == RACK_SIZE &&
      anchor->playthrough_blocks == 0) {
    const int num_bingos = gen->rit_entry->num_bingo_words;
    if (num_bingos > 0) {
      wgen->word_length = RACK_SIZE;
      wgen->tiles_to_play = RACK_SIZE;
      // Leave value is 0 for bingo (all tiles played), leave is empty.
      wgen->leave_value = 0;
      if (gen->number_of_tiles_in_bag == 0) {
        rack_reset(&gen->leave);
      }
      for (int bingo_idx = 0; bingo_idx < num_bingos; bingo_idx++) {
        // Point the word list at the RIT's inline word (no copy).
        wgen->words = gen->rit_entry->bingo_words[bingo_idx];
        wgen->num_words = 1;
        for (int start_col = anchor->leftmost_start_col;
             start_col <= anchor->rightmost_start_col; start_col++) {
          if (wordmap_gen_check_playthrough_and_crosses(gen, 0, start_col, 0)) {
            record_wmp_plays_for_word(gen, 0, start_col, 0, 0);
            if (gen->threshold_exceeded) {
              return;
            }
          }
        }
      }
      return;
    }
  }

  wmp_move_gen_set_playthrough_bit_rack(
      wgen, anchor, gen->row_squares,
      gen->word_info_table != NULL ? gen->wit_row_lane : NULL,
      gen->wit_len_lane, gen->word_info_table);
  wmp_move_gen_playthrough_metadata_init(wgen, anchor);

  assert(anchor->leftmost_start_col <= anchor->rightmost_start_col);
  assert(anchor->leftmost_start_col <= anchor->col);
  assert(anchor->rightmost_start_col <= anchor->col);
  const int num_subrack_combinations =
      wmp_move_gen_get_num_subrack_combinations(wgen);
  uint64_t forbidden_subrack_low = 0;
  uint64_t forbidden_subrack_high = 0;

  // Word info table prune, lifted entirely out of the loops below. The block
  // scan in wmp_move_gen_set_playthrough_bit_rack already AND-folded each
  // playthrough block's letter set at this word length into
  // wgen->playthrough_addable: every placed tile must be one of those letters.
  // So the rack must supply tiles_to_play tiles that are each addable (blanks
  // are wild). Count the rack tiles that are NOT placeable -- present non-blank
  // letters outside addable, usually just a couple of set bits -- and skip the
  // whole anchor if too few remain. Otherwise, use the same exact condition to
  // skip individual canonical subracks before hashing or probing the WMP. The
  // blocks (hence addable) are the same for every start column.
  if (gen->word_info_table != NULL && anchor->playthrough_blocks > 0) {
    const uint32_t letter_universe =
        (uint32_t)((1U << ld_get_size(&gen->ld)) - 1) & ~1U;
    const uint32_t addable = wgen->playthrough_addable & letter_universe;
    if (addable == 0) {
      return;
    }
    const uint32_t rack_present =
        (uint32_t)gen->rack_cross_set & letter_universe;
    uint32_t forbidden = rack_present & ~addable;
    int forbidden_count = 0;
    while (forbidden != 0) {
      const MachineLetter forbidden_ml =
          (MachineLetter)lowest_set_bit_index(forbidden);
      forbidden_count += rack_get_letter(&gen->player_rack, forbidden_ml);
      // BitRack stores a four-bit count for each machine letter. Mask the
      // entire lane so any positive count rejects the subrack.
      bit_rack_add_letter_to_mask(&forbidden_subrack_low,
                                  &forbidden_subrack_high, forbidden_ml);
      forbidden = clear_lowest_set_bit(forbidden);
    }
    if (gen->number_of_letters_on_rack - forbidden_count <
        anchor->tiles_to_play) {
      return;
    }
  }

  // Neither whole-anchor rejection above needs combined subracks. Build
  // them sequentially once for the subrack walk below.
  wmp_move_gen_build_playthrough_subracks(wgen);

  // Visit only the candidate subracks, in index order; every other subrack
  // would record nothing. A full scan would also have resolved each lazy
  // filter miss to NULL when it reached it, under the cutoff left by the
  // candidates below it. So each miss is resolved at that point: just before
  // the next higher candidate is recorded, or after the last one. Misses past
  // a threshold return stay unresolved, as they would have.
  //
  // The forbidden set is the same for every subrack, so the walk tests it
  // once per chunk and pays nothing for it when it is empty, as it always is
  // with WIT off.
  const bool has_forbidden =
      bit_rack_mask_has_letters(forbidden_subrack_low, forbidden_subrack_high);
  // Upper bound on the PAT term of every move from this anchor (zero when
  // the term is off). See wordmap_gen_leave_prunes_subrack.
  const Equity anchor_pat_bound =
      gen_wmp_anchor_pat_bound(gen, anchor->dir, anchor->row, anchor);
  for (int first_idx = 0; first_idx < num_subrack_combinations;
       first_idx += WMP_MOVE_GEN_SUBRACKS_PER_MASK) {
    const int remaining = num_subrack_combinations - first_idx;
    const int count = remaining < WMP_MOVE_GEN_SUBRACKS_PER_MASK
                          ? remaining
                          : WMP_MOVE_GEN_SUBRACKS_PER_MASK;
    uint64_t filter_misses = 0;
    uint64_t candidates = wordmap_gen_candidate_subracks(
        gen, anchor, first_idx, count, lazy, anchor_pat_bound, &filter_misses);
    if (has_forbidden) {
      const uint64_t allowed = wordmap_gen_allowed_subracks(
          wgen, first_idx, count, forbidden_subrack_low,
          forbidden_subrack_high);
      candidates &= allowed;
      filter_misses &= allowed;
    }
    while (candidates != 0) {
      const int bit_idx = wmp_move_gen_get_lowest_set_bit(candidates);
      candidates &= candidates - 1;
      const uint64_t misses_below = filter_misses & ((1ULL << bit_idx) - 1);
      if (misses_below != 0) {
        wordmap_gen_resolve_filter_misses(gen, anchor, first_idx, misses_below,
                                          anchor_pat_bound);
        filter_misses ^= misses_below;
      }
      if (wordmap_gen_record_subrack(gen, anchor, first_idx + bit_idx, lazy,
                                     anchor_pat_bound)) {
        return;
      }
    }
    if (filter_misses != 0) {
      wordmap_gen_resolve_filter_misses(gen, anchor, first_idx, filter_misses,
                                        anchor_pat_bound);
    }
  }
}

// Wordsmog with the WMP. A subrack whose letters and the playthrough letters
// are an anagram of a word forms a valid main word in every arrangement, so
// each of the anchor's start columns gets one play per such subrack: its best
// arrangement on the placement's empty squares (see wordsmog_arrangement.h).

// The tiles of a subrack for wordsmog_best_arrangement: its natural letters,
// its blanks, and the designations that keep main_word, the subrack plus the
// playthrough letters, an anagram of a word. entry is main_word's WMP entry.
static void wordsmog_tiles_init(const MoveGen *gen, const BitRack *subrack,
                                const BitRack *main_word, const WMPEntry *entry,
                                WordsmogTiles *tiles) {
  const int ld_size = ld_get_size(&gen->ld);
  tiles->num_letters = 0;
  tiles->vowels = 0;
  for (int ml = 1; ml < ld_size; ml++) {
    if (ld_get_is_vowel(&gen->ld, ml)) {
      tiles->vowels |= 1U << ml;
    }
    const int count = bit_rack_get_letter(subrack, ml);
    if (count == 0) {
      continue;
    }
    tiles->letters[tiles->num_letters] = (MachineLetter)ml;
    tiles->counts[tiles->num_letters] = count;
    tiles->scores[tiles->num_letters] = gen->tile_scores[ml];
    tiles->num_letters++;
  }
  tiles->num_blanks = bit_rack_get_letter(subrack, BLANK_MACHINE_LETTER);
  tiles->blank_score =
      gen->tile_scores[get_blanked_machine_letter(BLANK_MACHINE_LETTER)];
  tiles->blank_letters = 0;
  memset(tiles->pair_letters, 0, sizeof(tiles->pair_letters));
  if (tiles->num_blanks == 1) {
    tiles->blank_letters = entry->blank_letters;
  } else if (tiles->num_blanks == 2) {
    // A double-blank entry holds the lower letter of each pair. The higher
    // ones are the designations of the other blank once the lower one is
    // fixed, at or above it.
    const int word_length = gen->wmp_move_gen.word_length;
    for (uint32_t lows = entry->first_blank_letters & ~1U; lows != 0;
         lows &= lows - 1) {
      const MachineLetter low = (MachineLetter)lowest_set_bit_index(lows);
      BitRack one_blank = *main_word;
      bit_rack_take_letter(&one_blank, BLANK_MACHINE_LETTER);
      bit_rack_add_letter(&one_blank, low);
      const WMPEntry *one_blank_entry = wfl_get_blank_entry(
          &gen->wmp_move_gen.wmp->wfls[word_length], &one_blank);
      if (one_blank_entry != NULL) {
        tiles->pair_letters[low] =
            one_blank_entry->blank_letters & ~((1U << low) - 1U);
      }
    }
  }
  wordsmog_tiles_set_states(tiles);
}

// The empty squares of the current anchor's placement at start_col, in board
// order, for wordsmog_best_arrangement, and their positions in the word in
// square_letter_idxs. Marks the board's tiles in gen->playthrough_marked, and
// writes the part of every play's score there that does not depend on its
// tiles to base_score. Returns how many squares there are.
static int wordsmog_placement_squares(MoveGen *gen, int start_col,
                                      WordsmogSquare squares[RACK_SIZE],
                                      int square_letter_idxs[RACK_SIZE],
                                      Equity *base_score) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  // The opening move's placement penalty is an equity term that depends on
  // where the tiles go, less the parts the PAT term replaces, as
  // placement_adjustment prices it. The PAT term itself is left to
  // wordsmog_pat_refine.
  const bool opening = gen->move_sort_type == MOVE_SORT_EQUITY &&
                       gen->board_number_of_tiles_played == 0;
  const uint32_t pat_classes =
      pat_eval_context_get_active_classes(&gen->pat_eval_ctx);
  const bool word_penalties = (pat_classes & PAT_CLASS_MASK_WORD_MULT) == 0;
  const bool letter_penalties = (pat_classes & PAT_CLASS_MASK_LETTER_MULT) == 0;
  const int penalty_offset = board_is_dir_vertical(gen->dir) ? BOARD_DIM : 0;
  // A square with no cross word allows every bit; keep the alphabet's letters.
  const uint64_t letters =
      (((uint64_t)1 << ld_get_size(&gen->ld)) - 1) & ~(uint64_t)1;
  int num_squares = 0;
  int word_multiplier = 1;
  Equity playthrough_score = 0;
  Equity hooked_cross_score = 0;
  for (int letter_idx = 0; letter_idx < wgen->word_length; letter_idx++) {
    const int board_col = start_col + letter_idx;
    if ((wgen->playthrough_positions & (1U << board_col)) != 0) {
      gen->playthrough_marked[letter_idx] = PLAYED_THROUGH_MARKER;
      playthrough_score +=
          gen->tile_scores[gen_cache_get_letter(gen, board_col)];
      continue;
    }
    assert(num_squares < RACK_SIZE);
    const int this_word_multiplier = bonus_square_get_word_multiplier(
        gen_cache_get_bonus_square(gen, board_col));
    word_multiplier *= this_word_multiplier;
    hooked_cross_score +=
        gen_cache_get_cross_score(gen, board_col) * this_word_multiplier;
    square_letter_idxs[num_squares] = letter_idx;
    squares[num_squares].cross_set =
        gen_cache_get_cross_set(gen, board_col) & letters;
    Equity vowel_adjustment = 0;
    if (opening && word_penalties) {
      vowel_adjustment +=
          gen->opening_move_word_penalties[penalty_offset + board_col];
    }
    if (opening && letter_penalties) {
      vowel_adjustment +=
          gen->opening_move_letter_penalties[penalty_offset + board_col];
    }
    squares[num_squares].vowel_adjustment = vowel_adjustment;
    num_squares++;
  }
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    const int board_col = start_col + square_letter_idxs[square_idx];
    const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, board_col);
    const int cross_word_multiplier =
        gen_cache_get_is_cross_word(gen, board_col)
            ? bonus_square_get_word_multiplier(bonus_square)
            : 0;
    squares[square_idx].coefficient =
        bonus_square_get_letter_multiplier(bonus_square) *
        (word_multiplier + cross_word_multiplier);
  }
  *base_score = playthrough_score * word_multiplier + hooked_cross_score +
                (gen->max_tiles_to_play == RACK_SIZE ? gen->bingo_bonus : 0);
  return num_squares;
}

// Whether no play at a placement whose scores are at most score_bound can be
// recorded: as wordmap_gen_leave_prunes_subrack, with the placement's own
// score bound in place of the anchor's, under equity sort with tiles in the
// bag; by score alone under score sort.
static inline bool wordsmog_placement_pruned(const MoveGen *gen,
                                             Equity score_bound,
                                             Equity anchor_pat_bound) {
  if (gen->wmp_prune_subracks_by_leave) {
    return better_play_has_been_found(gen, score_bound + anchor_pat_bound +
                                               gen->wmp_move_gen.leave_value);
  }
  return gen->move_sort_type == MOVE_SORT_SCORE &&
         better_play_has_been_found(gen, score_bound);
}

// With the PAT term on, the best play of a placement and tile multiset, a
// group, need not be its best arrangement by every other term, since the term
// depends on which tile goes on which square and which letter each blank
// stands for. Under MOVE_RECORD_BEST, move generation records each group's
// best arrangement, priced with the term, and keeps the group;
// wordsmog_pat_refine then prices more of the groups' plays, a round at a
// time: every group that could still hold the best play gets its next play
// priced before any group gets another. It stops once gen->wordsmog_pat_evals
// plays have been priced in all, or once no play left could beat the best.
// Without a limit it prices every play that could be best, highest bound
// first, which is exact.
//
// Under MOVE_RECORD_ALL the list holds one play per group, as without the
// term: move generation prices every group's best arrangement, the rounds
// look for better arrangements of the groups that could still make the list,
// and each group's best play is recorded at the end. There every group's
// first play is priced anyway, so gen->wordsmog_pat_evals counts the plays
// priced in rounds.
//
// A group's plays come best first by every other term, as in the play graph:
// a node is the arrangements that pin the group's first squares, and the best
// of them, wordsmog_best_arrangement with the pins, bounds the rest by every
// other term. The term is at most the group's defense_bound plus the largest
// utility correction of the play's score, so a node that cannot beat the best
// play is passed over whole. Every designation of an arrangement's blanks is a
// play of its own.
typedef struct WordsmogPATGroup {
  int row;
  int dir;
  int start_col;
  int word_length;
  int tiles_played;
  int num_squares;
  // The play's tiles, with PLAYED_THROUGH_MARKER on the board's tiles; an
  // arrangement's tiles go at square_letter_idxs.
  MachineLetter strip[BOARD_DIM];
  int square_letter_idxs[RACK_SIZE];
  WordsmogSquare squares[RACK_SIZE];
  // The group's tiles, in the refine's tiles, shared by the start columns of
  // a subrack.
  int tiles_idx;
  Equity base_score;
  Equity leave_value;
  // The PAT term without the utility correction is at most this for every
  // play of the group (see pat_eval_move_defense_bound).
  Equity defense_bound;
  // The highest score of any of its arrangements (see wordsmog_score_bound).
  Equity max_score;
  // The tiles of the play move generation recorded, already priced.
  MachineLetter recorded[RACK_SIZE];
  // How many of its plays have been priced.
  int evals;
  // Under MOVE_RECORD_ALL, its best play so far: its tiles, score and equity.
  MachineLetter best_tiles[RACK_SIZE];
  Equity best_score;
  Equity best_equity;
} WordsmogPATGroup;

// The arrangements of a group that put pins on its first num_pinned squares,
// or with designated set, the one play tiles spells.
typedef struct WordsmogPATNode {
  // An upper bound on the equity of its plays, and on their scores.
  Equity bound;
  Equity max_score;
  // The score of its best arrangement, and that arrangement's tiles.
  Equity score;
  MachineLetter tiles[RACK_SIZE];
  MachineLetter pins[RACK_SIZE];
  int num_pinned;
  bool designated;
  int group_idx;
  // The group's evals when the node was pushed, and when it was pushed.
  int round;
  uint32_t order;
} WordsmogPATNode;

struct WordsmogPATRefine {
  WordsmogPATGroup *groups;
  int num_groups;
  int groups_capacity;
  WordsmogTiles *tiles;
  int num_tiles;
  int tiles_capacity;
  WordsmogPATNode *heap;
  int heap_count;
  int heap_capacity;
  uint32_t next_order;
  // Whether move generation records every play (MOVE_RECORD_ALL), and then
  // the equity a group's best play needs to make the move list:
  // EQUITY_MIN_VALUE until the list could be filled.
  bool record_all;
  Equity threshold;
};

static void wordsmog_pat_refine_reset(WordsmogPATRefine *refine) {
  refine->num_groups = 0;
  refine->num_tiles = 0;
  refine->heap_count = 0;
  refine->next_order = 0;
  refine->threshold = EQUITY_MIN_VALUE;
}

static void wordsmog_pat_refine_destroy(WordsmogPATRefine *refine) {
  if (refine == NULL) {
    return;
  }
  free(refine->groups);
  free(refine->tiles);
  free(refine->heap);
  free(refine);
}

// The equity without the PAT term of a play of tiles_played tiles with score
// and adjustment: exactly static evaluation's, since the adjustment is the
// opening move's placement_adjustment.
static inline Equity wordsmog_equity_without_pat(const MoveGen *gen,
                                                 int tiles_played, Equity score,
                                                 Equity adjustment,
                                                 Equity leave_value) {
  return static_eval_get_nonopening_move_equity(
             &gen->ld, &gen->leave, &gen->opponent_rack, tiles_played,
             gen->number_of_tiles_in_bag, score, leave_value) +
         adjustment;
}

// Whether plays of group with equity at most bound and score at most
// max_score cannot beat the best play, or under MOVE_RECORD_ALL, the group's
// best play or the list's threshold: one that only ties its equity with a
// lower score loses to it (see compare_moves).
static inline bool wordsmog_pat_prunes(const MoveGen *gen,
                                       const WordsmogPATGroup *group,
                                       Equity bound, Equity max_score) {
  if (gen->wordsmog_pat->record_all) {
    return bound < gen->wordsmog_pat->threshold || bound < group->best_equity ||
           (bound == group->best_equity && max_score < group->best_score);
  }
  if (better_play_has_been_found(gen, bound)) {
    return true;
  }
  const Move *best = gen_get_readonly_best_move(gen);
  return move_get_equity(best) != EQUITY_INITIAL_VALUE &&
         bound == move_get_equity(best) && max_score < move_get_score(best);
}

// Fills node from the best arrangement of its group's arrangements with pins,
// and returns whether it could still beat the best play.
static bool wordsmog_pat_node_init(const MoveGen *gen,
                                   const WordsmogPATGroup *group, int group_idx,
                                   const MachineLetter pins[RACK_SIZE],
                                   int num_pinned,
                                   const WordsmogArrangement *arrangement,
                                   WordsmogPATNode *node) {
  const bool opening = gen->board_number_of_tiles_played == 0;
  node->score = group->base_score + arrangement->score;
  // On the opening move an arrangement's adjustment lets a lower key have a
  // higher score.
  node->max_score = opening ? group->max_score : node->score;
  node->bound =
      wordsmog_equity_without_pat(gen, group->tiles_played, node->score,
                                  arrangement->adjustment, group->leave_value) +
      group->defense_bound +
      pat_eval_utility_bound_for_play(&gen->pat_eval_ctx, group->tiles_played,
                                      node->max_score);
  memcpy(node->tiles, arrangement->tiles, sizeof(node->tiles));
  memcpy(node->pins, pins, sizeof(node->pins));
  node->num_pinned = num_pinned;
  node->designated = false;
  node->group_idx = group_idx;
  // Without a limit every play that could be best is priced anyway, and
  // pricing the most promising first prunes the most.
  node->round = gen->wordsmog_pat_evals == INT_MAX ? 0 : group->evals;
  return !wordsmog_pat_prunes(gen, group, node->bound, node->max_score);
}

// Fewer of its group's plays priced, then a higher bound; among those, the
// deeper node and then the one pushed first.
static bool wordsmog_pat_node_ranks_above(const WordsmogPATNode *a,
                                          const WordsmogPATNode *b) {
  if (a->round != b->round) {
    return a->round < b->round;
  }
  if (a->bound != b->bound) {
    return a->bound > b->bound;
  }
  const int a_depth = a->num_pinned + (a->designated ? 1 : 0);
  const int b_depth = b->num_pinned + (b->designated ? 1 : 0);
  if (a_depth != b_depth) {
    return a_depth > b_depth;
  }
  return a->order < b->order;
}

static void wordsmog_pat_push(WordsmogPATRefine *refine, WordsmogPATNode node) {
  if (refine->heap_count == refine->heap_capacity) {
    refine->heap_capacity =
        refine->heap_capacity == 0 ? 256 : refine->heap_capacity * 2;
    refine->heap = realloc_or_die(refine->heap, (size_t)refine->heap_capacity *
                                                    sizeof(WordsmogPATNode));
  }
  node.order = refine->next_order++;
  int child = refine->heap_count++;
  while (child > 0) {
    const int parent = (child - 1) / 2;
    if (!wordsmog_pat_node_ranks_above(&node, &refine->heap[parent])) {
      break;
    }
    refine->heap[child] = refine->heap[parent];
    child = parent;
  }
  refine->heap[child] = node;
}

static WordsmogPATNode wordsmog_pat_pop(WordsmogPATRefine *refine) {
  const WordsmogPATNode top = refine->heap[0];
  const WordsmogPATNode last = refine->heap[--refine->heap_count];
  int parent = 0;
  for (;;) {
    int child = 2 * parent + 1;
    if (child >= refine->heap_count) {
      break;
    }
    if (child + 1 < refine->heap_count &&
        wordsmog_pat_node_ranks_above(&refine->heap[child + 1],
                                      &refine->heap[child])) {
      child++;
    }
    if (!wordsmog_pat_node_ranks_above(&refine->heap[child], &last)) {
      break;
    }
    refine->heap[parent] = refine->heap[child];
    parent = child;
  }
  if (refine->heap_count > 0) {
    refine->heap[parent] = last;
  }
  return top;
}

// Keeps the group of the placement at start_col whose best arrangement move
// generation just recorded, with its root node, when any of its other plays
// could beat the best play; under MOVE_RECORD_ALL, which records it, the group
// whose best arrangement this is, priced here. tiles_idx is the subrack's
// tiles in the refine, or -1 until a group of the subrack is kept.
static void wordsmog_pat_add_group(MoveGen *gen, int start_col,
                                   const WordsmogSquare *squares,
                                   const int *square_letter_idxs,
                                   int num_squares, const WordsmogTiles *tiles,
                                   int *tiles_idx, Equity base_score,
                                   Equity score_bound,
                                   const WordsmogArrangement *arrangement) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  if (gen->wordsmog_pat == NULL) {
    gen->wordsmog_pat = calloc_or_die(1, sizeof(WordsmogPATRefine));
    wordsmog_pat_refine_reset(gen->wordsmog_pat);
  }
  WordsmogPATRefine *refine = gen->wordsmog_pat;
  refine->record_all = gen->move_record_type == MOVE_RECORD_ALL;
  WordsmogPATGroup group = {
      .row = gen->current_row_index,
      .dir = gen->dir,
      .start_col = start_col,
      .word_length = wgen->word_length,
      .tiles_played = gen->max_tiles_to_play,
      .num_squares = num_squares,
      .base_score = base_score,
      .leave_value = wgen->leave_value,
      .max_score = base_score + score_bound,
      .evals = 1,
  };
  memcpy(group.strip, gen->playthrough_marked, (size_t)wgen->word_length);
  memcpy(group.square_letter_idxs, square_letter_idxs,
         (size_t)num_squares * sizeof(int));
  memcpy(group.squares, squares, (size_t)num_squares * sizeof(WordsmogSquare));
  memcpy(group.recorded, arrangement->tiles, sizeof(group.recorded));
  // Any arrangement gives the group's squares.
  Move move;
  set_play_for_record_wmp(gen, &move, start_col,
                          base_score + arrangement->score);
  group.defense_bound =
      pat_eval_move_defense_bound(&gen->pat_eval_ctx, &move, &gen->leave);
  if (refine->record_all) {
    memcpy(group.best_tiles, arrangement->tiles, sizeof(group.best_tiles));
    group.best_score = move_get_score(&move);
    group.best_equity =
        static_eval_get_move_equity_with_leave_value(
            &gen->ld, &move, &gen->leave, &gen->opponent_rack,
            gen->opening_move_word_penalties,
            gen->opening_move_letter_penalties,
            pat_eval_context_get_active_classes(&gen->pat_eval_ctx), NULL,
            gen->board_number_of_tiles_played, gen->number_of_tiles_in_bag,
            wgen->leave_value) +
        pat_eval_move_penalty(&gen->pat_eval_ctx, &move, &gen->leave);
  }
  MachineLetter pins[RACK_SIZE];
  memset(pins, WORDSMOG_PIN_FREE, sizeof(pins));
  WordsmogPATNode root;
  const bool searchable = wordsmog_pat_node_init(
      gen, &group, refine->num_groups, pins, 0, arrangement, &root);
  if (!searchable && !refine->record_all) {
    return;
  }
  if (*tiles_idx < 0) {
    if (refine->num_tiles == refine->tiles_capacity) {
      refine->tiles_capacity =
          refine->tiles_capacity == 0 ? 16 : refine->tiles_capacity * 2;
      refine->tiles =
          realloc_or_die(refine->tiles, (size_t)refine->tiles_capacity *
                                            sizeof(WordsmogTiles));
    }
    refine->tiles[refine->num_tiles] = *tiles;
    *tiles_idx = refine->num_tiles++;
  }
  group.tiles_idx = *tiles_idx;
  if (refine->num_groups == refine->groups_capacity) {
    refine->groups_capacity =
        refine->groups_capacity == 0 ? 64 : refine->groups_capacity * 2;
    refine->groups =
        realloc_or_die(refine->groups, (size_t)refine->groups_capacity *
                                           sizeof(WordsmogPATGroup));
  }
  refine->groups[refine->num_groups++] = group;
  if (searchable) {
    wordsmog_pat_push(refine, root);
  }
}

// Pushes the children of node: with squares left to pin, the next one pinned
// to each tile left that has a legal arrangement there; with every square
// pinned, each designation of its blanks that keeps every word valid.
static void wordsmog_pat_expand(const MoveGen *gen, WordsmogPATRefine *refine,
                                const WordsmogPATNode *node) {
  const WordsmogPATGroup *group = &refine->groups[node->group_idx];
  WordsmogTiles *tiles = &refine->tiles[group->tiles_idx];
  WordsmogPATNode child;
  if (node->num_pinned == group->num_squares) {
    int blank_squares[2] = {0};
    int num_blanks = 0;
    for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
      if (get_is_blanked(node->tiles[square_idx])) {
        blank_squares[num_blanks++] = square_idx;
      }
    }
    child = *node;
    child.designated = true;
    const uint64_t designations =
        (((uint64_t)1 << BIT_RACK_MAX_ALPHABET_SIZE) - 1) & ~(uint64_t)1;
    const uint64_t first_set =
        group->squares[blank_squares[0]].cross_set & designations;
    if (num_blanks == 1) {
      for (uint64_t xs = first_set & tiles->blank_letters; xs != 0;
           xs &= xs - 1) {
        child.tiles[blank_squares[0]] =
            get_blanked_machine_letter((MachineLetter)lowest_set_bit_index(xs));
        wordsmog_pat_push(refine, child);
      }
      return;
    }
    const uint64_t second_set =
        group->squares[blank_squares[1]].cross_set & designations;
    for (uint64_t xs = first_set; xs != 0; xs &= xs - 1) {
      const MachineLetter x = (MachineLetter)lowest_set_bit_index(xs);
      for (uint64_t ys = second_set; ys != 0; ys &= ys - 1) {
        const MachineLetter y = (MachineLetter)lowest_set_bit_index(ys);
        const MachineLetter low = x < y ? x : y;
        const MachineLetter high = x < y ? y : x;
        if (((tiles->pair_letters[low] >> high) & 1U) == 0) {
          continue;
        }
        child.tiles[blank_squares[0]] = get_blanked_machine_letter(x);
        child.tiles[blank_squares[1]] = get_blanked_machine_letter(y);
        wordsmog_pat_push(refine, child);
      }
    }
    return;
  }
  int counts[RACK_SIZE];
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    counts[letter_idx] = tiles->counts[letter_idx];
  }
  int blanks = tiles->num_blanks;
  for (int square_idx = 0; square_idx < node->num_pinned; square_idx++) {
    if (node->pins[square_idx] == WORDSMOG_PIN_BLANK) {
      blanks--;
      continue;
    }
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      if (tiles->letters[letter_idx] == node->pins[square_idx]) {
        counts[letter_idx]--;
      }
    }
  }
  MachineLetter pins[RACK_SIZE];
  memcpy(pins, node->pins, sizeof(pins));
  const uint64_t cross_set = group->squares[node->num_pinned].cross_set;
  for (int letter_idx = 0; letter_idx <= tiles->num_letters; letter_idx++) {
    if (letter_idx < tiles->num_letters) {
      pins[node->num_pinned] = tiles->letters[letter_idx];
      if (counts[letter_idx] == 0 ||
          ((cross_set >> tiles->letters[letter_idx]) & 1U) == 0) {
        continue;
      }
    } else {
      if (blanks == 0) {
        continue;
      }
      pins[node->num_pinned] = WORDSMOG_PIN_BLANK;
    }
    WordsmogArrangement arrangement;
    if (wordsmog_best_arrangement(group->squares, group->num_squares, tiles,
                                  pins, &arrangement) &&
        wordsmog_pat_node_init(gen, group, node->group_idx, pins,
                               node->num_pinned + 1, &arrangement, &child)) {
      wordsmog_pat_push(refine, child);
    }
  }
}

// Prices the play of group with tiles, whose score is score, and makes it the
// best play if it beats it. Returns whether it was priced.
// Writes the play of group with tiles, whose score is score, to move, without
// its equity.
static void wordsmog_pat_write_play(const WordsmogPATGroup *group,
                                    const MachineLetter *tiles, Equity score,
                                    Move *move) {
  MachineLetter strip[BOARD_DIM];
  memcpy(strip, group->strip, (size_t)group->word_length);
  for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
    strip[group->square_letter_idxs[square_idx]] = tiles[square_idx];
  }
  move_set_all_except_equity(move, strip, 0, group->word_length - 1, score,
                             group->row, group->start_col, group->tiles_played,
                             group->dir, GAME_EVENT_TILE_PLACEMENT_MOVE);
  if (board_is_dir_vertical(group->dir)) {
    move_set_row_start(move, group->start_col);
    move_set_col_start(move, group->row);
  }
}

// Under MOVE_RECORD_ALL, the equity the best plays of the groups need to make
// the move list: the capacity-th highest, or EQUITY_MIN_VALUE when fewer
// groups than that have plays (or the list is too long to track).
static Equity wordsmog_pat_list_threshold(const MoveGen *gen,
                                          const WordsmogPATRefine *refine) {
  Equity top[WORDSMOG_PAT_MAX_TRACKED_LIST] = {0};
  const int capacity = move_list_get_capacity(gen->move_list);
  if (capacity > WORDSMOG_PAT_MAX_TRACKED_LIST ||
      refine->num_groups < capacity) {
    return EQUITY_MIN_VALUE;
  }
  int count = 0;
  for (int group_idx = 0; group_idx < refine->num_groups; group_idx++) {
    const Equity equity = refine->groups[group_idx].best_equity;
    if (count == capacity && equity <= top[count - 1]) {
      continue;
    }
    int insert_idx = count < capacity ? count++ : capacity - 1;
    while (insert_idx > 0 && top[insert_idx - 1] < equity) {
      top[insert_idx] = top[insert_idx - 1];
      insert_idx--;
    }
    top[insert_idx] = equity;
  }
  return top[capacity - 1];
}

// Prices the play of group with tiles, whose score is score, and makes it the
// best play, or under MOVE_RECORD_ALL the group's, if it beats it. Returns
// whether it was priced.
static bool wordsmog_pat_price(MoveGen *gen, WordsmogPATGroup *group,
                               const MachineLetter *tiles, Equity score) {
  WordsmogPATRefine *refine = gen->wordsmog_pat;
  Move scratch;
  Move *move = refine->record_all ? &scratch : gen_get_current_move(gen);
  wordsmog_pat_write_play(group, tiles, score, move);
  const Equity equity_without_pat =
      static_eval_get_move_equity_with_leave_value(
          &gen->ld, move, &gen->leave, &gen->opponent_rack,
          gen->opening_move_word_penalties, gen->opening_move_letter_penalties,
          pat_eval_context_get_active_classes(&gen->pat_eval_ctx), NULL,
          gen->board_number_of_tiles_played, gen->number_of_tiles_in_bag,
          group->leave_value);
  if (wordsmog_pat_prunes(
          gen, group,
          equity_without_pat + group->defense_bound +
              pat_eval_utility_bound_for_play(&gen->pat_eval_ctx,
                                              group->tiles_played, score),
          score)) {
    return false;
  }
  if (refine->record_all) {
    // As gen_pat_contender_equity, against what the play has to reach: a play
    // that cannot reach it gets an overestimate that still falls short.
    const Equity needed_equity = group->best_equity > refine->threshold
                                     ? group->best_equity
                                     : refine->threshold;
    int64_t needed = (int64_t)needed_equity - equity_without_pat;
    if (needed < EQUITY_MIN_VALUE) {
      needed = EQUITY_MIN_VALUE;
    } else if (needed > EQUITY_MAX_VALUE) {
      needed = EQUITY_MAX_VALUE;
    }
    const Equity equity = equity_without_pat + pat_eval_move_penalty_capped(
                                                   &gen->pat_eval_ctx, move,
                                                   &gen->leave, (Equity)needed);
    if (equity > group->best_equity ||
        (equity == group->best_equity && score > group->best_score)) {
      const bool raises_threshold = equity > refine->threshold;
      memcpy(group->best_tiles, tiles,
             (size_t)group->num_squares * sizeof(MachineLetter));
      group->best_score = score;
      group->best_equity = equity;
      if (raises_threshold) {
        refine->threshold = wordsmog_pat_list_threshold(gen, refine);
      }
    }
    return true;
  }
  move_set_equity(move, gen_pat_contender_equity(gen, move, &gen->leave,
                                                 equity_without_pat));
  if (compare_moves(move, gen_get_readonly_best_move(gen), true) == 1) {
    gen_switch_best_move_and_current_move(gen);
    gen_update_cutoff_equity_or_score(gen);
  }
  return true;
}

// Prices the groups' plays a round at a time (see WordsmogPATGroup) until
// gen->wordsmog_pat_evals plays have been priced, counting under
// MOVE_RECORD_BEST the one of each group move generation recorded, or no play
// left could beat the best. Under MOVE_RECORD_ALL, then records each group's
// best play.
static void wordsmog_pat_refine(MoveGen *gen) {
  WordsmogPATRefine *refine = gen->wordsmog_pat;
  int evals_left = gen->wordsmog_pat_evals;
  if (evals_left != INT_MAX && !refine->record_all) {
    evals_left -= refine->num_groups;
  }
  if (refine->record_all) {
    refine->threshold = wordsmog_pat_list_threshold(gen, refine);
  }
  while (evals_left > 0 && refine->heap_count > 0) {
    WordsmogPATNode node = wordsmog_pat_pop(refine);
    WordsmogPATGroup *group = &refine->groups[node.group_idx];
    if (evals_left != INT_MAX && node.round != group->evals) {
      // Its group has had a play priced since: back in line for the next
      // round.
      node.round = group->evals;
      wordsmog_pat_push(refine, node);
      continue;
    }
    if (wordsmog_pat_prunes(gen, group, node.bound, node.max_score)) {
      continue;
    }
    bool undesignated_blank = false;
    for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
      undesignated_blank |= get_is_blanked(node.tiles[square_idx]);
    }
    undesignated_blank &= !node.designated;
    if (node.num_pinned < group->num_squares || undesignated_blank) {
      wordsmog_pat_expand(gen, refine, &node);
      continue;
    }
    if (memcmp(node.tiles, group->recorded,
               (size_t)group->num_squares * sizeof(MachineLetter)) == 0) {
      continue;
    }
    if (wordsmog_pat_price(gen, group, node.tiles, node.score) &&
        evals_left != INT_MAX) {
      group->evals++;
      evals_left--;
    }
  }
  if (refine->record_all) {
    for (int group_idx = 0; group_idx < refine->num_groups; group_idx++) {
      const WordsmogPATGroup *group = &refine->groups[group_idx];
      Move *move = move_list_get_spare_move(gen->move_list);
      wordsmog_pat_write_play(group, group->best_tiles, group->best_score,
                              move);
      move_list_insert_spare_move(gen->move_list, group->best_equity);
    }
  }
  wordsmog_pat_refine_reset(refine);
}

// Records the best arrangement of tiles at each of the anchor's start
// columns, and with the PAT term on under MOVE_RECORD_BEST keeps its group
// for wordsmog_pat_refine; under MOVE_RECORD_ALL it keeps the group instead,
// for wordsmog_pat_refine to record. Returns true once the recording
// threshold has been exceeded.
static bool wordsmog_record_placements(MoveGen *gen, const Anchor *anchor,
                                       WordsmogTiles *tiles,
                                       Equity anchor_pat_bound) {
  const WMPMoveGen *wgen = &gen->wmp_move_gen;
  const bool pat_refine = gen->move_sort_type == MOVE_SORT_EQUITY &&
                          gen_pat_is_active(gen) &&
                          (gen->move_record_type == MOVE_RECORD_BEST ||
                           gen->move_record_type == MOVE_RECORD_ALL) &&
                          !gen->stop_on_threshold;
  int tiles_idx = -1;
  for (int start_col = anchor->leftmost_start_col;
       start_col <= anchor->rightmost_start_col; start_col++) {
    WordsmogSquare squares[RACK_SIZE];
    int square_letter_idxs[RACK_SIZE];
    Equity base_score;
    const int num_squares = wordsmog_placement_squares(
        gen, start_col, squares, square_letter_idxs, &base_score);
    assert(num_squares == anchor->tiles_to_play);
    const Equity score_bound =
        wordsmog_score_bound(squares, num_squares, tiles);
    if (wordsmog_placement_pruned(gen, base_score + score_bound,
                                  anchor_pat_bound) ||
        wordsmog_placement_pruned(
            gen,
            base_score + wordsmog_cross_set_bound(squares, num_squares, tiles,
                                                  score_bound),
            anchor_pat_bound)) {
      continue;
    }
    WordsmogArrangement arrangement;
    if (!wordsmog_best_arrangement(squares, num_squares, tiles, NULL,
                                   &arrangement)) {
      continue;
    }
    for (int square_idx = 0; square_idx < num_squares; square_idx++) {
      gen->playthrough_marked[square_letter_idxs[square_idx]] =
          arrangement.tiles[square_idx];
    }
    // A play's score is the placement's part plus its tiles', which the
    // prune above and the play graph rely on.
    assert(wmp_play_score(gen, start_col) == base_score + arrangement.score);
    if (!pat_refine || gen->move_record_type != MOVE_RECORD_ALL) {
      record_wmp_play(gen, start_col, wgen->leave_value);
    }
    if (gen->threshold_exceeded) {
      return true;
    }
    if (pat_refine) {
      wordsmog_pat_add_group(gen, start_col, squares, square_letter_idxs,
                             num_squares, tiles, &tiles_idx, base_score,
                             score_bound, &arrangement);
    }
  }
  return false;
}

// As wordmap_gen_record_subrack, but plays the subrack's best arrangements
// instead of its words. Returns true once the recording threshold has been
// exceeded.
static bool wordsmog_record_subrack(MoveGen *gen, const Anchor *anchor,
                                    int subrack_idx, bool lazy,
                                    Equity anchor_pat_bound) {
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  if (gen->wmp_prune_subracks_by_leave) {
    wgen->leave_value = wmp_move_gen_get_leave_value(wgen, subrack_idx);
  }
  if (wordmap_gen_leave_prunes_subrack(gen, anchor, subrack_idx,
                                       anchor_pat_bound)) {
    return false;
  }
  BitRack main_word;
  const WMPEntry *entry =
      wmp_move_gen_get_subrack_entry(wgen, subrack_idx, lazy, &main_word);
  if (entry == NULL) {
    return false;
  }
  if (gen->number_of_tiles_in_bag == 0) {
    wordmap_gen_set_empty_bag_leave(gen, anchor, subrack_idx);
  }
  WordsmogTiles tiles;
  wordsmog_tiles_init(
      gen, wmp_move_gen_get_nonplaythrough_subrack(wgen, subrack_idx),
      &main_word, entry, &tiles);
  return wordsmog_record_placements(gen, anchor, &tiles, anchor_pat_bound);
}

// As wordmap_gen for wordsmog: the anchor's candidate subracks, each played in
// its best arrangement. The word info table is off in wordsmog, and
// the RIT's bingo words are spellings, so their prunes and fast path are
// skipped.
static void wordsmog_wmp_gen(MoveGen *gen, const Anchor *anchor, bool lazy) {
  gen->max_tiles_to_play = anchor->tiles_to_play;
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  // Upper bound on the PAT term of every move from this anchor (zero when
  // the term is off). See wordmap_gen_leave_prunes_subrack.
  const Equity anchor_pat_bound =
      gen_wmp_anchor_pat_bound(gen, anchor->dir, anchor->row, anchor);
  wmp_move_gen_set_playthrough_bit_rack(wgen, anchor, gen->row_squares, NULL,
                                        gen->wit_len_lane, NULL);
  wmp_move_gen_playthrough_metadata_init(wgen, anchor);
  wmp_move_gen_build_playthrough_subracks(wgen);
  const int num_subrack_combinations =
      wmp_move_gen_get_num_subrack_combinations(wgen);
  for (int first_idx = 0; first_idx < num_subrack_combinations;
       first_idx += WMP_MOVE_GEN_SUBRACKS_PER_MASK) {
    const int remaining = num_subrack_combinations - first_idx;
    const int count = remaining < WMP_MOVE_GEN_SUBRACKS_PER_MASK
                          ? remaining
                          : WMP_MOVE_GEN_SUBRACKS_PER_MASK;
    uint64_t filter_misses = 0;
    uint64_t candidates = wordmap_gen_candidate_subracks(
        gen, anchor, first_idx, count, lazy, anchor_pat_bound, &filter_misses);
    while (candidates != 0) {
      const int bit_idx = wmp_move_gen_get_lowest_set_bit(candidates);
      candidates &= candidates - 1;
      const uint64_t misses_below = filter_misses & ((1ULL << bit_idx) - 1);
      if (misses_below != 0) {
        wordmap_gen_resolve_filter_misses(gen, anchor, first_idx, misses_below,
                                          anchor_pat_bound);
        filter_misses ^= misses_below;
      }
      if (wordsmog_record_subrack(gen, anchor, first_idx + bit_idx, lazy,
                                  anchor_pat_bound)) {
        return;
      }
    }
    if (filter_misses != 0) {
      wordmap_gen_resolve_filter_misses(gen, anchor, first_idx, filter_misses,
                                        anchor_pat_bound);
    }
  }
}

void go_on(MoveGen *gen, int current_col, MachineLetter L,
           uint32_t new_node_index, bool accepts, int leftstrip, int rightstrip,
           bool unique_play, int main_word_score, int word_multiplier,
           Equity cross_score);

void recursive_gen(MoveGen *gen, int col, uint32_t node_index, int leftstrip,
                   int rightstrip, bool unique_play, int main_word_score,
                   int word_multiplier, Equity cross_score) {
  if (gen->threshold_exceeded) {
    return;
  }
  const MachineLetter current_letter = gen_cache_get_letter(gen, col);
  // If current_letter is nonempty, leftx is the set of letters that could go
  // immediately left of the current block of played tiles.
  //
  // If current_letter is empty, leftx is the set of letters that could
  // go immediately left of the block of tiles to the right of this square.
  //
  // In either case, it is a mask applied to the tile that could be placed by
  // this function.
  uint64_t possible_letters_here = gen_cache_get_cross_set(gen, col) &
                                   gen_cache_get_left_extension_set(gen, col);
  if ((gen->tiles_played == 0) && (col == gen->current_anchor_col + 1)) {
    possible_letters_here &= gen->anchor_right_extension_set;
  }
  if (possible_letters_here == 1) {
    possible_letters_here = 0;
  }
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    const MachineLetter raw = get_unblanked_machine_letter(current_letter);
    uint32_t next_node_index = 0;
    bool accepts = false;
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(gen->kwg, i);
      if (kwg_node_tile(node) == raw) {
        next_node_index = kwg_node_arc_index_prefetch(node, gen->kwg);
        accepts = kwg_node_accepts(node);
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    go_on(gen, col, current_letter, next_node_index, accepts, leftstrip,
          rightstrip, unique_play, main_word_score, word_multiplier,
          cross_score);
  } else if (!rack_is_empty(&gen->player_rack) &&
             ((possible_letters_here & gen->rack_cross_set) != 0)) {
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(gen->kwg, i);
      const MachineLetter ml = kwg_node_tile(node);
      const uint16_t number_of_ml = rack_get_letter(&gen->player_rack, ml);
      if (ml != 0 &&
          (number_of_ml != 0 ||
           rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) != 0) &&
          board_is_letter_allowed_in_cross_set(possible_letters_here, ml)) {
        const uint32_t next_node_index =
            kwg_node_arc_index_prefetch(node, gen->kwg);
        bool accepts = kwg_node_accepts(node);
        if (number_of_ml > 0) {
          leave_map_take_letter_and_update_current_index(&gen->leave_map,
                                                         &gen->player_rack, ml);
          gen->tiles_played++;
          go_on(gen, col, ml, next_node_index, accepts, leftstrip, rightstrip,
                unique_play, main_word_score, word_multiplier, cross_score);
          gen->tiles_played--;
          leave_map_add_letter_and_update_current_index(&gen->leave_map,
                                                        &gen->player_rack, ml);
        }
        // check blank
        if (rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) > 0) {
          leave_map_take_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, BLANK_MACHINE_LETTER);
          gen->tiles_played++;
          go_on(gen, col, get_blanked_machine_letter(ml), next_node_index,
                accepts, leftstrip, rightstrip, unique_play, main_word_score,
                word_multiplier, cross_score);
          gen->tiles_played--;
          leave_map_add_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
      if (gen->threshold_exceeded) {
        break;
      }
    }
  }
}

static inline bool play_is_nonempty_and_nonduplicate(int tiles_played,
                                                     bool is_unique) {
  return (tiles_played > 1) || ((tiles_played == 1) && is_unique);
}

void go_on(MoveGen *gen, int current_col, MachineLetter L,
           uint32_t new_node_index, bool accepts, int leftstrip, int rightstrip,
           bool unique_play, Equity main_word_score, int word_multiplier,
           Equity cross_score) {
  if (gen->threshold_exceeded) {
    return;
  }
  // Handle incremental scoring
  const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, current_col);
  uint8_t letter_multiplier = 1;
  uint8_t this_word_multiplier = 1;
  bool fresh_tile = false;

  const bool square_is_empty = gen_cache_is_empty(gen, current_col);
  MachineLetter ml;
  if (!square_is_empty) {
    gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    ml = gen_cache_get_letter(gen, current_col);
  } else {
    gen->strip[current_col] = L;
    ml = L;
    fresh_tile = true;
    this_word_multiplier = bonus_square_get_word_multiplier(bonus_square);
    letter_multiplier = bonus_square_get_letter_multiplier(bonus_square);
  }

  const int inc_word_multiplier = this_word_multiplier * word_multiplier;

  const Equity lsm = gen->tile_scores[ml] * letter_multiplier;

  const Equity inc_main_word_score = lsm + main_word_score;

  Equity inc_cross_scores = cross_score;

  if (fresh_tile && gen_cache_get_is_cross_word(gen, current_col)) {
    inc_cross_scores += (lsm + gen_cache_get_cross_score(gen, current_col)) *
                        this_word_multiplier;
  }

  if (current_col <= gen->current_anchor_col) {
    if (square_is_empty && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    leftstrip = current_col;
    bool no_letter_directly_left =
        (current_col == 0) || gen_cache_is_empty(gen, current_col - 1);

    if (accepts && no_letter_directly_left &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (new_node_index == 0) {
      return;
    }

    if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
      recursive_gen(gen, current_col - 1, new_node_index, leftstrip, rightstrip,
                    unique_play, inc_main_word_score, inc_word_multiplier,
                    inc_cross_scores);
    }

    // rightx only tells you which tiles can go to the right of a string which
    // begins a word. So it can only filter here if no tiles have been played
    // so far. If gen->tiles_played is 0, this recursive_gen call would be
    // placing the first tile of a play and continuing to the right.
    if ((gen->tiles_played != 0) ||
        (gen->anchor_right_extension_set & gen->rack_cross_set) != 0) {
      uint32_t separation_node_index = kwg_get_next_node_index(
          gen->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
      if (separation_node_index != 0 && no_letter_directly_left &&
          gen->current_anchor_col < BOARD_DIM - 1) {
        recursive_gen(gen, gen->current_anchor_col + 1, separation_node_index,
                      leftstrip, rightstrip, unique_play, inc_main_word_score,
                      inc_word_multiplier, inc_cross_scores);
      }
    }
  } else {
    if (square_is_empty && !unique_play && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    rightstrip = current_col;
    bool no_letter_directly_right = (current_col == BOARD_DIM - 1) ||
                                    gen_cache_is_empty(gen, current_col + 1);

    if (accepts && no_letter_directly_right &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      recursive_gen(gen, current_col + 1, new_node_index, leftstrip, rightstrip,
                    unique_play, inc_main_word_score, inc_word_multiplier,
                    inc_cross_scores);
    }
  }
}

// Forward declaration for small move generation (no leave tracking)
static inline void go_on_small(MoveGen *gen, int current_col, MachineLetter L,
                               uint32_t new_node_index, bool accepts,
                               int leftstrip, int rightstrip, bool unique_play,
                               Equity main_word_score, int word_multiplier,
                               Equity cross_score);

// Specialized recursive_gen for MOVE_RECORD_ALL_SMALL that skips leave_map
// operations. Only tracks rack state directly.
static inline void recursive_gen_small(MoveGen *gen, int col,
                                       uint32_t node_index, int leftstrip,
                                       int rightstrip, bool unique_play,
                                       int main_word_score, int word_multiplier,
                                       Equity cross_score) {
  // Load the row Square once; every field read below goes through it,
  // giving the compiler a single base for the col*sizeof(Square) arithmetic.
  const Square *sq = &gen->row_squares[col];
  const MachineLetter current_letter = square_get_letter(sq);
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    // Play-through square: possible_letters_here is dead on this branch (it
    // reads only raw / next_node_index / accepts), so it is not computed here.
    const MachineLetter raw = get_unblanked_machine_letter(current_letter);
    uint32_t next_node_index = 0;
    bool accepts = false;
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(gen->kwg, i);
      if (kwg_node_tile(node) == raw) {
        next_node_index = kwg_node_arc_index_prefetch(node, gen->kwg);
        accepts = kwg_node_accepts(node);
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    go_on_small(gen, col, current_letter, next_node_index, accepts, leftstrip,
                rightstrip, unique_play, main_word_score, word_multiplier,
                cross_score);
  } else if (!rack_is_empty(&gen->player_rack)) {
    // Empty square: compute the placeable-letter set only now -- it is unused
    // on the play-through branch and when the mover's rack is empty.
    uint64_t possible_letters_here =
        square_get_cross_set(sq) & square_get_left_extension_set(sq);
    if ((gen->tiles_played == 0) && (col == gen->current_anchor_col + 1)) {
      possible_letters_here &= gen->anchor_right_extension_set;
    }
    if (possible_letters_here == 1) {
      possible_letters_here = 0;
    }
    if ((possible_letters_here & gen->rack_cross_set) == 0) {
      return;
    }
    // The rack's blank count is invariant across the sibling loop: each
    // iteration's take/add of a blank (or a non-blank ml) is balanced and the
    // go_on_small recursion fully restores player_rack, so hoist it.
    const uint16_t num_blanks =
        rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER);
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(gen->kwg, i);
      const MachineLetter ml = kwg_node_tile(node);
      const uint16_t number_of_ml = rack_get_letter(&gen->player_rack, ml);
      if (ml != 0 && (number_of_ml != 0 || num_blanks != 0) &&
          board_is_letter_allowed_in_cross_set(possible_letters_here, ml)) {
        const uint32_t next_node_index =
            kwg_node_arc_index_prefetch(node, gen->kwg);
        bool accepts = kwg_node_accepts(node);
        if (number_of_ml > 0) {
          rack_take_letter(&gen->player_rack, ml);
          gen->tiles_played++;
          go_on_small(gen, col, ml, next_node_index, accepts, leftstrip,
                      rightstrip, unique_play, main_word_score, word_multiplier,
                      cross_score);
          gen->tiles_played--;
          rack_add_letter(&gen->player_rack, ml);
        }
        // check blank
        if (num_blanks > 0) {
          rack_take_letter(&gen->player_rack, BLANK_MACHINE_LETTER);
          gen->tiles_played++;
          go_on_small(gen, col, get_blanked_machine_letter(ml), next_node_index,
                      accepts, leftstrip, rightstrip, unique_play,
                      main_word_score, word_multiplier, cross_score);
          gen->tiles_played--;
          rack_add_letter(&gen->player_rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
}

// Specialized go_on for MOVE_RECORD_ALL_SMALL
static inline void go_on_small(MoveGen *gen, int current_col, MachineLetter L,
                               uint32_t new_node_index, bool accepts,
                               int leftstrip, int rightstrip, bool unique_play,
                               Equity main_word_score, int word_multiplier,
                               Equity cross_score) {
  const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, current_col);
  uint8_t letter_multiplier = 1;
  uint8_t this_word_multiplier = 1;
  bool fresh_tile = false;

  const bool square_is_empty = gen_cache_is_empty(gen, current_col);
  MachineLetter ml;
  if (!square_is_empty) {
    gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    ml = gen_cache_get_letter(gen, current_col);
  } else {
    gen->strip[current_col] = L;
    ml = L;
    fresh_tile = true;
    this_word_multiplier = bonus_square_get_word_multiplier(bonus_square);
    letter_multiplier = bonus_square_get_letter_multiplier(bonus_square);
  }

  const int inc_word_multiplier = this_word_multiplier * word_multiplier;
  const Equity lsm = gen->tile_scores[ml] * letter_multiplier;
  const Equity inc_main_word_score = lsm + main_word_score;
  Equity inc_cross_scores = cross_score;

  if (fresh_tile && gen_cache_get_is_cross_word(gen, current_col)) {
    inc_cross_scores += (lsm + gen_cache_get_cross_score(gen, current_col)) *
                        this_word_multiplier;
  }

  if (current_col <= gen->current_anchor_col) {
    if (square_is_empty && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    leftstrip = current_col;
    bool no_letter_directly_left =
        (current_col == 0) || gen_cache_is_empty(gen, current_col - 1);

    if (accepts && no_letter_directly_left &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (new_node_index == 0) {
      return;
    }

    if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
      recursive_gen_small(gen, current_col - 1, new_node_index, leftstrip,
                          rightstrip, unique_play, inc_main_word_score,
                          inc_word_multiplier, inc_cross_scores);
    }

    if ((gen->tiles_played != 0) ||
        (gen->anchor_right_extension_set & gen->rack_cross_set) != 0) {
      uint32_t separation_node_index = kwg_get_next_node_index(
          gen->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
      if (separation_node_index != 0 && no_letter_directly_left &&
          gen->current_anchor_col < BOARD_DIM - 1) {
        recursive_gen_small(gen, gen->current_anchor_col + 1,
                            separation_node_index, leftstrip, rightstrip,
                            unique_play, inc_main_word_score,
                            inc_word_multiplier, inc_cross_scores);
      }
    }
  } else {
    if (square_is_empty && !unique_play && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    rightstrip = current_col;
    bool no_letter_directly_right = (current_col == BOARD_DIM - 1) ||
                                    gen_cache_is_empty(gen, current_col + 1);

    if (accepts && no_letter_directly_right &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      recursive_gen_small(gen, current_col + 1, new_node_index, leftstrip,
                          rightstrip, unique_play, inc_main_word_score,
                          inc_word_multiplier, inc_cross_scores);
    }
  }
}

void go_on_alpha(MoveGen *gen, int current_col, MachineLetter L, int leftstrip,
                 int rightstrip, bool unique_play, int main_word_score,
                 int word_multiplier, Equity cross_score);

void recursive_gen_alpha(MoveGen *gen, int col, int leftstrip, int rightstrip,
                         bool unique_play, Equity main_word_score,
                         int word_multiplier, Equity cross_score) {
  const MachineLetter current_letter = gen_cache_get_letter(gen, col);
  uint64_t possible_letters_here = gen_cache_get_cross_set(gen, col);
  if (possible_letters_here == 1) {
    possible_letters_here = 0;
  }
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    const MachineLetter raw = get_unblanked_machine_letter(current_letter);
    rack_add_letter(&gen->full_player_rack, raw);
    go_on_alpha(gen, col, current_letter, leftstrip, rightstrip, unique_play,
                main_word_score, word_multiplier, cross_score);
    rack_take_letter(&gen->full_player_rack, raw);
  } else if (!rack_is_empty(&gen->player_rack) &&
             ((possible_letters_here & gen->rack_cross_set) != 0)) {
    const MachineLetter ld_size = ld_get_size(&gen->ld);
    for (MachineLetter ml = 1; ml < ld_size; ml++) {
      const uint16_t number_of_ml = rack_get_letter(&gen->player_rack, ml);
      if ((number_of_ml != 0 ||
           rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) != 0) &&
          board_is_letter_allowed_in_cross_set(possible_letters_here, ml)) {
        if (number_of_ml > 0) {
          alpha_leave_map_take_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, &gen->full_player_rack, ml,
              ml);
          gen->tiles_played++;
          go_on_alpha(gen, col, ml, leftstrip, rightstrip, unique_play,
                      main_word_score, word_multiplier, cross_score);
          gen->tiles_played--;
          alpha_leave_map_add_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, &gen->full_player_rack, ml,
              ml);
        }
        // check blank
        if (rack_get_letter(&gen->player_rack, BLANK_MACHINE_LETTER) > 0) {
          alpha_leave_map_take_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, &gen->full_player_rack,
              BLANK_MACHINE_LETTER, ml);
          gen->tiles_played++;
          go_on_alpha(gen, col, get_blanked_machine_letter(ml), leftstrip,
                      rightstrip, unique_play, main_word_score, word_multiplier,
                      cross_score);
          gen->tiles_played--;
          alpha_leave_map_add_letter_and_update_current_index(
              &gen->leave_map, &gen->player_rack, &gen->full_player_rack,
              BLANK_MACHINE_LETTER, ml);
        }
      }
    }
  }
}

void go_on_alpha(MoveGen *gen, int current_col, MachineLetter L, int leftstrip,
                 int rightstrip, bool unique_play, Equity main_word_score,
                 int word_multiplier, Equity cross_score) {
  // Handle incremental scoring
  const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, current_col);
  int letter_multiplier = 1;
  int this_word_multiplier = 1;
  bool fresh_tile = false;

  const bool square_is_empty = gen_cache_is_empty(gen, current_col);
  MachineLetter ml;
  if (!square_is_empty) {
    gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    ml = gen_cache_get_letter(gen, current_col);
  } else {
    gen->strip[current_col] = L;
    ml = L;
    fresh_tile = true;
    this_word_multiplier = bonus_square_get_word_multiplier(bonus_square);
    letter_multiplier = bonus_square_get_letter_multiplier(bonus_square);
  }

  const int inc_word_multiplier = this_word_multiplier * word_multiplier;

  const Equity lsm = gen->tile_scores[ml] * letter_multiplier;

  const Equity inc_main_word_score = lsm + main_word_score;

  Equity inc_cross_scores = cross_score;

  if (fresh_tile && gen_cache_get_is_cross_word(gen, current_col)) {
    inc_cross_scores += (lsm + gen_cache_get_cross_score(gen, current_col)) *
                        this_word_multiplier;
  }

  const bool accepts = kwg_accepts_alpha(gen->kwg, &gen->full_player_rack);
  if (current_col <= gen->current_anchor_col) {
    if (square_is_empty && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    leftstrip = current_col;
    bool no_letter_directly_left =
        (current_col == 0) || gen_cache_is_empty(gen, current_col - 1);

    if (accepts && no_letter_directly_left &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
      recursive_gen_alpha(gen, current_col - 1, leftstrip, rightstrip,
                          unique_play, inc_main_word_score, inc_word_multiplier,
                          inc_cross_scores);
    }

    // rightx only tells you which tiles can go to the right of a string which
    // begins a word. So it can only filter here if no tiles have been played
    // so far. If gen->tiles_played is 0, this recursive_gen call would be
    // placing the first tile of a play and continuing to the right.
    if ((gen->tiles_played != 0) ||
        (gen->anchor_right_extension_set & gen->rack_cross_set) != 0) {
      if (no_letter_directly_left && gen->current_anchor_col < BOARD_DIM - 1) {
        recursive_gen_alpha(gen, gen->current_anchor_col + 1, leftstrip,
                            rightstrip, unique_play, inc_main_word_score,
                            inc_word_multiplier, inc_cross_scores);
      }
    }
  } else {
    if (square_is_empty && !unique_play && gen->dir &&
        gen_cache_get_cross_set(gen, current_col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    rightstrip = current_col;
    bool no_letter_directly_right = (current_col == BOARD_DIM - 1) ||
                                    gen_cache_is_empty(gen, current_col + 1);

    if (accepts && no_letter_directly_right &&
        play_is_nonempty_and_nonduplicate(gen->tiles_played, unique_play)) {
      record_tile_placement_move(gen, leftstrip, rightstrip,
                                 inc_main_word_score, inc_word_multiplier,
                                 inc_cross_scores);
    }

    if (current_col < BOARD_DIM - 1) {
      recursive_gen_alpha(gen, current_col + 1, leftstrip, rightstrip,
                          unique_play, inc_main_word_score, inc_word_multiplier,
                          inc_cross_scores);
    }
  }
}

static inline __attribute__((always_inline)) void
shadow_record_impl(MoveGen *gen, bool wmp_active, uint32_t allowed_lengths) {
  const Equity *best_leaves = gen->best_leaves;
  if (wmp_active) {
    const int final_length =
        gen->tiles_played + gen->wmp_move_gen.num_tiles_played_through;
    if ((allowed_lengths & (UINT32_C(1) << final_length)) == 0) {
      return;
    }
    if (wmp_move_gen_has_playthrough(&gen->wmp_move_gen)) {
      // RIT fast path for single-playthrough anchors: the RIT's
      // playthrough_union[leave_size] holds a uint32 bitmask of letters L
      // such that at least one canonical (tiles_played)-tile subrack of
      // the current full rack forms a valid (tiles_played + 1)-letter
      // word with L. Works for any tiles_played value (1..RACK_SIZE) as
      // long as the built RIT has coverage for that played size. Replaces
      // the KWG/WMP hash walk with one array load and one bit test.
      //
      // Preconditions:
      //   - gen->rit_entry is set (implies number_of_letters_on_rack ==
      //     RACK_SIZE, since gen_look_up_leaves_and_record_exchanges only
      //     populates rit_entry for full racks)
      //   - exactly one playthrough tile (the RIT's unions are built for
      //     single-blank-equivalent queries, not multi-blank)
      //   - played_size falls in the RIT's coverage interval
      const bool rit_fast_path =
          gen->rit_entry != NULL &&
          gen->wmp_move_gen.num_tiles_played_through == 1 &&
          rack_info_table_has_playthrough_coverage(gen->rack_info_table,
                                                   gen->tiles_played);
      if (rit_fast_path) {
        const int leave_size = RACK_SIZE - gen->tiles_played;
        const uint32_t union_bitmask =
            rack_info_table_entry_get_playthrough_union(gen->rit_entry,
                                                        leave_size);
        const MachineLetter playthrough_ml =
            wmp_move_gen_single_playthrough_letter(&gen->wmp_move_gen);
        if (((union_bitmask >> playthrough_ml) & 1U) == 0) {
          return;
        }
      } else if (gen->tiles_played == gen->number_of_letters_on_rack) {
        // Fall back to the existing WMP check for full-rack cases not
        // handled by the RIT fast path: full rack + multi-playthrough,
        // or full rack + single-playthrough when the RIT is unavailable
        // or doesn't cover full-rack play.        // Multi-pt tp=7 bitvec
        // pre-filter ahead of the exact wmp walk. The RIT stores per word
        // length a uint32 with bit L set iff some 7-tile subrack of this rack +
        // {L} + (other letters) makes a word of that length. Tests each
        // playthrough letter against that single load. Only applies to 7-tile
        // bingo plays (tiles_played == RACK_SIZE) with >=2 playthrough tiles on
        // blankless racks.
        if (gen->rit_entry != NULL && gen->tiles_played == RACK_SIZE &&
            gen->wmp_move_gen.num_tiles_played_through >= 2 &&
            bit_rack_get_letter(&gen->wmp_move_gen.player_bit_rack,
                                BLANK_MACHINE_LETTER) == 0) {
          const int word_length =
              gen->tiles_played + gen->wmp_move_gen.num_tiles_played_through;
          uint32_t length_bitvec = 0;
          if (wmp_move_gen_get_multi_pt_bitvec_for_tiles_played(
                  gen->rit_entry, gen->tiles_played, word_length,
                  &length_bitvec) &&
              wmp_move_gen_multi_pt_bitvec_says_prune(&gen->wmp_move_gen,
                                                      length_bitvec)) {
            return;
          }
        }
        if (!wmp_move_gen_check_playthrough_full_rack_existence(
                &gen->wmp_move_gen)) {
          return;
        }
        // Exact WMP lookup confirmed word exists.
      } else {
        // Partial rack + multi-playthrough. Previously had no shadow-
        // time word-existence check at all. The tp=6 bitvec populated
        // by the maker fills that gap for the tiles_played == RACK_SIZE
        // - 1 case: load the per-length uint32 and AND across the
        // actual playthrough letters as a necessary condition. tp=5
        // was measured and dropped (4.7% prune rate didn't cover the
        // file-size cost); other tiles_played values still fall through
        // with no check.
        if (gen->rit_entry != NULL &&
            gen->wmp_move_gen.num_tiles_played_through >= 2 &&
            bit_rack_get_letter(&gen->wmp_move_gen.player_bit_rack,
                                BLANK_MACHINE_LETTER) == 0 &&
            gen->tiles_played == RACK_SIZE - 1) {
          const int word_length =
              gen->tiles_played + gen->wmp_move_gen.num_tiles_played_through;
          uint32_t length_bitvec = 0;
          if (wmp_move_gen_get_multi_pt_bitvec_for_tiles_played(
                  gen->rit_entry, gen->tiles_played, word_length,
                  &length_bitvec) &&
              wmp_move_gen_multi_pt_bitvec_says_prune(&gen->wmp_move_gen,
                                                      length_bitvec)) {
            return;
          }
        }
      }
    }
    if (!wmp_move_gen_has_playthrough(&gen->wmp_move_gen) &&
        (gen->tiles_played >= MINIMUM_WORD_LENGTH)) {
      if (!wmp_move_gen_nonplaythrough_word_of_length_exists(
              &gen->wmp_move_gen, gen->tiles_played)) {
        return;
      }
      if (gen->number_of_tiles_in_bag > 0) {
        best_leaves = wmp_move_gen_get_nonplaythrough_best_leave_values(
            &gen->wmp_move_gen);
        const int leave_size =
            gen->number_of_letters_on_rack - gen->tiles_played;
        (void)leave_size;
        assert(best_leaves[leave_size] <= gen->best_leaves[leave_size]);
      }
    }
  }

  // With the WMP on, its existence checks above have already answered this.
  if (gen->is_wordsmog && !wmp_active && (gen->tiles_played == RACK_SIZE) &&
      !kwg_accepts_alpha_with_blanks(gen->kwg, &gen->bingo_alpha_rack)) {
    return;
  }
  Equity tiles_played_score = 0;
  for (int i = 0; i < RACK_SIZE; i++) {
    tiles_played_score += gen->descending_tile_scores[i] *
                          gen->descending_effective_letter_multipliers[i];
  }

  Equity bingo_bonus = 0;
  if (gen->tiles_played == RACK_SIZE) {
    bingo_bonus = gen->bingo_bonus;
  }

  const Equity score =
      tiles_played_score +
      (gen->shadow_mainword_restricted_score * gen->shadow_word_multiplier) +
      gen->shadow_perpendicular_additional_score + bingo_bonus;
  Equity equity = score;
  if (gen->move_sort_type == MOVE_SORT_EQUITY) {
    equity += static_eval_get_shadow_equity(
        &gen->ld, &gen->opponent_rack, best_leaves,
        gen->full_rack_descending_tile_scores, gen->number_of_tiles_in_bag,
        gen->number_of_letters_on_rack, gen->tiles_played);
    // The PAT term is <= 0, so the bound would stay valid without
    // it, but then every anchor's bound is loose by roughly the position's
    // baseline penalty and anchors survive the cutoff on a penalty all of
    // their moves will pay. The recursive generator's anchors take the
    // per-lane bound (set when the lane is loaded; zero when the term is
    // off). WMP slots take one with a tighter utility correction, from
    // their tile count and score bound, once the anchor's shadow is done
    // (see gen_add_pat_bounds_to_wmp_anchors).
    if (!wmp_active) {
      equity += gen->pat_lane_penalty_bound;
    }
  }
  if (wmp_active) {
    const int word_length =
        gen->wmp_move_gen.num_tiles_played_through + gen->tiles_played;
    if (word_length >= MINIMUM_WORD_LENGTH) {
      wmp_move_gen_maybe_update_anchor(&gen->wmp_move_gen, gen->tiles_played,
                                       word_length, gen->current_left_col,
                                       score, equity);
    }
    // WMP move generation consumes the per-slot bounds above. The global
    // reductions below are only used to construct a legacy recursive-movegen
    // anchor, so maintaining them here would duplicate the same maxima.
    return;
  }
  if (equity > gen->highest_shadow_equity) {
    gen->highest_shadow_equity = equity;
  }
  if (score > gen->highest_shadow_score) {
    gen->highest_shadow_score = score;
  }
  if (gen->tiles_played > gen->max_tiles_to_play) {
    gen->max_tiles_to_play = gen->tiles_played;
  }
}

static __attribute__((noinline)) void
shadow_record_wmp(MoveGen *gen, uint32_t allowed_lengths) {
  shadow_record_impl(gen, true, allowed_lengths);
}

static __attribute__((noinline)) void shadow_record_recursive(MoveGen *gen) {
  shadow_record_impl(gen, false, UINT32_MAX);
}

static inline void shadow_record(MoveGen *gen, uint32_t allowed_lengths) {
  if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    shadow_record_wmp(gen, allowed_lengths);
  } else {
    shadow_record_recursive(gen);
  }
}

// Simplified shadow_record for small move types (BEST_SMALL).
// Skips WMP, wordsmog, and leave/equity computations.
static inline void shadow_record_small(MoveGen *gen) {
  Equity tiles_played_score = 0;
  for (int i = 0; i < RACK_SIZE; i++) {
    tiles_played_score += gen->descending_tile_scores[i] *
                          gen->descending_effective_letter_multipliers[i];
  }

  Equity bingo_bonus = 0;
  if (gen->tiles_played == RACK_SIZE) {
    bingo_bonus = gen->bingo_bonus;
  }

  const Equity score =
      tiles_played_score +
      (gen->shadow_mainword_restricted_score * gen->shadow_word_multiplier) +
      gen->shadow_perpendicular_additional_score + bingo_bonus;
  // Bound the ranking value, which includes the out bonus only for plays that
  // use the whole rack.
  Equity bound = score;
  if (gen->tiles_played == gen->best_small_out_rack_size) {
    bound += gen->best_small_out_bonus;
  }
  if (bound > gen->highest_shadow_score) {
    gen->highest_shadow_score = bound;
  }
  if (gen->tiles_played > gen->max_tiles_to_play) {
    gen->max_tiles_to_play = gen->tiles_played;
  }
}

static inline void
insert_unrestricted_effective_letter_multiplier(MoveGen *gen,
                                                uint8_t multiplier) {
  int insert_index = gen->num_unrestricted_multipliers;
  for (; insert_index > 0 &&
         gen->descending_effective_letter_multipliers[insert_index - 1] <
             multiplier;
       insert_index--) {
    gen->descending_effective_letter_multipliers[insert_index] =
        gen->descending_effective_letter_multipliers[insert_index - 1];
  }
  gen->descending_effective_letter_multipliers[insert_index] = multiplier;
}

// Recalculate and reinsert all unrestricted effective letter multipliers,
// triggered by a change in the word multiplier.
static inline void maybe_recalculate_effective_multipliers(MoveGen *gen) {
  if (gen->last_word_multiplier == gen->shadow_word_multiplier) {
    return;
  }
  gen->last_word_multiplier = gen->shadow_word_multiplier;

  const int original_num_unrestricted_multipliers =
      gen->num_unrestricted_multipliers;
  gen->num_unrestricted_multipliers = 0;
  for (int multiplier_idx = 0;
       multiplier_idx < original_num_unrestricted_multipliers;
       multiplier_idx++) {
    const uint8_t xw_multiplier =
        gen->unrestricted_multipliers[multiplier_idx].multiplier;
    const uint8_t col = gen->unrestricted_multipliers[multiplier_idx].column;
    const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, col);
    const uint8_t letter_multiplier =
        bonus_square_get_letter_multiplier(bonus_square);
    const uint8_t effective_letter_multiplier =
        gen->shadow_word_multiplier * letter_multiplier + xw_multiplier;
    insert_unrestricted_effective_letter_multiplier(
        gen, effective_letter_multiplier);
    gen->num_unrestricted_multipliers++;
  }
}

static inline void insert_unrestricted_multipliers(MoveGen *gen, int col) {
  maybe_recalculate_effective_multipliers(gen);

  const bool is_cross_word = gen_cache_get_is_cross_word(gen, col);
  const BonusSquare bonus_square = gen_cache_get_bonus_square(gen, col);
  const uint8_t this_word_multiplier =
      bonus_square_get_word_multiplier(bonus_square);
  const uint8_t letter_multiplier =
      bonus_square_get_letter_multiplier(bonus_square);
  const uint8_t effective_cross_word_multiplier =
      letter_multiplier * this_word_multiplier * is_cross_word;
  // Cross-word multipliers only retain the information needed to rebuild the
  // effective multipliers after the main-word multiplier changes. Their order
  // has no effect on the resulting sorted effective-multiplier list, so append
  // instead of maintaining a second sorted array in the shadow inner loop.
  gen->unrestricted_multipliers[gen->num_unrestricted_multipliers].multiplier =
      effective_cross_word_multiplier;
  gen->unrestricted_multipliers[gen->num_unrestricted_multipliers].column = col;
  const uint8_t main_word_multiplier =
      gen->shadow_word_multiplier * letter_multiplier;
  insert_unrestricted_effective_letter_multiplier(
      gen, main_word_multiplier + effective_cross_word_multiplier);
  gen->num_unrestricted_multipliers++;
}

static inline bool is_single_bit_set(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_popcountll)
  return __builtin_popcountll(bitset) == 1;
#else
  return bitset && !(bitset & (bitset - 1));
#endif
}

static inline int get_single_bit_index(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(bitset);
#else
  // Probably not the fastest fallback but it does well because it often
  // finds the blank's bit the first time through the loop.
  // Beware this infinite loops if bitset is zero.
  int index = 0;
  while ((bitset & 1) == 0) {
    bitset >>= 1;
    index++;
  }
  return index;
#endif
}

// descending_tile_scores is a list of tile scores in descending order.
// find and remove this score, shift the rest of the list to the left.
// fill the last element with 0.
static inline void remove_score_from_descending_tile_scores(MoveGen *gen,
                                                            Equity score) {
  // The tile has already been removed from the rack, so num_available_tiles
  // is actually the desired new size we want to be in sync with.
  const int num_available_tiles = rack_get_total_letters(&gen->player_rack);
  for (int i = num_available_tiles; i-- > 0;) {
    if (gen->descending_tile_scores[i] == score) {
      for (int j = i; j < num_available_tiles; j++) {
        gen->descending_tile_scores[j] = gen->descending_tile_scores[j + 1];
      }
      gen->descending_tile_scores[num_available_tiles] = 0;
      break;
    }
  }
}

// Removes the (single) tile in possible_letters_here from the rack and adds
// the score. Callers must have already established that possible_letters_here
// has exactly one bit set.
static inline void
restrict_tile_and_accumulate_score(MoveGen *gen, uint64_t possible_letters_here,
                                   int letter_multiplier,
                                   int this_word_multiplier, int col) {
  const MachineLetter ml = get_single_bit_index(possible_letters_here);
  rack_take_letter(&gen->player_rack, ml);
  if (rack_get_letter(&gen->player_rack, ml) == 0) {
    gen->rack_cross_set &= ~possible_letters_here;
  }
  const bool is_cross_word = gen_cache_get_is_cross_word(gen, col);
  const Equity tile_score = gen->tile_scores[ml];
  remove_score_from_descending_tile_scores(gen, tile_score);
  const Equity lsm = tile_score * letter_multiplier;
  gen->shadow_mainword_restricted_score += lsm;

  // Equivalent to
  //
  //   gen->shadow_perpendicular_additional_score +=
  //     is_cross_word ? (lsm * this_word_multiplier) : 0;
  //
  // But verified with godbolt that this compiles to faster code.
  gen->shadow_perpendicular_additional_score +=
      (lsm * this_word_multiplier) & (-(int)is_cross_word);
}

// Returns false if the tile can not be restricted.
// Otherwise it removes the tile from the rack and adds the score.
static inline bool try_restrict_tile_and_accumulate_score(
    MoveGen *gen, uint64_t possible_letters_here, int letter_multiplier,
    int this_word_multiplier, int col) {
  if (!is_single_bit_set(possible_letters_here)) {
    return false;
  }
  restrict_tile_and_accumulate_score(
      gen, possible_letters_here, letter_multiplier, this_word_multiplier, col);
  return true;
}

// Sound shadow early-stop using the WordInfoTable. After a rightward shadow
// step places a tile and walks through a playthrough block beginning at
// `block_col`, every tile in the main word must be an addable letter of that
// block for the block's containing word at its *eventual* length. Shadow does
// not know that length yet, only a minimum (`min_word_length`), so we take the
// cumulative "length >= min" addable set: the suffix-OR of the block's WIT row
// from index `min_word_length - block_len`. Lengthening the word only shrinks
// this set, so if the just-placed tile (achievable letters
// `possible_letters_here`, including a wild blank when one is playable here)
// cannot be in it, no rightward continuation forms a word and the caller stops.
//
// The blank is treated conservatively: when a blank can be placed at this
// square (bit 0 of `possible_letters_here`), it is assumed able to realize any
// addable letter, so we only declare the step dead when no addable letter
// exists at all. Blank-placeability must be read from `possible_letters_here`
// rather than the live rack, because a square restricted to a blank has already
// consumed that blank from the rack by this point. This never prunes a legal
// play.
static inline bool wit_shadow_right_block_dead(const MoveGen *gen,
                                               int block_col,
                                               uint64_t possible_letters_here,
                                               int min_word_length) {
  const uint32_t *block_row = gen->wit_row_lane[block_col];
  if (block_row == NULL) {
    return false;
  }
  const int block_len = gen->wit_len_lane[block_col];
  int min_idx = min_word_length - block_len;
  if (min_idx < 0) {
    min_idx = 0;
  }
  const bool blank_here = (possible_letters_here & 1U) != 0;
  const uint32_t cand = (uint32_t)possible_letters_here & ~1U;
  const int stride = BOARD_DIM - block_len + 1;
  uint32_t geq = 0;
  for (int idx = min_idx; idx < stride; idx++) {
    geq |= block_row[idx];
    if ((cand & geq) != 0 || (blank_here && geq != 0)) {
      return false;
    }
  }
  return true;
}

// Only a complete, owned block contained in this shadow span can constrain
// word positions. Missing, foreign, and partial cached rows remain permissive.
static inline const uint32_t *shadow_position_row_for_block(const MoveGen *gen,
                                                            int block_col) {
  if (!wmp_move_gen_is_active(&gen->wmp_move_gen) || gen->is_wordsmog ||
      gen->word_info_table == NULL || gen->wit_row_lane == NULL ||
      gen->wit_len_lane == NULL || block_col < gen->current_left_col ||
      block_col < 0 || block_col >= BOARD_DIM) {
    return NULL;
  }
  const int block_length = gen->wit_len_lane[block_col];
  if (block_length < WIT_POSITION_MIN_BASE_LENGTH ||
      block_length > WIT_POSITION_MAX_BASE_LENGTH ||
      block_col + block_length - 1 > gen->current_right_col ||
      (block_col > 0 && gen_cache_get_letter(gen, block_col - 1) !=
                            ALPHABET_EMPTY_SQUARE_MARKER) ||
      (block_col + block_length < BOARD_DIM &&
       gen_cache_get_letter(gen, block_col + block_length) !=
           ALPHABET_EMPTY_SQUARE_MARKER)) {
    return NULL;
  }
  return word_info_table_get_position_lengths(
      gen->word_info_table, gen->wit_row_lane[block_col], block_length);
}

static inline uint32_t shadow_initial_position_lengths(const MoveGen *gen) {
  if (gen->shadow_position_row == NULL) {
    return UINT32_MAX;
  }
  const int position = gen->shadow_position_col - gen->current_left_col;
  if (position < 0 || position > BOARD_DIM - gen->shadow_position_length) {
    return UINT32_MAX;
  }
  return gen->shadow_position_row[position];
}

static inline void shadow_play_right(MoveGen *gen, bool is_unique,
                                     uint32_t allowed_lengths) {
  int position_base_length = gen->shadow_position_length;

  // Save the score totals to be reset after shadowing right.
  const Equity orig_main_restricted_score =
      gen->shadow_mainword_restricted_score;
  const Equity orig_perp_score = gen->shadow_perpendicular_additional_score;
  const int orig_wordmul = gen->shadow_word_multiplier;

  // The rack with the tiles available before beginning shadow right is
  // saved lazily: any tiles restricted by unique hooks modify the rack, so
  // the rack (and the descending tile scores) are copied just before the
  // first restriction and restored after exhausting rightward shadow.
  const uint64_t orig_rack_cross_set = gen->rack_cross_set;
  // Only recopy the originals if a restriction modified the arrays above.
  bool restricted_any_tiles = false;

  if (gen->is_wordsmog) {
    rack_copy(&gen->bingo_alpha_rack_shadow_right_copy, &gen->bingo_alpha_rack);
  }

  // The state of the unrestricted multiplier arrays is saved lazily (just
  // before the first modification) so they can be restored after exhausting
  // the rightward shadow plays. Shadowing right changes the values of
  // multipliers found while shadowing left. We need to restore them to how
  // they were before looking further left.
  const int orig_num_unrestricted_multipliers =
      gen->num_unrestricted_multipliers;
  bool changed_any_unrestricted_multipliers = false;

  const int original_current_right_col = gen->current_right_col;
  const int original_tiles_played = gen->tiles_played;
  // Playthrough state changes only when this shadow crosses an existing board
  // block. Avoid snapshotting it for the common empty-rightward path.
  bool changed_wmp_playthrough_state = false;

  while (gen->current_right_col < (BOARD_DIM - 1) &&
         gen->tiles_played < gen->number_of_letters_on_rack) {
    // The next empty square makes the word at least one letter longer.
    // Keep this stop local to the rightward branch: extending left changes
    // the base's position and may revive lengths excluded at this start.
    const int next_min_length =
        gen->current_right_col - gen->current_left_col + 2;
    if (position_base_length != 0 &&
        (allowed_lengths >> next_min_length) == 0) {
      break;
    }
    gen->current_right_col++;
    gen->tiles_played++;

    const uint64_t cross_set =
        gen_cache_get_cross_set(gen, gen->current_right_col);
    // If there are tiles to the right of this empty square, this holds a bitset
    // of the tiles that could possibly extend that sequence one square to the
    // left. Otherwise current_leftx will just be TRIVIAL_CROSS_SET.
    const uint64_t current_leftx =
        gen_cache_get_left_extension_set(gen, gen->current_right_col);
    const uint64_t nonblank_leftx =
        current_leftx & ~(1ULL << BLANK_MACHINE_LETTER);
    const uint64_t nonblank_cross_set =
        cross_set & ~(1ULL << BLANK_MACHINE_LETTER);
    // Which *letters* can be played here? Here we do not consider the blank to
    // be a letter. The letter it is designated must be compatible with both the
    // cross set and the left extension set.
    if ((nonblank_cross_set & nonblank_leftx) == 0) {
      break;
    }
    const uint64_t possible_letters_here = cross_set & gen->rack_cross_set &
                                           gen->anchor_right_extension_set &
                                           current_leftx;
    gen->anchor_right_extension_set = TRIVIAL_CROSS_SET;
    if (possible_letters_here == 0) {
      break;
    }
    if (gen->tiles_played == 1) {
      gen->first_played_tile_col = gen->current_right_col;
    }

    const BonusSquare bonus_square =
        gen_cache_get_bonus_square(gen, gen->current_right_col);
    const Equity cross_score =
        gen_cache_get_cross_score(gen, gen->current_right_col);
    const int letter_multiplier =
        bonus_square_get_letter_multiplier(bonus_square);
    const int this_word_multiplier =
        bonus_square_get_word_multiplier(bonus_square);
    gen->shadow_perpendicular_additional_score +=
        cross_score * this_word_multiplier;
    gen->shadow_word_multiplier *= this_word_multiplier;

    if (is_single_bit_set(possible_letters_here)) {
      if (!restricted_any_tiles) {
        // First restriction in this rightward shadow: save the rack and
        // descending tile scores so they can be restored on exit.
        rack_copy(&gen->player_rack_shadow_right_copy, &gen->player_rack);
        memcpy(gen->descending_tile_scores_copy, gen->descending_tile_scores,
               sizeof(gen->descending_tile_scores));
        restricted_any_tiles = true;
      }
      restrict_tile_and_accumulate_score(
          gen, possible_letters_here, letter_multiplier, this_word_multiplier,
          gen->current_right_col);
    } else {
      if (!changed_any_unrestricted_multipliers) {
        // Backup entries are append-only: restoring the count discards the
        // rightward additions. Only the sorted effective array needs a copy.
        memcpy(gen->desc_eff_letter_muls_copy,
               gen->descending_effective_letter_multipliers,
               sizeof(gen->descending_effective_letter_multipliers));
        changed_any_unrestricted_multipliers = true;
      }
      insert_unrestricted_multipliers(gen, gen->current_right_col);
    }
    if (cross_set == TRIVIAL_CROSS_SET) {
      is_unique = true;
    }
    bool found_playthrough_tile = false;
    const int right_block_col = gen->current_right_col + 1;
    while (gen->current_right_col + 1 < BOARD_DIM) {
      const MachineLetter next_letter =
          gen_cache_get_letter(gen, gen->current_right_col + 1);
      if (next_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        break;
      }
      found_playthrough_tile = true;
      const MachineLetter unblanked_playthrough_ml =
          get_unblanked_machine_letter(next_letter);
      if (gen->is_wordsmog) {
        // bingo_alpha_rack is only read by the wordsmog alphagram check in
        // shadow_record, so skip maintaining it otherwise.
        rack_add_letter(&gen->bingo_alpha_rack, unblanked_playthrough_ml);
      }
      // Adding a letter here would be unsafe if the LetterDistribution's
      // alphabet size exceeded BIT_RACK_MAX_ALPHABET_SIZE.
      if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
        if (!changed_wmp_playthrough_state) {
          wmp_move_gen_save_playthrough_state(&gen->wmp_move_gen);
          changed_wmp_playthrough_state = true;
        }
        wmp_move_gen_add_playthrough_letter(&gen->wmp_move_gen,
                                            unblanked_playthrough_ml);
      }
      gen->shadow_mainword_restricted_score += gen->tile_scores[next_letter];
      gen->current_right_col++;
    }

    if (wmp_move_gen_is_active(&gen->wmp_move_gen) && found_playthrough_tile) {
      wmp_move_gen_increment_playthrough_blocks(&gen->wmp_move_gen);
    }

    if (found_playthrough_tile && position_base_length == 0) {
      const uint32_t *position_row =
          shadow_position_row_for_block(gen, right_block_col);
      if (position_row != NULL) {
        position_base_length = gen->wit_len_lane[right_block_col];
        allowed_lengths = position_row[right_block_col - gen->current_left_col];
      }
    }

    // WordInfoTable shadow early-stop: if the tile just placed can never be an
    // addable letter of the right playthrough block at any reachable word
    // length, no rightward continuation forms a word, so stop shadowing right.
    if (found_playthrough_tile && gen->word_info_table != NULL &&
        wmp_move_gen_is_active(&gen->wmp_move_gen)) {
      const int min_word_length =
          gen->wmp_move_gen.num_tiles_played_through + gen->tiles_played;
      if (wit_shadow_right_block_dead(gen, right_block_col,
                                      possible_letters_here, min_word_length)) {
        break;
      }
    }

    if (play_is_nonempty_and_nonduplicate(gen->tiles_played, is_unique)) {
      // word_multiplier may have changed while playing through restricted
      // squares, in which case the restricted multiplier squares would
      // be invalidated.
      maybe_recalculate_effective_multipliers(gen);
      shadow_record(gen, allowed_lengths);
    }
  }

  // Restore state for score totals
  gen->shadow_mainword_restricted_score = orig_main_restricted_score;
  gen->shadow_perpendicular_additional_score = orig_perp_score;
  gen->shadow_word_multiplier = orig_wordmul;

  // Restore state for restricted squares
  if (restricted_any_tiles) {
    rack_copy(&gen->player_rack, &gen->player_rack_shadow_right_copy);
    gen->rack_cross_set = orig_rack_cross_set;
    memcpy(gen->descending_tile_scores, gen->descending_tile_scores_copy,
           sizeof(gen->descending_tile_scores));
  }

  // Restore state for unrestricted squares
  if (changed_any_unrestricted_multipliers) {
    gen->num_unrestricted_multipliers = orig_num_unrestricted_multipliers;
    memcpy(gen->descending_effective_letter_multipliers,
           gen->desc_eff_letter_muls_copy,
           sizeof(gen->descending_effective_letter_multipliers));
  }

  // Restore state to undo other shadow progress
  gen->current_right_col = original_current_right_col;
  gen->tiles_played = original_tiles_played;
  if (gen->is_wordsmog) {
    rack_copy(&gen->bingo_alpha_rack, &gen->bingo_alpha_rack_shadow_right_copy);
  }
  if (changed_wmp_playthrough_state) {
    wmp_move_gen_restore_playthrough_state(&gen->wmp_move_gen);
  }

  // The change of shadow_word_multiplier necessitates recalculating effective
  // multipliers.
  maybe_recalculate_effective_multipliers(gen);
}

// Simplified shadow_play_right for small move types.
// Skips bingo_alpha_rack and WMP operations.
static inline void shadow_play_right_small(MoveGen *gen, bool is_unique) {
  const Equity orig_main_restricted_score =
      gen->shadow_mainword_restricted_score;
  const Equity orig_perp_score = gen->shadow_perpendicular_additional_score;
  const int orig_wordmul = gen->shadow_word_multiplier;

  // The rack/tile-score and multiplier-array snapshots are saved LAZILY on the
  // first restriction / first unrestricted insertion in the loop below,
  // mirroring the non-small shadow_play_right. Most rightward shadows over the
  // short racks of an endgame restrict nothing, so these ~132 bytes of wide
  // copies are usually never paid. orig_rack_cross_set and
  // orig_num_unrestricted_multipliers are cheap scalars, kept eager.
  const uint64_t orig_rack_cross_set = gen->rack_cross_set;
  bool restricted_any_tiles = false;
  const int orig_num_unrestricted_multipliers =
      gen->num_unrestricted_multipliers;
  bool changed_any_unrestricted_multipliers = false;

  const int original_current_right_col = gen->current_right_col;
  const int original_tiles_played = gen->tiles_played;

  while (gen->current_right_col < (BOARD_DIM - 1) &&
         gen->tiles_played < gen->number_of_letters_on_rack) {
    gen->current_right_col++;
    gen->tiles_played++;

    const uint64_t cross_set =
        gen_cache_get_cross_set(gen, gen->current_right_col);
    const uint64_t current_leftx =
        gen_cache_get_left_extension_set(gen, gen->current_right_col);
    const uint64_t nonblank_leftx =
        current_leftx & ~(1ULL << BLANK_MACHINE_LETTER);
    const uint64_t nonblank_cross_set =
        cross_set & ~(1ULL << BLANK_MACHINE_LETTER);
    if ((nonblank_cross_set & nonblank_leftx) == 0) {
      break;
    }
    const uint64_t possible_letters_here = cross_set & gen->rack_cross_set &
                                           gen->anchor_right_extension_set &
                                           current_leftx;
    gen->anchor_right_extension_set = TRIVIAL_CROSS_SET;
    if (possible_letters_here == 0) {
      break;
    }
    if (gen->tiles_played == 1) {
      gen->first_played_tile_col = gen->current_right_col;
    }

    const BonusSquare bonus_square =
        gen_cache_get_bonus_square(gen, gen->current_right_col);
    const Equity cross_score =
        gen_cache_get_cross_score(gen, gen->current_right_col);
    const int letter_multiplier =
        bonus_square_get_letter_multiplier(bonus_square);
    const int this_word_multiplier =
        bonus_square_get_word_multiplier(bonus_square);
    gen->shadow_perpendicular_additional_score +=
        cross_score * this_word_multiplier;
    gen->shadow_word_multiplier *= this_word_multiplier;

    if (is_single_bit_set(possible_letters_here)) {
      if (!restricted_any_tiles) {
        // First restriction in this rightward shadow: snapshot the rack and
        // descending tile scores that restrict_tile_and_accumulate_score is
        // about to mutate. Must save BEFORE the mutating call.
        rack_copy(&gen->player_rack_shadow_right_copy, &gen->player_rack);
        memcpy(gen->descending_tile_scores_copy, gen->descending_tile_scores,
               sizeof(gen->descending_tile_scores));
        restricted_any_tiles = true;
      }
      restrict_tile_and_accumulate_score(
          gen, possible_letters_here, letter_multiplier, this_word_multiplier,
          gen->current_right_col);
    } else {
      if (!changed_any_unrestricted_multipliers) {
        // Backup entries are append-only; only the sorted array is mutated.
        memcpy(gen->desc_eff_letter_muls_copy,
               gen->descending_effective_letter_multipliers,
               sizeof(gen->descending_effective_letter_multipliers));
        changed_any_unrestricted_multipliers = true;
      }
      insert_unrestricted_multipliers(gen, gen->current_right_col);
    }
    if (cross_set == TRIVIAL_CROSS_SET) {
      is_unique = true;
    }
    while (gen->current_right_col + 1 < BOARD_DIM) {
      const MachineLetter next_letter =
          gen_cache_get_letter(gen, gen->current_right_col + 1);
      if (next_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        break;
      }
      gen->shadow_mainword_restricted_score += gen->tile_scores[next_letter];
      gen->current_right_col++;
    }

    if (play_is_nonempty_and_nonduplicate(gen->tiles_played, is_unique)) {
      maybe_recalculate_effective_multipliers(gen);
      shadow_record_small(gen);
    }
  }

  gen->shadow_mainword_restricted_score = orig_main_restricted_score;
  gen->shadow_perpendicular_additional_score = orig_perp_score;
  gen->shadow_word_multiplier = orig_wordmul;

  if (restricted_any_tiles) {
    rack_copy(&gen->player_rack, &gen->player_rack_shadow_right_copy);
    gen->rack_cross_set = orig_rack_cross_set;
    memcpy(gen->descending_tile_scores, gen->descending_tile_scores_copy,
           sizeof(gen->descending_tile_scores));
  }

  if (changed_any_unrestricted_multipliers) {
    gen->num_unrestricted_multipliers = orig_num_unrestricted_multipliers;
    memcpy(gen->descending_effective_letter_multipliers,
           gen->desc_eff_letter_muls_copy,
           sizeof(gen->descending_effective_letter_multipliers));
  }

  gen->current_right_col = original_current_right_col;
  gen->tiles_played = original_tiles_played;

  maybe_recalculate_effective_multipliers(gen);
}

static inline void nonplaythrough_shadow_play_left(MoveGen *gen,
                                                   bool is_unique) {
  for (;;) {
    const uint64_t possible_tiles_for_shadow_right =
        gen->anchor_right_extension_set & gen->rack_cross_set;
    if (possible_tiles_for_shadow_right != 0) {
      shadow_play_right(gen, is_unique, UINT32_MAX);
    }
    gen->anchor_right_extension_set = TRIVIAL_CROSS_SET;
    if (gen->current_left_col == 0 ||
        gen->current_left_col == gen->last_anchor_col + 1 ||
        gen->tiles_played >= gen->number_of_letters_on_rack) {
      return;
    }
    const uint64_t possible_tiles_for_shadow_left =
        gen->anchor_left_extension_set & gen->rack_cross_set;
    if (possible_tiles_for_shadow_left == 0) {
      return;
    }
    gen->anchor_left_extension_set = TRIVIAL_CROSS_SET;

    gen->current_left_col--;
    gen->tiles_played++;
    assert(gen->tiles_played > 1);
    const BonusSquare bonus_square =
        gen_cache_get_bonus_square(gen, gen->current_left_col);
    int letter_multiplier = bonus_square_get_letter_multiplier(bonus_square);
    int this_word_multiplier = bonus_square_get_word_multiplier(bonus_square);
    gen->shadow_word_multiplier *= this_word_multiplier;
    if (!try_restrict_tile_and_accumulate_score(
            gen, possible_tiles_for_shadow_left, letter_multiplier,
            this_word_multiplier, gen->current_left_col)) {
      insert_unrestricted_multipliers(gen, gen->current_left_col);
    }
    shadow_record(gen, UINT32_MAX);
  }
}

static inline void nonplaythrough_shadow_play_left_small(MoveGen *gen,
                                                         bool is_unique) {
  for (;;) {
    const uint64_t possible_tiles_for_shadow_right =
        gen->anchor_right_extension_set & gen->rack_cross_set;
    if (possible_tiles_for_shadow_right != 0) {
      shadow_play_right_small(gen, is_unique);
    }
    gen->anchor_right_extension_set = TRIVIAL_CROSS_SET;
    if (gen->current_left_col == 0 ||
        gen->current_left_col == gen->last_anchor_col + 1 ||
        gen->tiles_played >= gen->number_of_letters_on_rack) {
      return;
    }
    const uint64_t possible_tiles_for_shadow_left =
        gen->anchor_left_extension_set & gen->rack_cross_set;
    if (possible_tiles_for_shadow_left == 0) {
      return;
    }
    gen->anchor_left_extension_set = TRIVIAL_CROSS_SET;

    gen->current_left_col--;
    gen->tiles_played++;
    assert(gen->tiles_played > 1);
    const BonusSquare bonus_square =
        gen_cache_get_bonus_square(gen, gen->current_left_col);
    int letter_multiplier = bonus_square_get_letter_multiplier(bonus_square);
    int this_word_multiplier = bonus_square_get_word_multiplier(bonus_square);
    gen->shadow_word_multiplier *= this_word_multiplier;
    if (!try_restrict_tile_and_accumulate_score(
            gen, possible_tiles_for_shadow_left, letter_multiplier,
            this_word_multiplier, gen->current_left_col)) {
      insert_unrestricted_multipliers(gen, gen->current_left_col);
    }
    shadow_record_small(gen);
  }
}

static inline void playthrough_shadow_play_left(MoveGen *gen, bool is_unique) {
  uint32_t allowed_lengths = shadow_initial_position_lengths(gen);
  for (;;) {
    const uint64_t possible_tiles_for_shadow_right =
        gen->anchor_right_extension_set & gen->rack_cross_set;
    if (possible_tiles_for_shadow_right != 0) {
      shadow_play_right(gen, is_unique, allowed_lengths);
    }
    gen->anchor_right_extension_set = TRIVIAL_CROSS_SET;

    uint64_t possible_tiles_for_shadow_left =
        gen->anchor_left_extension_set & gen->rack_cross_set;
    const uint64_t nonblank_leftx =
        gen->anchor_left_extension_set & ~(1ULL << BLANK_MACHINE_LETTER);
    gen->anchor_left_extension_set = TRIVIAL_CROSS_SET;

    if (gen->current_left_col == 0 ||
        gen->current_left_col == gen->last_anchor_col + 1 ||
        gen->tiles_played >= gen->number_of_letters_on_rack) {
      break;
    }
    if (possible_tiles_for_shadow_left == 0) {
      break;
    }
    gen->current_left_col--;
    gen->tiles_played++;
    const uint64_t cross_set =
        gen_cache_get_cross_set(gen, gen->current_left_col);
    const uint64_t nonblank_cross_set =
        cross_set & ~(1ULL << BLANK_MACHINE_LETTER);
    // Which *letters* can be played here? Here we do not consider the blank to
    // be a letter. The letter it is designated must be compatible with both the
    // cross set and the left extension set.
    if ((nonblank_cross_set & nonblank_leftx) == 0) {
      break;
    }
    possible_tiles_for_shadow_left &= cross_set;
    if (possible_tiles_for_shadow_left == 0) {
      break;
    }
    if (gen->tiles_played == 1) {
      gen->first_played_tile_col = gen->current_left_col;
    }

    const BonusSquare bonus_square =
        gen_cache_get_bonus_square(gen, gen->current_left_col);
    const Equity cross_score =
        gen_cache_get_cross_score(gen, gen->current_left_col);
    const int letter_multiplier =
        bonus_square_get_letter_multiplier(bonus_square);
    const int this_word_multiplier =
        bonus_square_get_word_multiplier(bonus_square);
    gen->shadow_perpendicular_additional_score +=
        cross_score * this_word_multiplier;

    gen->shadow_word_multiplier *= this_word_multiplier;
    if (!try_restrict_tile_and_accumulate_score(
            gen, possible_tiles_for_shadow_left, letter_multiplier,
            this_word_multiplier, gen->current_left_col)) {
      insert_unrestricted_multipliers(gen, gen->current_left_col);
    }

    if (cross_set == TRIVIAL_CROSS_SET) {
      // See equivalent in shadow_play_right for the reasoning here.
      is_unique = true;
    }

    allowed_lengths = shadow_initial_position_lengths(gen);
    if (play_is_nonempty_and_nonduplicate(gen->tiles_played, is_unique)) {
      shadow_record(gen, allowed_lengths);
    }
  }
}

static inline void playthrough_shadow_play_left_small(MoveGen *gen,
                                                      bool is_unique) {
  for (;;) {
    const uint64_t possible_tiles_for_shadow_right =
        gen->anchor_right_extension_set & gen->rack_cross_set;
    if (possible_tiles_for_shadow_right != 0) {
      shadow_play_right_small(gen, is_unique);
    }
    gen->anchor_right_extension_set = TRIVIAL_CROSS_SET;

    uint64_t possible_tiles_for_shadow_left =
        gen->anchor_left_extension_set & gen->rack_cross_set;
    const uint64_t nonblank_leftx =
        gen->anchor_left_extension_set & ~(1ULL << BLANK_MACHINE_LETTER);
    gen->anchor_left_extension_set = TRIVIAL_CROSS_SET;

    if (gen->current_left_col == 0 ||
        gen->current_left_col == gen->last_anchor_col + 1 ||
        gen->tiles_played >= gen->number_of_letters_on_rack) {
      break;
    }
    if (possible_tiles_for_shadow_left == 0) {
      break;
    }
    gen->current_left_col--;
    gen->tiles_played++;
    const uint64_t cross_set =
        gen_cache_get_cross_set(gen, gen->current_left_col);
    const uint64_t nonblank_cross_set =
        cross_set & ~(1ULL << BLANK_MACHINE_LETTER);
    if ((nonblank_cross_set & nonblank_leftx) == 0) {
      break;
    }
    possible_tiles_for_shadow_left &= cross_set;
    if (possible_tiles_for_shadow_left == 0) {
      break;
    }
    if (gen->tiles_played == 1) {
      gen->first_played_tile_col = gen->current_left_col;
    }

    const BonusSquare bonus_square =
        gen_cache_get_bonus_square(gen, gen->current_left_col);
    const Equity cross_score =
        gen_cache_get_cross_score(gen, gen->current_left_col);
    const int letter_multiplier =
        bonus_square_get_letter_multiplier(bonus_square);
    const int this_word_multiplier =
        bonus_square_get_word_multiplier(bonus_square);
    gen->shadow_perpendicular_additional_score +=
        cross_score * this_word_multiplier;

    gen->shadow_word_multiplier *= this_word_multiplier;
    if (!try_restrict_tile_and_accumulate_score(
            gen, possible_tiles_for_shadow_left, letter_multiplier,
            this_word_multiplier, gen->current_left_col)) {
      insert_unrestricted_multipliers(gen, gen->current_left_col);
    }

    if (cross_set == TRIVIAL_CROSS_SET) {
      is_unique = true;
    }

    if (play_is_nonempty_and_nonduplicate(gen->tiles_played, is_unique)) {
      shadow_record_small(gen);
    }
  }
}

static inline void shadow_start_nonplaythrough(MoveGen *gen) {
  const uint64_t cross_set =
      gen_cache_get_cross_set(gen, gen->current_left_col);
  gen->first_played_tile_col = gen->current_left_col;
  const uint64_t possible_letters_here = cross_set & gen->rack_cross_set;
  // Only play a letter if a letter from the rack fits in the cross set
  if (possible_letters_here == 0) {
    return;
  }

  // Play tile on empty anchor square and set scoring parameters
  const BonusSquare bonus_square =
      gen_cache_get_bonus_square(gen, gen->current_left_col);
  const Equity cross_score =
      gen_cache_get_cross_score(gen, gen->current_left_col);
  const int letter_multiplier =
      bonus_square_get_letter_multiplier(bonus_square);
  const int this_word_multiplier =
      bonus_square_get_word_multiplier(bonus_square);
  gen->shadow_perpendicular_additional_score =
      cross_score * this_word_multiplier;

  // Temporarily set to zero, to not score in the other direction.
  gen->shadow_word_multiplier = 0;
  if (!try_restrict_tile_and_accumulate_score(
          gen, possible_letters_here, letter_multiplier, this_word_multiplier,
          gen->current_left_col)) {
    insert_unrestricted_multipliers(gen, gen->current_left_col);
  }
  gen->tiles_played++;
  if (!board_is_dir_vertical(gen->dir)) {
    // word_multiplier is always hard-coded as 0 since we are recording a
    // single tile
    shadow_record(gen, UINT32_MAX);
  }
  gen->shadow_word_multiplier = this_word_multiplier;
  maybe_recalculate_effective_multipliers(gen);

  nonplaythrough_shadow_play_left(gen, !board_is_dir_vertical(gen->dir));
}

static inline void shadow_start_nonplaythrough_small(MoveGen *gen) {
  const uint64_t cross_set =
      gen_cache_get_cross_set(gen, gen->current_left_col);
  gen->first_played_tile_col = gen->current_left_col;
  const uint64_t possible_letters_here = cross_set & gen->rack_cross_set;
  if (possible_letters_here == 0) {
    return;
  }

  const BonusSquare bonus_square =
      gen_cache_get_bonus_square(gen, gen->current_left_col);
  const Equity cross_score =
      gen_cache_get_cross_score(gen, gen->current_left_col);
  const int letter_multiplier =
      bonus_square_get_letter_multiplier(bonus_square);
  const int this_word_multiplier =
      bonus_square_get_word_multiplier(bonus_square);
  gen->shadow_perpendicular_additional_score =
      cross_score * this_word_multiplier;

  gen->shadow_word_multiplier = 0;
  if (!try_restrict_tile_and_accumulate_score(
          gen, possible_letters_here, letter_multiplier, this_word_multiplier,
          gen->current_left_col)) {
    insert_unrestricted_multipliers(gen, gen->current_left_col);
  }
  gen->tiles_played++;
  if (!board_is_dir_vertical(gen->dir)) {
    shadow_record_small(gen);
  }
  gen->shadow_word_multiplier = this_word_multiplier;
  maybe_recalculate_effective_multipliers(gen);

  nonplaythrough_shadow_play_left_small(gen, !board_is_dir_vertical(gen->dir));
}

static inline void shadow_start_playthrough(MoveGen *gen,
                                            MachineLetter current_letter) {
  // Traverse the full length of the tiles on the board until hitting an
  // empty square
  for (;;) {
    const MachineLetter unblanked_playthrough_ml =
        get_unblanked_machine_letter(current_letter);
    if (gen->is_wordsmog) {
      // bingo_alpha_rack is only read by the wordsmog alphagram check in
      // shadow_record, so skip maintaining it otherwise.
      rack_add_letter(&gen->bingo_alpha_rack, unblanked_playthrough_ml);
    }
    if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
      wmp_move_gen_add_playthrough_letter(&gen->wmp_move_gen,
                                          unblanked_playthrough_ml);
    }
    gen->shadow_mainword_restricted_score += gen->tile_scores[current_letter];
    if (gen->current_left_col == 0 ||
        gen->current_left_col == gen->last_anchor_col + 1) {
      break;
    }
    gen->current_left_col--;
    current_letter = gen_cache_get_letter(gen, gen->current_left_col);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      gen->current_left_col++;
      break;
    }
  }
  if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    wmp_move_gen_increment_playthrough_blocks(&gen->wmp_move_gen);
  }
  gen->shadow_position_row =
      shadow_position_row_for_block(gen, gen->current_left_col);
  if (gen->shadow_position_row != NULL) {
    gen->shadow_position_col = gen->current_left_col;
    gen->shadow_position_length = gen->wit_len_lane[gen->current_left_col];
  }
  playthrough_shadow_play_left(gen, !board_is_dir_vertical(gen->dir));
}

// Simplified shadow_start_playthrough for small move types.
// Skips bingo_alpha_rack and WMP operations.
static inline void
shadow_start_playthrough_small(MoveGen *gen, MachineLetter current_letter) {
  for (;;) {
    gen->shadow_mainword_restricted_score += gen->tile_scores[current_letter];
    if (gen->current_left_col == 0 ||
        gen->current_left_col == gen->last_anchor_col + 1) {
      break;
    }
    gen->current_left_col--;
    current_letter = gen_cache_get_letter(gen, gen->current_left_col);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      gen->current_left_col++;
      break;
    }
  }
  playthrough_shadow_play_left_small(gen, !board_is_dir_vertical(gen->dir));
}

static inline void shadow_start(MoveGen *gen) {
  const uint64_t any_extension_set =
      gen->anchor_left_extension_set | gen->anchor_right_extension_set;
  if (any_extension_set == 0) {
    return;
  }

  const uint64_t original_rack_cross_set = gen->rack_cross_set;
  rack_copy(&gen->full_player_rack, &gen->player_rack);
  if (gen->is_wordsmog) {
    rack_copy(&gen->bingo_alpha_rack, &gen->player_rack);
  }

  const MachineLetter current_letter =
      gen_cache_get_letter(gen, gen->current_left_col);
  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    shadow_start_nonplaythrough(gen);
  } else {
    shadow_start_playthrough(gen, current_letter);
  }

  gen->rack_cross_set = original_rack_cross_set;
  rack_copy(&gen->player_rack, &gen->full_player_rack);
}

// Simplified shadow_start for small move types.
// Skips bingo_alpha_rack copy.
static inline void shadow_start_small(MoveGen *gen) {
  const uint64_t any_extension_set =
      gen->anchor_left_extension_set | gen->anchor_right_extension_set;
  if (any_extension_set == 0) {
    return;
  }

  const uint64_t original_rack_cross_set = gen->rack_cross_set;

  const MachineLetter current_letter =
      gen_cache_get_letter(gen, gen->current_left_col);
  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    shadow_start_nonplaythrough_small(gen);
  } else {
    shadow_start_playthrough_small(gen, current_letter);
  }

  gen->rack_cross_set = original_rack_cross_set;
  rack_copy(&gen->player_rack, &gen->full_player_rack);
}

// The algorithm used in this file for
// shadow playing was originally developed in wolges.
// For more details about the shadow playing algorithm, see
// https://github.com/andy-k/wolges/blob/main/details.txt
// Adds each touched WMP slot's PAT bound (see gen_wmp_anchor_pat_bound) to
// its equity bound, which shadow_record left without the term.
static void gen_add_pat_bounds_to_wmp_anchors(MoveGen *gen) {
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  for (int word_idx = 0; word_idx < WMP_ANCHOR_MASK_WORDS; word_idx++) {
    uint64_t touched = wgen->touched_anchor_masks[word_idx];
    while (touched != 0) {
      const int slot_idx =
          word_idx * 64 + wmp_move_gen_get_lowest_set_bit(touched);
      touched &= touched - 1;
      Anchor *anchor = &wgen->anchors[slot_idx];
      anchor->highest_possible_equity += gen_wmp_anchor_pat_bound(
          gen, gen->dir, gen->current_row_index, anchor);
    }
  }
}

void shadow_play_for_anchor(MoveGen *gen, int col) {
  // Shadow playing is designed to find the best plays first. When we find plays
  // for endgame using MOVE_RECORD_ALL_SMALL, we need to find all of the plays,
  // and because they are ranked for search in the endgame code rather than
  // here, they're returned unordered.
  //
  // It would be better not to even use these Anchor structs in the first place
  // for MOVE_RECORD_ALL_SMALL (or MOVE_RECORD_ALL and instead to just add moves
  // while looping over the board), but we'll put that off until after other
  // MoveGen changes land.
  if (gen->move_record_type == MOVE_RECORD_ALL_SMALL) {
    anchor_heap_add_unheaped_anchor(&gen->anchor_heap, gen->current_row_index,
                                    col, gen->last_anchor_col, gen->dir,
                                    EQUITY_MAX_VALUE, EQUITY_MAX_VALUE);
    return;
  }

  gen->shadow_position_row = NULL;
  gen->shadow_position_col = 0;
  gen->shadow_position_length = 0;

  // Set cols
  gen->current_left_col = col;
  gen->current_right_col = col;

  // Set leftx/rightx
  gen->anchor_left_extension_set = gen_cache_get_left_extension_set(gen, col);
  gen->anchor_right_extension_set = gen_cache_get_right_extension_set(gen, col);

  // Reset unrestricted multipliers
  gen->num_unrestricted_multipliers = 0;
  memset(gen->descending_effective_letter_multipliers, 0,
         sizeof(gen->descending_effective_letter_multipliers));
  gen->last_word_multiplier = 1;

  // Reset available tile scores
  memcpy(gen->descending_tile_scores, gen->full_rack_descending_tile_scores,
         sizeof(gen->descending_tile_scores));
  // Reset score totals
  gen->shadow_mainword_restricted_score = 0;
  gen->shadow_perpendicular_additional_score = 0;
  gen->shadow_word_multiplier = 1;

  // Reset shadow score
  gen->highest_shadow_equity = 0;

  // Set the number of letters
  gen->number_of_letters_on_rack = rack_get_total_letters(&gen->player_rack);

  // Set the current anchor column
  gen->current_anchor_col = col;

  // Reset tiles played
  gen->tiles_played = 0;
  gen->max_tiles_to_play = 0;
  wmp_move_gen_reset_playthrough(&gen->wmp_move_gen);
  wmp_move_gen_reset_anchors(&gen->wmp_move_gen);

  shadow_start(gen);
  if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    if (gen->move_sort_type == MOVE_SORT_EQUITY && gen_pat_is_active(gen)) {
      gen_add_pat_bounds_to_wmp_anchors(gen);
    }
    // A one-square perpendicular shadow may touch no slot here (its
    // playthrough anchor is emitted in the opposite orientation); walking an
    // empty touched mask is already a no-op.
    wmp_move_gen_add_anchors(&gen->wmp_move_gen, gen->current_row_index, col,
                             gen->last_anchor_col, gen->dir,
                             gen->target_equity_cutoff, &gen->anchor_heap);
    return;
  }
  if (gen->max_tiles_to_play == 0) {
    return;
  }
  anchor_heap_add_unheaped_anchor(
      &gen->anchor_heap, gen->current_row_index, col, gen->last_anchor_col,
      gen->dir, gen->highest_shadow_equity, gen->highest_shadow_score);
}

// Simplified shadow_play_for_anchor for small move types (BEST_SMALL).
// Skips WMP operations.
void shadow_play_for_anchor_small(MoveGen *gen, int col) {
  gen->current_left_col = col;
  gen->current_right_col = col;

  gen->anchor_left_extension_set = gen_cache_get_left_extension_set(gen, col);
  gen->anchor_right_extension_set = gen_cache_get_right_extension_set(gen, col);

  gen->num_unrestricted_multipliers = 0;
  memset(gen->descending_effective_letter_multipliers, 0,
         sizeof(gen->descending_effective_letter_multipliers));
  gen->last_word_multiplier = 1;

  memcpy(gen->descending_tile_scores, gen->full_rack_descending_tile_scores,
         sizeof(gen->descending_tile_scores));
  gen->shadow_mainword_restricted_score = 0;
  gen->shadow_perpendicular_additional_score = 0;
  gen->shadow_word_multiplier = 1;

  gen->highest_shadow_score = 0;

  gen->number_of_letters_on_rack = rack_get_total_letters(&gen->player_rack);

  gen->current_anchor_col = col;

  gen->tiles_played = 0;
  gen->max_tiles_to_play = 0;

  shadow_start_small(gen);
  if (gen->max_tiles_to_play == 0) {
    return;
  }

  anchor_heap_add_unheaped_anchor(
      &gen->anchor_heap, gen->current_row_index, col, gen->last_anchor_col,
      gen->dir, gen->highest_shadow_score, gen->highest_shadow_score);
}

void shadow_by_orientation(MoveGen *gen) {
  for (int row = 0; row < BOARD_DIM; row++) {
    gen->current_row_index = row;
    if (gen->row_number_of_anchors_cache[BOARD_DIM * gen->dir + row] == 0) {
      continue;
    }
    gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
    gen->row_squares =
        board_get_row_cache(gen->board_lanes, gen->current_row_index, gen->dir);
    gen->wit_row_lane = board_get_wit_row_lane(
        gen->board, gen->current_row_index, gen->dir, gen->cross_index);
    gen->wit_len_lane = board_get_wit_len_lane(
        gen->board, gen->current_row_index, gen->dir, gen->cross_index);
    gen->pat_lane_penalty_bound = pat_eval_lane_penalty_bound(
        &gen->pat_eval_ctx, gen->dir, gen->current_row_index);
    for (int col = 0; col < BOARD_DIM; col++) {
      if (gen_cache_get_is_anchor(gen, col)) {
        shadow_play_for_anchor(gen, col);

        gen->last_anchor_col = col;
        // The next anchor to search after a playthrough tile should
        // leave a gap of one square so that it will not search backwards
        // into the square adjacent to the playthrough tile.
        if (!gen_cache_is_empty(gen, col)) {
          gen->last_anchor_col++;
        }
      }
    }
  }
}

// Shadows every anchor of lane `row` in the current gen->dir for BEST_SMALL.
static inline __attribute__((always_inline)) void
shadow_lane_small(MoveGen *gen, int row) {
  gen->current_row_index = row;
  if (gen->row_number_of_anchors_cache[BOARD_DIM * gen->dir + row] == 0) {
    return;
  }
  gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
  gen->row_squares =
      board_get_row_cache(gen->board_lanes, gen->current_row_index, gen->dir);
  gen->wit_row_lane = board_get_wit_row_lane(gen->board, gen->current_row_index,
                                             gen->dir, gen->cross_index);
  gen->wit_len_lane = board_get_wit_len_lane(gen->board, gen->current_row_index,
                                             gen->dir, gen->cross_index);
  for (int col = 0; col < BOARD_DIM; col++) {
    if (gen_cache_get_is_anchor(gen, col)) {
      shadow_play_for_anchor_small(gen, col);

      gen->last_anchor_col = col;
      if (!gen_cache_is_empty(gen, col)) {
        gen->last_anchor_col++;
      }
    }
  }
}

void shadow_by_orientation_small(MoveGen *gen) {
  for (int row = 0; row < BOARD_DIM; row++) {
    shadow_lane_small(gen, row);
  }
}

static inline void set_descending_tile_scores(MoveGen *gen) {
  int i = 0;
  const int ld_size = ld_get_size(&gen->ld);
  for (int j = 0; j < ld_size; j++) {
    const MachineLetter j_score_order = ld_get_score_order(&gen->ld, j);
    for (int k = 0; k < rack_get_letter(&gen->player_rack, j_score_order);
         k++) {
      gen->descending_tile_scores[i] = gen->tile_scores[j_score_order];
      i++;
    }
  }
  memcpy(gen->full_rack_descending_tile_scores, gen->descending_tile_scores,
         sizeof(gen->descending_tile_scores));
}

void gen_load_position(MoveGen *gen, const MoveGenArgs *args) {
  const Game *game = args->game;
  gen->move_record_type = args->move_record_type;
  gen->move_sort_type = args->move_sort_type;
  MoveList *move_list = args->move_list;
  const KWG *override_kwg = args->override_kwg;
  gen->eq_margin_movegen = args->eq_margin_movegen;
  gen->target_equity_cutoff = args->target_equity;
  gen->best_small_out_bonus = args->best_small_out_bonus;
  gen->best_small_canonical_ties = args->best_small_canonical_ties;

  gen->board = game_get_board(game);
  // The endgame defers moves' anchor and cross-set updates; they must be
  // applied before generating on the board.
  assert(board_get_cross_sets_valid(gen->board));
  gen->player_index = game_get_player_on_turn_index(game);
  const Player *player = game_get_player(game, gen->player_index);
  const Player *opponent = game_get_player(game, 1 - gen->player_index);
  gen->best_small_out_rack_size =
      rack_get_total_letters(player_get_rack(player));

  // gen->ld is a by-value copy and the per-thread MoveGen is reused across
  // every node of a solve with a stable game->ld, so skip the ~3.4 KB memcpy
  // (and the bit_rack compat scan) when the source distribution is unchanged --
  // pure per-node-setup work removed on the movegen-dominated endgame profile.
  // The guard is the ld's content fingerprint, NOT its address: the MoveGen
  // cache outlives the Config that owns an ld, so a freed ld (and its name
  // string) can be reused by a different distribution at the same address
  // (ABA), which a pointer comparison misses but the content hash catches. The
  // cached copy's fingerprint is 0 until the first load (calloc), and a real
  // fingerprint is never 0, so the first node always loads.
  const LetterDistribution *ld_src = game_get_ld(game);
  if (ld_get_content_fingerprint(ld_src) != gen->ld.content_fingerprint) {
    memcpy(&gen->ld, ld_src, sizeof(LetterDistribution));
    gen->bit_rack_compatible = bit_rack_is_compatible_with_ld(&gen->ld);
  }
  gen->kwg = (override_kwg == NULL) ? player_get_kwg(player) : override_kwg;
  const KLV *new_klv = player_get_klv(player);
  const uint64_t new_klv_mutation_counter =
      (new_klv != NULL) ? klv_get_mutation_counter(new_klv) : 0;
  const uint64_t new_klv_instance_fp =
      (new_klv != NULL) ? klv_get_instance_fingerprint(new_klv) : 0;
  // Invalidate when the KLV pointer changes (player swap, lexicon change), the
  // KLV instance fingerprint changes (catches a reloaded KLV the allocator
  // placed at the freed KLV's address -- ABA -- which the pointer comparison
  // misses because the move_gen cache outlives the Config that owned the old
  // KLV), OR the same KLV's leave_values have been mutated in place. The
  // mutation-counter path catches both leavegen's bulk updates and test-only
  // set_klv_leave_value calls between generate_moves invocations, which would
  // otherwise leave stale leave_values cached in the subrack cache and
  // anchor-cache upper-bound entries.
  const bool klv_changed =
      (new_klv != gen->klv) ||
      (new_klv_instance_fp != gen->klv_instance_fp_at_load) ||
      (new_klv_mutation_counter != gen->klv_mutation_counter_at_load);
  if (klv_changed) {
    for (int i = 0; i < MOVEGEN_SUBRACK_CACHE_SIZE; i++) {
      gen->subrack_cache[i].valid = false;
    }
    for (int i = 0; i < MOVEGEN_KLV_LEAVES_CACHE_SIZE; i++) {
      gen->klv_leaves_cache[i].valid = false;
    }
  }
  gen->klv = new_klv;
  gen->klv_mutation_counter_at_load = new_klv_mutation_counter;
  gen->klv_instance_fp_at_load = new_klv_instance_fp;
  // The RIT keys on a BitRack, so it is unusable for a BitRack-incompatible
  // alphabet; treat it as absent (the WMP is gated the same way below).
  const RackInfoTable *new_rit =
      gen->bit_rack_compatible ? player_get_rack_info_table(player) : NULL;
  const uint64_t new_rit_instance_id =
      (new_rit != NULL) ? rack_info_table_get_instance_id(new_rit) : 0;
  if (new_rit != gen->rack_info_table ||
      new_rit_instance_id != gen->rit_instance_id_at_load) {
    memset(gen->rit_cache_valid, 0, sizeof(gen->rit_cache_valid));
    // Leave values are stored in the subrack cache; they depend on the
    // RIT (via the leave_map), so invalidate when the RIT changes.
    for (int i = 0; i < MOVEGEN_SUBRACK_CACHE_SIZE; i++) {
      gen->subrack_cache[i].valid = false;
    }
  }
  gen->rack_info_table = new_rit;
  gen->rit_instance_id_at_load = new_rit_instance_id;
  // The word info table describes how letters sit in words, which wordsmog
  // ignores.
  gen->word_info_table = game_get_variant(game) == GAME_VARIANT_WORDSMOG
                             ? NULL
                             : player_get_word_info_table(player);
  gen->board_number_of_tiles_played = board_get_tiles_played(gen->board);
  rack_copy(&gen->opponent_rack, player_get_rack(opponent));
  rack_copy(&gen->player_rack, player_get_rack(player));
  move_list_set_rack(move_list, &gen->player_rack);
  rack_set_dist_size(&gen->leave, ld_get_size(&gen->ld));
  const WMP *previous_wmp = gen->wmp_move_gen.wmp;
  // Decide up front whether WMP will be active. It is disabled for
  // ALL_SMALL/TILES_PLAYED (unsupported), when override_kwg is set (WMP data
  // corresponds to the original KWG, not the override), and on a
  // BitRack-incompatible alphabet (the WMP keys on a BitRack). Passing NULL
  // when disabled lets wmp_move_gen_init early-return instead of building a
  // BitRack, clearing the length table, and seeding all anchor slots -- wasted
  // work on every endgame node, which always passes override_kwg. The seeded
  // fields are only read while WMP is active, so skipping them when it is NULL
  // is safe.
  const bool wmp_disabled = gen->move_record_type == MOVE_RECORD_ALL_SMALL ||
                            gen->move_record_type == MOVE_RECORD_TILES_PLAYED ||
                            override_kwg != NULL || !gen->bit_rack_compatible;
  wmp_move_gen_init(&gen->wmp_move_gen, &gen->ld, &gen->player_rack,
                    wmp_disabled ? NULL : player_get_wmp(player));
  // The subrack cache holds wmp_entry pointers derived from the WMP; a WMP
  // swap (different lexicon) makes those stale -- and "stale" means dangling,
  // since the old WMP's Config may have been freed. Invalidate on a WMP
  // pointer change OR an instance-fingerprint change. The fingerprint catches
  // a reloaded WMP (even of the same lexicon) that the allocator placed at the
  // freed WMP's struct address (ABA) but whose internal maps are at new
  // addresses -- a pointer comparison alone would miss it because the move_gen
  // cache outlives the Config that owned the old WMP.
  const WMP *new_wmp = gen->wmp_move_gen.wmp;
  if (new_wmp != NULL && bit_rack_get_letter(&gen->wmp_move_gen.player_bit_rack,
                                             BLANK_MACHINE_LETTER) != 0) {
    wmp_blank_word_cache_reset(&gen->blank_word_cache);
  }
  const uint64_t new_wmp_instance_fp =
      (new_wmp != NULL) ? wmp_get_instance_fingerprint(new_wmp) : 0;
  if (new_wmp != previous_wmp ||
      new_wmp_instance_fp != gen->wmp_instance_fp_at_load) {
    for (int i = 0; i < MOVEGEN_SUBRACK_CACHE_SIZE; i++) {
      gen->subrack_cache[i].valid = false;
    }
  }
  gen->wmp_instance_fp_at_load = new_wmp_instance_fp;

  gen->bingo_bonus = game_get_bingo_bonus(game);
  gen->number_of_tiles_in_bag = bag_get_letters(game_get_bag(game));
  gen->wmp_prune_subracks_by_leave = (gen->number_of_tiles_in_bag > 0) &&
                                     (gen->move_sort_type == MOVE_SORT_EQUITY);
  gen->kwgs_are_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
  gen->move_list = move_list;
  gen->cross_index =
      board_get_cross_set_index(gen->kwgs_are_shared, gen->player_index);

  // Reset the move list
  if (gen->move_record_type == MOVE_RECORD_ALL_SMALL ||
      gen->move_record_type == MOVE_RECORD_TILES_PLAYED ||
      gen->move_record_type == MOVE_RECORD_BEST_SMALL) {
    if (gen->move_record_type == MOVE_RECORD_BEST_SMALL &&
        gen->move_sort_type != MOVE_SORT_SCORE) {
      log_fatal("MOVE_RECORD_BEST_SMALL only supports MOVE_SORT_SCORE");
    }
    small_move_list_reset(gen->move_list);
  } else {
    move_list_reset(gen->move_list);
  }

  // Reset the best and current moves
  gen->best_move_index = 0;
  move_set_equity(gen_get_best_move(gen), EQUITY_INITIAL_VALUE);
  gen->best_move_equity_or_score = EQUITY_INITIAL_VALUE;
  gen->cutoff_equity_or_score = EQUITY_INITIAL_VALUE;

  // Set rack cross set and cache ld's tile scores
  gen->rack_cross_set = 0;
  memset(gen->tile_scores, 0, sizeof(gen->tile_scores));
  for (int i = 0; i < ld_get_size(&gen->ld); i++) {
    if (rack_get_letter(&gen->player_rack, i) > 0) {
      gen->rack_cross_set = gen->rack_cross_set | ((uint64_t)1 << i);
    }
    gen->tile_scores[i] = ld_get_score(&gen->ld, i);
    gen->tile_scores[get_blanked_machine_letter(i)] =
        ld_get_score(&gen->ld, BLANK_MACHINE_LETTER);
  }

  set_descending_tile_scores(gen);

  board_load_number_of_row_anchors_cache(gen->board,
                                         gen->row_number_of_anchors_cache);
  gen->board_lanes = board_get_readonly_lanes(gen->board, gen->cross_index);

  // opening_move_word/letter_penalties is read only by gen_get_static_equity
  // (the equity-recording paths). The endgame's small-record movegen types
  // never read it, so skip the per-node 240-byte copy for them.
  if (gen->move_record_type != MOVE_RECORD_ALL_SMALL &&
      gen->move_record_type != MOVE_RECORD_TILES_PLAYED &&
      gen->move_record_type != MOVE_RECORD_BEST_SMALL) {
    board_copy_opening_penalties(gen->board, gen->opening_move_word_penalties,
                                 gen->opening_move_letter_penalties);
    // The PAT term is equity-only, bag-gated like the leave value,
    // and requires valid cross sets (its scans read them). The weights are
    // re-read from the player on every position load, so a training loop
    // that rewrites them between generations needs no extra invalidation,
    // and so are the player's PAT settings (see player_set_pat_usage).
    // In wordsmog the weights are prepared in anagram mode (see
    // PATWeights.anagram), and the WMP, when the board's cross sets came
    // from it, resolves the hooks a move creates.
    const PATWeights *pat = player_get_pat(player);
    if (pat && !args->disable_pat && !player_get_pat_disabled(player) &&
        gen->move_sort_type == MOVE_SORT_EQUITY &&
        gen->number_of_tiles_in_bag > 0 &&
        board_get_cross_sets_valid(gen->board)) {
      pat_eval_context_load(&gen->pat_eval_ctx, pat, gen->board_lanes, &gen->ld,
                            &gen->player_rack,
                            PAT_CLASS_MASK_ALL &
                                ~args->pat_disabled_classes_mask &
                                ~player_get_pat_disabled_classes_mask(player),
                            rack_get_total_letters(&gen->opponent_rack));
      pat_eval_context_set_kwg(&gen->pat_eval_ctx, gen->kwg);
      pat_eval_context_set_wmp(&gen->pat_eval_ctx, player_get_wmp(player));
      pat_eval_context_set_utility(
          &gen->pat_eval_ctx,
          equity_to_int(player_get_score(player) - player_get_score(opponent)),
          gen->number_of_tiles_in_bag);
    } else {
      pat_eval_context_disable(&gen->pat_eval_ctx);
    }
  } else {
    pat_eval_context_disable(&gen->pat_eval_ctx);
  }

  gen->is_wordsmog = game_get_variant(game) == GAME_VARIANT_WORDSMOG;
  const int wordsmog_pat_evals = player_get_wordsmog_pat_evals(player);
  gen->wordsmog_pat_evals = args->wordsmog_exact_pat || wordsmog_pat_evals == 0
                                ? INT_MAX
                                : wordsmog_pat_evals;
  if (gen->wordsmog_pat != NULL) {
    wordsmog_pat_refine_reset(gen->wordsmog_pat);
  }
  gen->threshold_exceeded = false;
  gen->stop_on_threshold = args->target_equity != EQUITY_MAX_VALUE;
  // A word info table's prune and wordsmog's letters are shadow inputs that
  // the cache's keys do not hold.
  gen->lane_shadow_cache = gen->move_record_type == MOVE_RECORD_BEST_SMALL &&
                                   gen->word_info_table == NULL &&
                                   !gen->is_wordsmog
                               ? args->lane_shadow_cache
                               : NULL;
}

void gen_look_up_leaves_and_record_exchanges(MoveGen *gen) {
  leave_map_init(&gen->player_rack, &gen->leave_map);
  if (rack_get_total_letters(&gen->player_rack) < RACK_SIZE) {
    leave_map_set_current_value(
        &gen->leave_map, klv_get_leave_value(gen->klv, &gen->player_rack));
  } else {
    leave_map_set_current_value(&gen->leave_map, EQUITY_INITIAL_VALUE);
  }

  for (int leave_size_idx = 0; leave_size_idx < (RACK_SIZE + 1);
       leave_size_idx++) {
    gen->best_leaves[leave_size_idx] = EQUITY_INITIAL_VALUE;
  }

  const bool check_leaves = (gen->number_of_tiles_in_bag > 0) &&
                            (gen->move_sort_type != MOVE_SORT_SCORE);

  // Assumes the player has drawn a full rack but not the opponent.
  const bool add_exchange = gen->number_of_tiles_in_bag +
                                rack_get_total_letters(&gen->opponent_rack) >=
                            (RACK_SIZE * 2);

  // Try to use the pre-computed rack info table for full racks.
  const bool has_full_rack =
      rack_get_total_letters(&gen->player_rack) == RACK_SIZE;
  const RackInfoTable *rit = gen->rack_info_table;
  gen->rit_entry = NULL;

  // rit is NULL for BitRack-incompatible alphabets (gated in
  // gen_load_position), so this RIT-cache path is already skipped for them.
  if (has_full_rack && rit != NULL) {
    const BitRack player_bit_rack =
        bit_rack_create_from_rack(&gen->ld, &gen->player_rack);
    // Check per-thread direct-mapped cache before the full hash lookup.
    const uint32_t cache_idx =
        bit_rack_get_bucket_index(&player_bit_rack, MOVEGEN_RIT_CACHE_SIZE);
    if (gen->rit_cache_valid[cache_idx] &&
        bit_rack_equals(&player_bit_rack, &gen->rit_cache_keys[cache_idx])) {
      gen->rit_entry = gen->rit_cache_entries[cache_idx];
    } else {
      const RackInfoTableEntry *entry =
          rack_info_table_lookup(rit, &player_bit_rack);
      gen->rit_cache_keys[cache_idx] = player_bit_rack;
      gen->rit_cache_entries[cache_idx] = entry;
      gen->rit_cache_valid[cache_idx] = true;
      if (entry != NULL) {
        gen->rit_entry = entry;
      }
    }
  }

  // gen->leave_map.current_index moves differently when filling leave_values
  // than when reading from it to generate plays. Start at 0, which represents
  // using (exchanging) gen->player_rack->number_of_letters tiles and keeping
  // 0 tiles.
  leave_map_set_current_index(&gen->leave_map, 0);
  rack_reset(&gen->leave);

  if (gen->rit_entry != NULL) {
    // Fast path: unpack the RIT's 24-bit packed leave values to 32-bit
    // in the leave map so play-time leave_map_get_current_value calls
    // and wmp_move_gen's subrack reads return the right value.
    rack_info_table_entry_unpack_leaves(gen->rit_entry,
                                        gen->leave_map.leave_values);
    // Load best_leaves directly from the RIT entry, avoiding the
    // 2^RACK_SIZE canonical subset walk. Only fall back to the walk
    // when we actually need to record exchange moves.
    const Equity *rit_best_leaves =
        rack_info_table_entry_get_best_leaves(gen->rit_entry);
    memcpy(gen->best_leaves, rit_best_leaves,
           RACK_INFO_TABLE_BEST_LEAVES_PER_ENTRY * sizeof(Equity));
    if (add_exchange) {
      if (gen->move_record_type == MOVE_RECORD_BEST) {
        // For MOVE_RECORD_BEST, record the single precomputed best
        // exchange from the RIT — O(1) instead of 2^RACK_SIZE walk.
        record_best_exchange_from_table(gen);
      } else {
        generate_exchange_moves_from_table(gen, &gen->leave, 0, true);
      }
    }
  } else if (check_leaves || add_exchange) {
    // Mini-RIT fast path: when the leave_values and best_leaves for this
    // rack have been computed by a previous KLV walk on this thread,
    // memcpy them in and only walk the KLV tree again when add_exchange
    // requires recording the actual exchange moves (which goes through
    // the RIT-alike generate_exchange_moves_from_table, skipping the
    // KLV descent either way).
    // Unlike the RIT/WMP caches (gated by being NULL), this KLV-leaves cache is
    // always available, so it must consult bit_rack_compatible directly: its
    // key is a BitRack, unrepresentable for a too-large alphabet.
    const bool kle_eligible = has_full_rack && gen->bit_rack_compatible;
    const BitRack kle_bit_rack =
        kle_eligible ? bit_rack_create_from_rack(&gen->ld, &gen->player_rack)
                     : bit_rack_create_empty();
    const uint32_t kle_slot =
        kle_eligible ? bit_rack_get_bucket_index(&kle_bit_rack,
                                                 MOVEGEN_KLV_LEAVES_CACHE_SIZE)
                     : 0;
    KlvLeavesCacheEntry *kle =
        kle_eligible ? &gen->klv_leaves_cache[kle_slot] : NULL;
    if (kle_eligible && kle->valid &&
        bit_rack_equals(&kle_bit_rack, &kle->key)) {
      memcpy(gen->leave_map.leave_values, kle->leave_values,
             sizeof(kle->leave_values));
      memcpy(gen->best_leaves, kle->best_leaves, sizeof(kle->best_leaves));
      if (add_exchange) {
        // Leaves are already in the leave map, so the RIT-style variant
        // (no KLV descent) can record exchanges straight from memory.
        generate_exchange_moves_from_table(gen, &gen->leave, 0, true);
      }
    } else {
      const uint32_t node_index = kwg_get_dawg_root_node_index(gen->klv->kwg);
      generate_exchange_moves(gen, &gen->leave, node_index, 0, 0, add_exchange);
      if (kle_eligible) {
        kle->key = kle_bit_rack;
        memcpy(kle->leave_values, gen->leave_map.leave_values,
               sizeof(kle->leave_values));
        memcpy(kle->best_leaves, gen->best_leaves, sizeof(kle->best_leaves));
        kle->valid = true;
      }
    }
  }

  if (!check_leaves) {
    for (int leave_size_idx = 0; leave_size_idx < RACK_SIZE + 1;
         leave_size_idx++) {
      gen->best_leaves[leave_size_idx] = 0;
    }
  }
}

void gen_shadow(MoveGen *gen) {
  // Set the leave_map index to 2^number_of_letters - 1, which represents
  // using (playing) zero tiles and keeping
  // gen->player_rack->number_of_letters tiles.
  leave_map_set_current_index(
      &gen->leave_map, (1 << rack_get_total_letters(&gen->player_rack)) - 1);
  anchor_heap_reset(&gen->anchor_heap);

  for (int dir = 0; dir < 2; dir++) {
    gen->dir = dir;
    shadow_by_orientation(gen);
  }

  // Also unnecessary for MOVE_RECORD_ALL, but our tests might care about the
  // ordering of output.
  if (gen->move_record_type != MOVE_RECORD_ALL_SMALL) {
    anchor_heapify_all(&gen->anchor_heap);
  } else {
    // The anchors are still extracted one by one, and each extraction's sift
    // reads the sentinels.
    anchor_heap_set_sentinels(&gen->anchor_heap);
  }
}

// MurmurHash3's 64-bit finalizer.
static inline uint64_t lane_shadow_mix(uint64_t value) {
  value ^= value >> 33;
  value *= UINT64_C(0xFF51AFD7ED558CCD);
  value ^= value >> 33;
  value *= UINT64_C(0xC4CEB9FE1A85EC53);
  value ^= value >> 33;
  return value;
}

// The key of a lane's squares as the small shadow reads them, with
// context_key: each square's cross set, and the word holding its cross
// score, letter, bonus square and flags. The extension sets follow from the
// letters and the lexicon, which context_key holds. The two words go in
// separate chains of multiply-adds; each step is a bijection of its chain so
// far (the multiplier is odd), so lanes that differ in one word always differ
// in a chain. One finalizer then mixes both chains and context_key into the
// top bits that index the cache. A key is never 0, which marks no lane.
static inline uint64_t lane_shadow_key(const Square *lane_squares,
                                       uint64_t context_key) {
  uint64_t cross_sets = UINT64_C(0x9E3779B97F4A7C15);
  uint64_t fields = UINT64_C(0xC2B2AE3D27D4EB4F);
  for (int col = 0; col < BOARD_DIM; col++) {
    uint64_t field_word;
    memcpy(&field_word,
           (const char *)&lane_squares[col] + offsetof(Square, cross_score),
           sizeof(field_word));
    cross_sets = cross_sets * LANE_HASH_MULTIPLIER +
                 square_get_cross_set(&lane_squares[col]);
    fields = fields * LANE_HASH_MULTIPLIER + field_word;
  }
  const uint64_t rotated_fields = (fields << 32) | (fields >> 32);
  return lane_shadow_mix(cross_sets ^ rotated_fields ^ context_key) | 1;
}

static_assert(offsetof(Square, cross_score) + sizeof(uint64_t) <=
                  sizeof(Square),
              "lane_shadow_key reads a word from cross_score on");

// The key of what a lane's small shadow reads besides the lane's squares and
// direction: the solve (by the cache's salt, which also stands for the letter
// distribution and the bingo bonus), the lexicon and cross set index, the
// rack, and the out bonus and the rack size it applies to.
static uint64_t lane_shadow_context_key(const MoveGen *gen) {
  uint64_t key = gen->lane_shadow_cache->salt;
  key = key * LANE_HASH_MULTIPLIER + (uint64_t)(uintptr_t)gen->kwg;
  key = key * LANE_HASH_MULTIPLIER + (uint64_t)gen->cross_index;
  for (int ml = 0; ml < ld_get_size(&gen->ld); ml++) {
    key = key * LANE_HASH_MULTIPLIER +
          (uint64_t)rack_get_letter(&gen->player_rack, ml);
  }
  key = key * LANE_HASH_MULTIPLIER +
        (uint64_t)(uint32_t)gen->best_small_out_bonus;
  key = key * LANE_HASH_MULTIPLIER + (uint64_t)gen->best_small_out_rack_size;
  return lane_shadow_mix(key);
}

// Anchor idx of entry, in lane row of direction dir. Its other fields are
// zero, which the small generator never reads.
static inline Anchor lane_shadow_entry_anchor(const LaneShadowEntry *entry,
                                              int idx, int row, int dir) {
  return (Anchor){.highest_possible_equity = entry->bounds[idx],
                  .highest_possible_score = entry->bounds[idx],
                  .row = (unsigned)row,
                  .col = entry->cols[idx],
                  .last_anchor_col = entry->last_anchor_cols[idx],
                  .dir = (unsigned)dir};
}

#ifndef NDEBUG
// Asserts that shadowing the lane adds the anchors entry holds, so that a
// shadow input the key misses fails the tests. The anchors are added past
// the heap's end and then dropped.
static void lane_shadow_assert_entry_is_shadow(MoveGen *gen,
                                               const LaneShadowEntry *entry,
                                               int row, int dir) {
  const int first_anchor = gen->anchor_heap.count;
  gen->dir = dir;
  shadow_lane_small(gen, row);
  assert(gen->anchor_heap.count - first_anchor == entry->count);
  for (int idx = 0; idx < entry->count; idx++) {
    const Anchor *anchor = &gen->anchor_heap.anchors[first_anchor + idx];
    assert(anchor->col == entry->cols[idx]);
    assert(anchor->last_anchor_col == entry->last_anchor_cols[idx]);
    assert(anchor->highest_possible_equity == entry->bounds[idx]);
  }
  gen->anchor_heap.count = first_anchor;
}
#endif

// Shadows the lanes of lane_mask with anchors, as shadow_lane_small does,
// through gen->lane_shadow_cache: a lane whose key the cache holds adds the
// anchors recorded under it, in the order shadowing adds them, and every
// other lane is shadowed, and recorded if it adds few enough anchors.
static void shadow_lanes_small_cached(MoveGen *gen, uint64_t lane_mask) {
  LaneShadowCache *cache = gen->lane_shadow_cache;
  // A play of one tile is shadowed only across, so a row and a column with
  // the same squares can have different shadows: a column's keys differ by a
  // constant, which the key's finalizer spreads to every bit.
  const uint64_t row_context_key = lane_shadow_context_key(gen);
  const uint64_t context_keys[2] = {
      row_context_key, row_context_key ^ UINT64_C(0x9E3779B97F4A7C15)};
  uint64_t lanes = 0;
  for (int lane = 0; lane < 2 * BOARD_DIM; lane++) {
    lanes |= (uint64_t)(gen->row_number_of_anchors_cache[lane] != 0) << lane;
  }
  lanes &= lane_mask;
  // Every lane's key first, so that the loads of their entries overlap.
  uint64_t keys[2 * BOARD_DIM];
  for (uint64_t remaining = lanes; remaining != 0; remaining &= remaining - 1) {
    const int lane = lowest_set_bit_index_64(remaining);
    keys[lane] =
        lane_shadow_key(board_get_row_cache(gen->board_lanes, lane % BOARD_DIM,
                                            lane / BOARD_DIM),
                        context_keys[lane / BOARD_DIM]);
    __builtin_prefetch(lane_shadow_cache_entry(cache, keys[lane]));
  }
  for (uint64_t remaining = lanes; remaining != 0; remaining &= remaining - 1) {
    const int lane = lowest_set_bit_index_64(remaining);
    const int row = lane % BOARD_DIM;
    const int dir = lane / BOARD_DIM;
    LaneShadowEntry *entry = lane_shadow_cache_entry(cache, keys[lane]);
    if (entry->key == keys[lane]) {
#ifndef NDEBUG
      lane_shadow_assert_entry_is_shadow(gen, entry, row, dir);
#endif
      // The first LANE_SHADOW_UNCONDITIONAL_ANCHORS anchors are written
      // whether or not the entry holds them, so that most lanes copy without
      // a loop branch to mispredict; the count then takes only those it
      // holds, and the rest are scratch past the heap's end.
      AnchorHeap *heap = &gen->anchor_heap;
      assert(heap->count + LANE_SHADOW_MAX_ANCHORS <= ANCHOR_HEAP_CAPACITY);
      Anchor *anchors = &heap->anchors[heap->count];
      for (int idx = 0; idx < LANE_SHADOW_UNCONDITIONAL_ANCHORS; idx++) {
        anchors[idx] = lane_shadow_entry_anchor(entry, idx, row, dir);
      }
      for (int idx = LANE_SHADOW_UNCONDITIONAL_ANCHORS; idx < entry->count;
           idx++) {
        anchors[idx] = lane_shadow_entry_anchor(entry, idx, row, dir);
      }
      heap->count += entry->count;
      continue;
    }
    gen->dir = dir;
    const int first_anchor = gen->anchor_heap.count;
    shadow_lane_small(gen, row);
    const int count = gen->anchor_heap.count - first_anchor;
    if (count > LANE_SHADOW_MAX_ANCHORS) {
      continue;
    }
    entry->key = keys[lane];
    entry->count = (uint8_t)count;
    for (int idx = 0; idx < count; idx++) {
      const Anchor *anchor = &gen->anchor_heap.anchors[first_anchor + idx];
      // shadow_play_for_anchor_small bounds the equity and the score alike.
      assert(anchor->highest_possible_score == anchor->highest_possible_equity);
      entry->cols[idx] = (uint8_t)anchor->col;
      entry->last_anchor_cols[idx] = (uint8_t)anchor->last_anchor_col;
      entry->bounds[idx] = anchor->highest_possible_equity;
    }
  }
}

// Simplified gen_shadow for BEST_SMALL.
// Skips leave_map setup and uses small shadow functions.
void gen_shadow_small(MoveGen *gen) {
  // Every anchor restores this same rack; snapshot it once per generation.
  rack_copy(&gen->full_player_rack, &gen->player_rack);
  anchor_heap_reset(&gen->anchor_heap);

  if (gen->lane_shadow_cache != NULL) {
    shadow_lanes_small_cached(gen, (UINT64_C(1) << (2 * BOARD_DIM)) - 1);
  } else {
    for (int dir = 0; dir < 2; dir++) {
      gen->dir = dir;
      shadow_by_orientation_small(gen);
    }
  }

  anchor_heapify_all(&gen->anchor_heap);
}

// Points the generator at lane `row` of the current gen->dir.
static inline __attribute__((always_inline)) void gen_load_lane(MoveGen *gen,
                                                                int row) {
  const int dir = gen->dir;
  gen->current_row_index = row;
  gen->row_squares = board_get_row_cache(gen->board_lanes, row, dir);
  gen->wit_row_lane =
      board_get_wit_row_lane(gen->board, row, dir, gen->cross_index);
  gen->wit_len_lane =
      board_get_wit_len_lane(gen->board, row, dir, gen->cross_index);
}

// Every play anchored in lane `row` of the current gen->dir, scanning the
// anchors in board order.
static inline __attribute__((always_inline)) void
gen_record_lane_plays_unordered(MoveGen *gen, int row,
                                uint32_t kwg_root_node_index,
                                bool record_small) {
  gen_load_lane(gen, row);

  int last_anchor_col = INITIAL_LAST_ANCHOR_COL;
  for (int col = 0; col < BOARD_DIM; col++) {
    if (gen_cache_get_is_anchor(gen, col)) {
      gen->current_anchor_col = col;
      gen->last_anchor_col = last_anchor_col;
      gen->anchor_right_extension_set =
          gen_cache_get_right_extension_set(gen, col);
      gen->current_anchor_highest_possible_score = EQUITY_MAX_VALUE;

      if (record_small) {
        recursive_gen_small(gen, col, kwg_root_node_index, col, col,
                            gen->dir == BOARD_HORIZONTAL_DIRECTION, 0, 1, 0);
      } else {
        recursive_gen(gen, col, kwg_root_node_index, col, col,
                      gen->dir == BOARD_HORIZONTAL_DIRECTION, 0, 1, 0);
      }
      last_anchor_col = col;
      if (!gen_cache_is_empty(gen, col)) {
        last_anchor_col++;
      }
    }
  }
}

static inline __attribute__((always_inline)) void
gen_record_scoring_plays_unordered(MoveGen *gen, bool record_small) {
  gen->tiles_played = 0;
  const uint32_t kwg_root_node_index = kwg_get_root_node_index(gen->kwg);

  for (int dir = 0; dir < 2; dir++) {
    gen->dir = dir;
    for (int row = 0; row < BOARD_DIM; row++) {
      if (gen->threshold_exceeded) {
        return;
      }
      if (gen->row_number_of_anchors_cache[BOARD_DIM * dir + row] == 0) {
        continue;
      }
      gen_record_lane_plays_unordered(gen, row, kwg_root_node_index,
                                      record_small);
    }
  }
}

// Points the generator at an anchor's lane and sets the anchor's fields.
static inline __attribute__((always_inline)) void
gen_load_anchor(MoveGen *gen, const Anchor *anchor) {
  gen->current_anchor_col = anchor->col;
  // Don't recopy the row cache if we're working on the same board lane
  // as the previous anchor. When anchors have been sorted by descending
  // max equity, doing this check is a wash, but it helps for endgame when
  // we are just scanning over the board in order.
  if ((gen->current_row_index != anchor->row) || (gen->dir != anchor->dir)) {
    gen->current_row_index = anchor->row;
    gen->dir = anchor->dir;
    gen->row_squares =
        board_get_row_cache(gen->board_lanes, anchor->row, anchor->dir);
    gen->wit_row_lane = board_get_wit_row_lane(gen->board, anchor->row,
                                               anchor->dir, gen->cross_index);
    gen->wit_len_lane = board_get_wit_len_lane(gen->board, anchor->row,
                                               anchor->dir, gen->cross_index);
  }
  gen->last_anchor_col = anchor->last_anchor_col;
  gen->anchor_right_extension_set =
      gen_cache_get_right_extension_set(gen, gen->current_anchor_col);
  gen->current_anchor_highest_possible_score = anchor->highest_possible_score;
}

static inline __attribute__((always_inline)) void
gen_record_scoring_plays_impl(MoveGen *gen, bool lazy) {
  if (gen->threshold_exceeded) {
    return;
  }
  // Reset the reused generator fields
  gen->tiles_played = 0;

  // Set these fields to values outside their valid ranges so the row cache gets
  // loaded for the first anchor.
  gen->current_row_index = -1;
  gen->dir = -1;

  const uint32_t kwg_root_node_index = kwg_get_root_node_index(gen->kwg);
  if (gen->is_wordsmog) {
    rack_reset(&gen->full_player_rack);
  }
  while (gen->anchor_heap.count > 0) {
    if (gen->threshold_exceeded) {
      break;
    }
    const Anchor anchor = anchor_heap_extract_max(&gen->anchor_heap);
    // The heap is ordered by highest_possible_equity, so one anchor whose
    // bound cannot beat the cutoff ends the whole generation. This is only
    // sound while every anchor's bound is a true upper bound on the equity
    // of every move from that anchor; see the invariant comment on
    // static_eval_get_shadow_equity for what that requires of new terms.
    if (better_play_has_been_found(gen, anchor.highest_possible_equity)) {
      break;
    }
    gen_load_anchor(gen, &anchor);
    if (gen->move_record_type == MOVE_RECORD_BEST_SMALL) {
      recursive_gen_small(gen, anchor.col, kwg_root_node_index, anchor.col,
                          anchor.col, gen->dir == BOARD_HORIZONTAL_DIRECTION, 0,
                          1, 0);
    } else if (gen->is_wordsmog) {
      if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
        wordsmog_wmp_gen(gen, &anchor, lazy);
      } else {
        recursive_gen_alpha(gen, anchor.col, anchor.col, anchor.col,
                            gen->dir == BOARD_HORIZONTAL_DIRECTION, 0, 1, 0);
      }
    } else if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
      wordmap_gen(gen, &anchor, lazy);
    } else {
      recursive_gen(gen, anchor.col, kwg_root_node_index, anchor.col,
                    anchor.col, gen->dir == BOARD_HORIZONTAL_DIRECTION, 0, 1,
                    0);
    }

    // If a better play has been found than should have been possible for
    // this anchor, highest_possible_equity was invalid.
    assert(!better_play_has_been_found(gen, anchor.highest_possible_equity));
  }
}

static __attribute__((noinline)) void
gen_record_scoring_plays_eager(MoveGen *gen) {
  gen_record_scoring_plays_impl(gen, false);
}

static inline __attribute__((always_inline)) void
gen_record_scoring_plays(MoveGen *gen) {
  if (gen->rit_entry == NULL) {
    gen_record_scoring_plays_eager(gen);
  } else {
    gen_record_scoring_plays_impl(gen, true);
  }
}

void gen_record_pass(MoveGen *gen) {
  switch (gen->move_record_type) {
  case MOVE_RECORD_ALL:
    move_list_set_spare_move_as_pass(gen->move_list);
    move_list_insert_spare_move(gen->move_list, EQUITY_PASS_VALUE);
    break;
  case MOVE_RECORD_BEST:
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:;
    const Move *top_move = gen_get_readonly_best_move(gen);
    Move *spare_move = move_list_get_spare_move(gen->move_list);
    if (move_get_equity(top_move) < EQUITY_PASS_VALUE) {
      move_list_set_spare_move_as_pass(gen->move_list);
    } else {
      move_copy(spare_move, top_move);
    }
    move_list_insert_spare_move_top_equity(gen->move_list,
                                           move_get_equity(spare_move));
    break;
  case MOVE_RECORD_ALL_SMALL:
    move_list_set_spare_small_move_as_pass(gen->move_list);
    move_list_insert_spare_small_move(gen->move_list);
    break;
  case MOVE_RECORD_TILES_PLAYED:
    // Pass doesn't use any tiles — nothing to record.
    break;
  case MOVE_RECORD_BEST_SMALL:
    if (gen->best_move_equity_or_score < EQUITY_PASS_VALUE) {
      // No scoring play was found; result is a pass.
      small_move_set_as_pass(gen->move_list->small_moves[0]);
      gen->move_list->count = 1;
    }
    // Otherwise small_moves[0] already holds the best scoring play.
    break;
  }
}

// Index of the highest set bit of a nonzero bitset, following
// lowest_set_bit_index's guarded-intrinsic pattern.
static inline int highest_set_bit_index(uint32_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_clz)
  return 31 - __builtin_clz(bitset);
#else
  int index = 31;
  while ((bitset & ((uint32_t)1 << index)) == 0) {
    index--;
  }
  return index;
#endif
}

// The lane positions a play from the anchor at `col` can span: right of
// `limit` (the anchor's leftward limit, or -1 for none), and on either side
// of the anchor no further than `spare` of the lane's `empty` squares, plus
// the tiles it plays through, and short of any empty square in `blocked`,
// where no rack tile fits.
static inline uint32_t anchor_span_reach(uint32_t empty, uint32_t blocked,
                                         int col, int limit, int spare) {
  const uint32_t right_of_col = ~(((uint32_t)2 << col) - 1);
  const uint32_t left_of_col = ((uint32_t)1 << col) - 1;
  uint32_t right_empties = empty & right_of_col;
  uint32_t left_empties = empty & left_of_col;
  for (int tile_idx = 0; tile_idx < spare; tile_idx++) {
    right_empties &= right_empties - 1;
    if (left_empties != 0) {
      left_empties &= ~((uint32_t)1 << highest_set_bit_index(left_empties));
    }
  }
  // The nearest empty square each way that the rack cannot fill: one past
  // the tiles it has, or a blocked one.
  right_empties |= blocked & right_of_col;
  left_empties |= blocked & left_of_col;
  const int right = right_empties != 0 ? lowest_set_bit_index(right_empties) - 1
                                       : BOARD_DIM - 1;
  int left = left_empties != 0 ? highest_set_bit_index(left_empties) + 1 : 0;
  if (left <= limit) {
    left = limit + 1;
  }
  return (((uint32_t)2 << right) - 1) & ~(((uint32_t)1 << left) - 1);
}

// Records, by the generation's record type, the plays of lane `row` of the
// current gen->dir from the anchors whose plays may span a position in `hit`,
// scanning the anchors in board order (see generate_small_moves_in_lanes).
// Sets *anchors to the bits of all of the lane's anchors and *generated to
// those of the anchors generated from.
static void gen_record_lane_plays_reaching(MoveGen *gen, int row,
                                           uint32_t kwg_root_node_index,
                                           uint32_t hit, int rack_tiles,
                                           uint32_t *anchors,
                                           uint32_t *generated) {
  gen_load_lane(gen, row);
  const uint64_t rack_cross_set = gen->rack_cross_set;
  uint32_t lane_anchors = 0;
  uint32_t empty = 0;
  uint32_t blocked = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    const uint32_t col_bit = (uint32_t)1 << col;
    if (gen_cache_get_is_anchor(gen, col)) {
      lane_anchors |= col_bit;
    }
    if (gen_cache_is_empty(gen, col)) {
      empty |= col_bit;
      // Read as recursive_gen_small reads it: a cross-set of bit 0 alone
      // admits no tile.
      uint64_t possible_letters_here = gen_cache_get_cross_set(gen, col);
      if (possible_letters_here == 1) {
        possible_letters_here = 0;
      }
      if ((possible_letters_here & rack_cross_set) == 0) {
        blocked |= col_bit;
      }
    }
  }
  uint32_t lane_generated = 0;
  int last_anchor_col = INITIAL_LAST_ANCHOR_COL;
  for (uint32_t remaining = lane_anchors; remaining != 0;
       remaining &= remaining - 1) {
    const int col = lowest_set_bit_index(remaining);
    const uint32_t anchor_bit = (uint32_t)1 << col;
    const int limit =
        last_anchor_col == INITIAL_LAST_ANCHOR_COL ? -1 : last_anchor_col;
    const int spare = rack_tiles - ((empty & anchor_bit) != 0 ? 1 : 0);
    // A blocked anchor square anchors no play; one in `hit` is reached.
    if ((blocked & anchor_bit) == 0 &&
        ((hit & anchor_bit) != 0 ||
         (anchor_span_reach(empty, blocked, col, limit, spare) & hit) != 0)) {
      lane_generated |= anchor_bit;
      gen->current_anchor_col = col;
      gen->last_anchor_col = last_anchor_col;
      gen->anchor_right_extension_set =
          gen_cache_get_right_extension_set(gen, col);
      gen->current_anchor_highest_possible_score = EQUITY_MAX_VALUE;
      recursive_gen_small(gen, col, kwg_root_node_index, col, col,
                          gen->dir == BOARD_HORIZONTAL_DIRECTION, 0, 1, 0);
    }
    last_anchor_col = col;
    if ((empty & anchor_bit) == 0) {
      last_anchor_col++;
    }
  }
  *anchors = lane_anchors;
  *generated = lane_generated;
}

void generate_small_moves_in_lanes(const MoveGenArgs *args, uint64_t lane_mask,
                                   const uint32_t *lane_hits, int *lane_end,
                                   uint32_t *lane_anchors,
                                   uint32_t *lane_generated) {
  MoveGen *gen = get_movegen();
  gen_count_movegen(gen);
  gen_load_position(gen, args);
  assert(gen->move_record_type == MOVE_RECORD_ALL_SMALL);
  gen->tiles_played = 0;
  const uint32_t kwg_root_node_index = kwg_get_root_node_index(gen->kwg);
  const int rack_tiles = rack_get_total_letters(&gen->player_rack);
  for (int dir = 0; dir < 2; dir++) {
    gen->dir = dir;
    for (int row = 0; row < BOARD_DIM; row++) {
      const int lane = BOARD_DIM * dir + row;
      if (((lane_mask >> lane) & 1) != 0) {
        lane_anchors[lane] = 0;
        lane_generated[lane] = 0;
        if (gen->row_number_of_anchors_cache[lane] != 0) {
          gen_record_lane_plays_reaching(
              gen, row, kwg_root_node_index, lane_hits[lane], rack_tiles,
              &lane_anchors[lane], &lane_generated[lane]);
        }
      }
      lane_end[lane] = gen->move_list->count;
    }
  }
}

// Sets up a MOVE_RECORD_TILES_PLAYED generation: the rack's tile types are
// the target, seeded with those args already knows to be played.
static void gen_init_tiles_played(MoveGen *gen, const MoveGenArgs *args) {
  gen->tiles_played_bv = args->initial_tiles_bv;
  gen->stop_on_threshold = true;
  gen->target_tiles_bv = 0;
  const uint16_t dist_size = rack_get_dist_size(&gen->player_rack);
  for (uint16_t ml = 0; ml < dist_size; ml++) {
    if (rack_get_letter(&gen->player_rack, ml) > 0) {
      gen->target_tiles_bv |= ((uint64_t)1 << ml);
    }
  }
  gen->threshold_exceeded =
      (gen->tiles_played_bv & gen->target_tiles_bv) == gen->target_tiles_bv;
}

void generate_tiles_played_in_lanes(const MoveGenArgs *args, uint64_t lane_mask,
                                    const uint32_t *lane_hits) {
  MoveGen *gen = get_movegen();
  gen_count_movegen(gen);
  gen_load_position(gen, args);
  assert(gen->move_record_type == MOVE_RECORD_TILES_PLAYED);
  gen_init_tiles_played(gen, args);
  gen->tiles_played = 0;
  const uint32_t kwg_root_node_index = kwg_get_root_node_index(gen->kwg);
  const int rack_tiles = rack_get_total_letters(&gen->player_rack);
  for (int dir = 0; dir < 2; dir++) {
    gen->dir = dir;
    for (int row = 0; row < BOARD_DIM; row++) {
      if (gen->threshold_exceeded) {
        break;
      }
      const int lane = BOARD_DIM * dir + row;
      if (((lane_mask >> lane) & 1) != 0 &&
          gen->row_number_of_anchors_cache[lane] != 0) {
        uint32_t anchors;
        uint32_t generated;
        gen_record_lane_plays_reaching(gen, row, kwg_root_node_index,
                                       lane_hits[lane], rack_tiles, &anchors,
                                       &generated);
      }
    }
  }
  if (args->tiles_played_bv) {
    *args->tiles_played_bv = gen->tiles_played_bv;
  }
}

bool generate_best_small_move_in_lanes(const MoveGenArgs *args,
                                       uint64_t lane_mask,
                                       const SmallMove *incumbent,
                                       Equity incumbent_value) {
  MoveGen *gen = get_movegen();
  gen_count_movegen(gen);
  gen_load_position(gen, args);
  assert(gen->move_record_type == MOVE_RECORD_BEST_SMALL);
  if (incumbent != NULL) {
    // The incumbent is the best so far, so the anchor heap prunes against it
    // from the start and an equal-valued play must beat its tiny_move.
    *gen->move_list->small_moves[0] = *incumbent;
    gen->move_list->count = 1;
    gen->best_move_equity_or_score = incumbent_value;
    gen->cutoff_equity_or_score = incumbent_value;
  }
  rack_copy(&gen->full_player_rack, &gen->player_rack);
  anchor_heap_reset(&gen->anchor_heap);
  if (gen->lane_shadow_cache != NULL) {
    shadow_lanes_small_cached(gen, lane_mask);
  } else {
    for (int dir = 0; dir < 2; dir++) {
      gen->dir = dir;
      for (int row = 0; row < BOARD_DIM; row++) {
        if (((lane_mask >> (BOARD_DIM * dir + row)) & 1) != 0) {
          shadow_lane_small(gen, row);
        }
      }
    }
  }
  anchor_heapify_all(&gen->anchor_heap);
  gen_record_scoring_plays(gen);
  return gen->move_list->count > 0;
}

// Scores a remembered move on this board into move, or returns false when it
// is not legal here. It is legal when every square it plays through holds the
// letter it held, every square it fills is empty and allows its letter, the
// squares just past its ends are empty, and it touches a tile, so it forms the
// same main word and only valid cross words. Its score is the generator's sum
// for those squares.
static bool gen_score_carried_move(const MoveGen *gen,
                                   const CarriedBestMove *carried, Move *move) {
  const Move *carried_move = &carried->move;
  const int dir = move_get_dir(carried_move);
  const bool vertical = board_is_dir_vertical(dir);
  const int lane = vertical ? move_get_col_start(carried_move)
                            : move_get_row_start(carried_move);
  const int start = vertical ? move_get_row_start(carried_move)
                             : move_get_col_start(carried_move);
  const int length = move_get_tiles_length(carried_move);
  const Square *lane_squares = board_get_row_cache(gen->board_lanes, lane, dir);
  if (start > 0 && square_get_letter(&lane_squares[start - 1]) !=
                       ALPHABET_EMPTY_SQUARE_MARKER) {
    return false;
  }
  if (start + length < BOARD_DIM &&
      square_get_letter(&lane_squares[start + length]) !=
          ALPHABET_EMPTY_SQUARE_MARKER) {
    return false;
  }
  Equity main_word_score = 0;
  int word_multiplier = 1;
  Equity cross_scores = 0;
  bool touches_a_tile = false;
  for (int tile_idx = 0; tile_idx < length; tile_idx++) {
    const Square *square = &lane_squares[start + tile_idx];
    const MachineLetter board_ml = square_get_letter(square);
    const MachineLetter ml = carried_move->tiles[tile_idx];
    if (ml == PLAYED_THROUGH_MARKER) {
      if (board_ml != carried->board_letters[tile_idx]) {
        return false;
      }
      main_word_score += gen->tile_scores[board_ml];
      touches_a_tile = true;
      continue;
    }
    if (board_ml != ALPHABET_EMPTY_SQUARE_MARKER ||
        square_get_is_brick(square) ||
        !board_is_letter_allowed_in_cross_set(
            square_get_cross_set(square), get_unblanked_machine_letter(ml))) {
      return false;
    }
    const BonusSquare bonus_square = square_get_bonus_square(square);
    const int this_word_multiplier =
        bonus_square_get_word_multiplier(bonus_square);
    const Equity letter_score =
        gen->tile_scores[ml] * bonus_square_get_letter_multiplier(bonus_square);
    main_word_score += letter_score;
    word_multiplier *= this_word_multiplier;
    if (square_get_is_cross_word(square)) {
      cross_scores += (letter_score + square_get_cross_score(square)) *
                      this_word_multiplier;
      touches_a_tile = true;
    }
  }
  if (!touches_a_tile) {
    return false;
  }
  Equity bingo_bonus = 0;
  if (move_get_tiles_played(carried_move) == RACK_SIZE) {
    bingo_bonus = gen->bingo_bonus;
  }
  move_copy(move, carried_move);
  move_set_score(move, main_word_score * word_multiplier + cross_scores +
                           bingo_bonus);
  return true;
}

// The equity the generator records for a scored tile placement from rack.
static Equity gen_get_carried_move_equity(const MoveGen *gen, const Move *move,
                                          const Rack *rack) {
  Rack leave;
  rack_copy(&leave, rack);
  const int length = move_get_tiles_length(move);
  for (int tile_idx = 0; tile_idx < length; tile_idx++) {
    const MachineLetter ml = move->tiles[tile_idx];
    if (ml == PLAYED_THROUGH_MARKER) {
      continue;
    }
    rack_take_letter(&leave, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
  }
  Equity leave_value = 0;
  if (!rack_is_empty(&leave)) {
    leave_value = klv_get_leave_value(gen->klv, &leave);
  }
  return static_eval_get_move_equity_with_leave_value(
      &gen->ld, move, &leave, &gen->opponent_rack,
      gen->opening_move_word_penalties, gen->opening_move_letter_penalties,
      pat_eval_context_get_active_classes(&gen->pat_eval_ctx),
      &gen->pat_eval_ctx, gen->board_number_of_tiles_played,
      gen->number_of_tiles_in_bag, leave_value);
}

// Whether this generation can start from remembered moves and remember its
// best: a best-by-equity generation with no target, on a board with tiles and
// a bag with tiles, where leaves, scores and legality mean what they meant
// when the moves were remembered.
static bool gen_can_carry_best_moves(const MoveGen *gen,
                                     const MoveGenArgs *args) {
  // In wordsmog, carry needs the WMP generator, which records a placement's
  // tiles in its one best arrangement, and no PAT term, whose budgeted search
  // (see WordsmogPATGroup) is not exact, so seeding it could change its
  // result. A carried move's check then holds as it does in classic: the same
  // letters on the same squares form the same main word, whose letters are a
  // word's.
  return args->best_move_carry != NULL &&
         gen->move_record_type == MOVE_RECORD_BEST &&
         gen->move_sort_type == MOVE_SORT_EQUITY && !gen->stop_on_threshold &&
         (!gen->is_wordsmog || (wmp_move_gen_is_active(&gen->wmp_move_gen) &&
                                !gen_pat_is_active(gen))) &&
         args->override_kwg == NULL && gen->number_of_tiles_in_bag > 0 &&
         gen->board_number_of_tiles_played > 0;
}

// Hashes what decides which moves a lane holds and what they score: its
// letters, bonus squares, anchors, and the cross sets, cross scores and
// cross word flags of its empty squares. The extension sets are left out:
// they only prune, and are computed from the lane's letters. Lanes without
// anchors hold no moves and hash to 0, which no other lane hashes to.
static uint64_t gen_hash_lane(const Square *lane_squares) {
  // Two independent chains, so that they run in parallel. Each step is a
  // bijection of the chain so far, so lanes that differ in one square's
  // cross set or in one square's other fields always differ.
  uint64_t cross_set_hash = UINT64_C(0x9E3779B97F4A7C15);
  uint64_t square_hash = UINT64_C(0xC2B2AE3D27D4EB4F);
  for (int col = 0; col < BOARD_DIM; col++) {
    const Square *square = &lane_squares[col];
    const uint64_t square_word =
        (uint64_t)(uint32_t)square_get_cross_score(square) |
        ((uint64_t)square_get_letter(square) << 32) |
        ((uint64_t)square_get_bonus_square(square).raw << 40) |
        ((uint64_t)square_get_anchor(square) << 48) |
        ((uint64_t)square_get_is_cross_word(square) << 49);
    cross_set_hash =
        (cross_set_hash ^ square_get_cross_set(square)) * LANE_HASH_MULTIPLIER;
    cross_set_hash ^= cross_set_hash >> 29;
    square_hash = (square_hash ^ square_word) * LANE_HASH_MULTIPLIER;
    square_hash ^= square_hash >> 29;
  }
  uint64_t hash = cross_set_hash * LANE_HASH_MULTIPLIER;
  hash ^= hash >> 32;
  hash = (hash ^ square_hash) * LANE_HASH_MULTIPLIER;
  hash ^= hash >> 32;
  return hash | 1;
}

static void gen_hash_lanes(const MoveGen *gen, uint64_t *lane_hashes) {
  for (int dir = 0; dir < 2; dir++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      const int lane = BOARD_DIM * dir + row;
      lane_hashes[lane] = 0;
      if (gen->row_number_of_anchors_cache[lane] != 0) {
        lane_hashes[lane] =
            gen_hash_lane(board_get_row_cache(gen->board_lanes, row, dir));
      }
    }
  }
}

// The lanes the bounds in found show need not be searched, given the
// generation's best move so far: those it is no worse than. A lane with a
// bound hashes as it did when a generation searched it, so it holds the same
// moves with the same scores, leaves and, without PAT, equities as it did
// then, and that generation's best move, the bound, beat each of them under
// compare_moves. The generation's best move, at least as good as the bound,
// beats them too.
static uint64_t gen_get_skippable_lanes(const MoveGen *gen,
                                        const CarriedRackFind *found) {
  const Move *best_move = gen_get_readonly_best_move(gen);
  uint64_t skippable_lanes = 0;
  for (int lane = 0; lane < BEST_MOVE_CARRY_NUM_LANES; lane++) {
    if ((found->lanes_with_bounds & ((uint64_t)1 << lane)) &&
        compare_moves(best_move, &found->lane_bounds[lane].bound, true) != 0) {
      skippable_lanes |= (uint64_t)1 << lane;
    }
  }
  return skippable_lanes;
}

// Starts the generation from the best of the remembered moves for this rack
// that are still legal, and drops every WMP anchor that cannot reach it. The
// moves are found among the plays the generator would record, at the equity
// it would record them with, and compare_moves breaks ties, so the
// generation's best move is unchanged. Given lane_hashes, the lanes' bounds
// that are moves of their lanes are such moves too, and it also drops the
// lanes gen_get_skippable_lanes finds, by clearing their anchor counts.
// Returns those lanes.
static uint64_t gen_seed_carried_best_move(MoveGen *gen, BestMoveCarry *carry,
                                           const uint64_t *lane_hashes,
                                           const Rack *rack) {
  CarriedRackFind found;
  const bool can_skip_lanes = lane_hashes != NULL;
  if (!best_move_carry_find(carry, rack, gen->player_index, lane_hashes,
                            gen->number_of_tiles_in_bag, &found)) {
    return 0;
  }
  bool seeded = false;
  for (int move_idx = 0; move_idx < found.count; move_idx++) {
    const CarriedBestMove *carried = &found.moves[move_idx];
    Move *current_move = gen_get_current_move(gen);
    if (!gen_score_carried_move(gen, carried, current_move)) {
      continue;
    }
    move_set_equity(current_move,
                    gen_get_carried_move_equity(gen, current_move, rack));
    if (compare_moves(current_move, gen_get_readonly_best_move(gen), true) ==
        1) {
      gen_switch_best_move_and_current_move(gen);
      seeded = true;
    }
  }
  uint64_t skippable_lanes = 0;
  if (can_skip_lanes) {
    for (int lane = 0; lane < BEST_MOVE_CARRY_NUM_LANES; lane++) {
      const CarriedLaneBound *lane_bound = &found.lane_bounds[lane];
      if ((found.lanes_with_bounds & ((uint64_t)1 << lane)) &&
          lane_bound->bound_is_in_lane &&
          compare_moves(&lane_bound->bound, gen_get_readonly_best_move(gen),
                        true) == 1) {
        move_copy(gen_get_current_move(gen), &lane_bound->bound);
        gen_switch_best_move_and_current_move(gen);
        seeded = true;
      }
    }
    skippable_lanes = gen_get_skippable_lanes(gen, &found);
  }
  if (seeded) {
    gen_update_cutoff_equity_or_score(gen);
    // With no target, target_equity_cutoff is read only by
    // wmp_move_gen_add_anchors, which drops anchors whose bound is below it.
    gen->target_equity_cutoff =
        move_get_equity(gen_get_readonly_best_move(gen));
  }
  for (int lane = 0; lane < BEST_MOVE_CARRY_NUM_LANES; lane++) {
    if (skippable_lanes & ((uint64_t)1 << lane)) {
      gen->row_number_of_anchors_cache[lane] = 0;
    }
  }
  return skippable_lanes;
}

// Remembers the generation's best move for its rack, and given lane_hashes as
// the bound of each lane it searched. Plays of one tile are not carried to
// other boards: the generator records each in only one direction, and which
// one depends on the board.
static void gen_remember_best_move(const MoveGen *gen, BestMoveCarry *carry,
                                   const uint64_t *lane_hashes,
                                   uint64_t skipped_lanes, const Rack *rack) {
  const Move *best_move = gen_get_readonly_best_move(gen);
  if (move_get_equity(best_move) == EQUITY_INITIAL_VALUE) {
    return;
  }
  uint64_t searched_lanes = 0;
  if (lane_hashes != NULL) {
    for (int lane = 0; lane < BEST_MOVE_CARRY_NUM_LANES; lane++) {
      if (lane_hashes[lane] != 0 && !(skipped_lanes & ((uint64_t)1 << lane))) {
        searched_lanes |= (uint64_t)1 << lane;
      }
    }
  }
  const bool carry_move =
      move_get_type(best_move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
      move_get_tiles_played(best_move) >= 2;
  if (!carry_move && searched_lanes == 0) {
    return;
  }
  CarriedBestMove carried;
  if (carry_move) {
    move_copy(&carried.move, best_move);
    const int dir = move_get_dir(best_move);
    const bool vertical = board_is_dir_vertical(dir);
    const int lane = vertical ? move_get_col_start(best_move)
                              : move_get_row_start(best_move);
    const int start = vertical ? move_get_row_start(best_move)
                               : move_get_col_start(best_move);
    const Square *lane_squares =
        board_get_row_cache(gen->board_lanes, lane, dir);
    const int length = move_get_tiles_length(best_move);
    for (int tile_idx = 0; tile_idx < length; tile_idx++) {
      carried.board_letters[tile_idx] =
          square_get_letter(&lane_squares[start + tile_idx]);
    }
  }
  best_move_carry_add(carry, rack, gen->player_index,
                      carry_move ? &carried : NULL, best_move, lane_hashes,
                      searched_lanes, gen->number_of_tiles_in_bag);
}

// The wordsmog play graph (see move_gen.h). Its frontier is the shadow's
// anchor heap and a heap of nodes:
// - An anchor is expanded into its groups, a group for each start column and
//   tile multiset with a legal arrangement, each pushed as the root of its
//   arrangements.
// - A node of a group is the arrangements that put its pins on its first
//   squares; expanding it pins one more square to each tile the node has
//   left. A node's equity is that of the best of its arrangements, exactly:
//   wordsmog_best_arrangement with its pins.
// - A node with every square pinned is one play.
// Every anchor's equity bounds its plays', so the next play is the best node
// popped while no anchor bounds more.

// A placement and tile multiset: what a group's plays share.
typedef struct WordsmogGraphGroup {
  int row;
  int dir;
  int start_col;
  int word_length;
  int tiles_played;
  // The play's tiles, with PLAYED_THROUGH_MARKER on the board's tiles; an
  // arrangement's tiles go at square_letter_idxs.
  MachineLetter strip[BOARD_DIM];
  int square_letter_idxs[RACK_SIZE];
  int num_squares;
  WordsmogSquare squares[RACK_SIZE];
  // The group's tiles, in the graph's tiles, shared by the start columns of
  // a subrack.
  int tiles_idx;
  // A play's equity is base_equity plus its arrangement's score and
  // adjustment, and its score is base_score plus its arrangement's score.
  Equity base_equity;
  Equity base_score;
  // The equity and score of its best play.
  Equity best_equity;
  Equity best_score;
} WordsmogGraphGroup;

// The arrangements of a group that put pins[square] on each square below
// num_pinned, with the equity and score of the best of them.
typedef struct WordsmogGraphNode {
  Equity equity;
  Equity score;
  // When the node was pushed: equal nodes come out first in, first out.
  uint32_t order;
  int group_idx;
  int num_pinned;
  MachineLetter pins[RACK_SIZE];
} WordsmogGraphNode;

struct WordsmogPlayGraph {
  // The graph's own generator, which keeps the position's anchors and lanes
  // between calls.
  MoveGen *gen;
  MoveList *move_list;
  WordsmogGraphGroup *groups;
  int num_groups;
  int groups_capacity;
  WordsmogTiles *tiles;
  int num_tiles;
  int tiles_capacity;
  // The groups best first, once wordsmog_play_graph_rank_groups has run.
  int *group_ranks;
  WordsmogGraphNode *heap;
  int heap_count;
  int heap_capacity;
  uint32_t next_order;
};

// Best equity, then score. Among nodes that tie, the one with more squares
// pinned, and then the smaller pins, so that a node's best play comes out
// before the plays it ties with, without visiting their nodes first.
static bool wordsmog_graph_node_ranks_above(const WordsmogGraphNode *a,
                                            const WordsmogGraphNode *b) {
  if (a->equity != b->equity) {
    return a->equity > b->equity;
  }
  if (a->score != b->score) {
    return a->score > b->score;
  }
  if (a->num_pinned != b->num_pinned) {
    return a->num_pinned > b->num_pinned;
  }
  const int pins_order = memcmp(a->pins, b->pins, sizeof(a->pins));
  if (pins_order != 0) {
    return pins_order < 0;
  }
  return a->order < b->order;
}

static void wordsmog_graph_push(WordsmogPlayGraph *graph,
                                WordsmogGraphNode node) {
  if (graph->heap_count == graph->heap_capacity) {
    graph->heap_capacity =
        graph->heap_capacity == 0 ? 256 : graph->heap_capacity * 2;
    graph->heap = realloc_or_die(graph->heap, (size_t)graph->heap_capacity *
                                                  sizeof(WordsmogGraphNode));
  }
  node.order = graph->next_order++;
  int child = graph->heap_count++;
  while (child > 0) {
    const int parent = (child - 1) / 2;
    if (!wordsmog_graph_node_ranks_above(&node, &graph->heap[parent])) {
      break;
    }
    graph->heap[child] = graph->heap[parent];
    child = parent;
  }
  graph->heap[child] = node;
}

static WordsmogGraphNode wordsmog_graph_pop(WordsmogPlayGraph *graph) {
  const WordsmogGraphNode top = graph->heap[0];
  const WordsmogGraphNode last = graph->heap[--graph->heap_count];
  int parent = 0;
  for (;;) {
    int child = 2 * parent + 1;
    if (child >= graph->heap_count) {
      break;
    }
    if (child + 1 < graph->heap_count &&
        wordsmog_graph_node_ranks_above(&graph->heap[child + 1],
                                        &graph->heap[child])) {
      child++;
    }
    if (!wordsmog_graph_node_ranks_above(&graph->heap[child], &last)) {
      break;
    }
    graph->heap[parent] = graph->heap[child];
    parent = child;
  }
  if (graph->heap_count > 0) {
    graph->heap[parent] = last;
  }
  return top;
}

// The node of group's arrangements with pins, and whether any is legal.
static bool wordsmog_graph_node_init(const WordsmogPlayGraph *graph,
                                     int group_idx, int num_pinned,
                                     const MachineLetter pins[RACK_SIZE],
                                     WordsmogGraphNode *node,
                                     WordsmogArrangement *arrangement) {
  const WordsmogGraphGroup *group = &graph->groups[group_idx];
  if (!wordsmog_best_arrangement(group->squares, group->num_squares,
                                 &graph->tiles[group->tiles_idx], pins,
                                 arrangement)) {
    return false;
  }
  node->equity =
      group->base_equity + arrangement->score + arrangement->adjustment;
  node->score = group->base_score + arrangement->score;
  node->group_idx = group_idx;
  node->num_pinned = num_pinned;
  memcpy(node->pins, pins, sizeof(node->pins));
  return true;
}

// Adds the groups of an anchor, each as the root of its arrangements.
static void wordsmog_graph_expand_anchor(WordsmogPlayGraph *graph,
                                         const Anchor *anchor) {
  MoveGen *gen = graph->gen;
  gen_load_anchor(gen, anchor);
  gen->max_tiles_to_play = anchor->tiles_to_play;
  WMPMoveGen *wgen = &gen->wmp_move_gen;
  wmp_move_gen_set_playthrough_bit_rack(wgen, anchor, gen->row_squares, NULL,
                                        gen->wit_len_lane, NULL);
  wmp_move_gen_playthrough_metadata_init(wgen, anchor);
  wmp_move_gen_build_playthrough_subracks(wgen);
  const int num_subrack_combinations =
      wmp_move_gen_get_num_subrack_combinations(wgen);
  for (int subrack_idx = 0; subrack_idx < num_subrack_combinations;
       subrack_idx++) {
    BitRack main_word;
    const WMPEntry *entry =
        wmp_move_gen_get_subrack_entry(wgen, subrack_idx, false, &main_word);
    if (entry == NULL) {
      continue;
    }
    if (gen->wmp_prune_subracks_by_leave) {
      wgen->leave_value = wmp_move_gen_get_leave_value(wgen, subrack_idx);
    }
    if (gen->number_of_tiles_in_bag == 0) {
      wordmap_gen_set_empty_bag_leave(gen, anchor, subrack_idx);
    }
    if (graph->num_tiles == graph->tiles_capacity) {
      graph->tiles_capacity =
          graph->tiles_capacity == 0 ? 16 : graph->tiles_capacity * 2;
      graph->tiles = realloc_or_die(
          graph->tiles, (size_t)graph->tiles_capacity * sizeof(WordsmogTiles));
    }
    const int tiles_idx = graph->num_tiles;
    WordsmogTiles *tiles = &graph->tiles[tiles_idx];
    wordsmog_tiles_init(
        gen, wmp_move_gen_get_nonplaythrough_subrack(wgen, subrack_idx),
        &main_word, entry, &graph->tiles[tiles_idx]);
    bool tiles_used = false;
    for (int start_col = anchor->leftmost_start_col;
         start_col <= anchor->rightmost_start_col; start_col++) {
      if (graph->num_groups == graph->groups_capacity) {
        graph->groups_capacity =
            graph->groups_capacity == 0 ? 64 : graph->groups_capacity * 2;
        graph->groups =
            realloc_or_die(graph->groups, (size_t)graph->groups_capacity *
                                              sizeof(WordsmogGraphGroup));
      }
      WordsmogGraphGroup *group = &graph->groups[graph->num_groups];
      Equity base_score;
      group->num_squares =
          wordsmog_placement_squares(gen, start_col, group->squares,
                                     group->square_letter_idxs, &base_score);
      assert(group->num_squares == anchor->tiles_to_play);
      group->tiles_idx = tiles_idx;
      WordsmogArrangement best;
      if (!wordsmog_best_arrangement(group->squares, group->num_squares, tiles,
                                     NULL, &best)) {
        continue;
      }
      tiles_used = true;
      // Price the best arrangement as move generation would, and keep what
      // the group's other arrangements share with it.
      for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
        gen->playthrough_marked[group->square_letter_idxs[square_idx]] =
            best.tiles[square_idx];
      }
      const Equity score = wmp_play_score(gen, start_col);
      Move *move = move_list_get_spare_move(graph->move_list);
      set_play_for_record_wmp(gen, move, start_col, score);
      const Equity equity =
          get_move_equity_for_sort_type_wmp(gen, move, wgen->leave_value);
      group->row = gen->current_row_index;
      group->dir = gen->dir;
      group->start_col = start_col;
      group->word_length = wgen->word_length;
      group->tiles_played = anchor->tiles_to_play;
      memcpy(group->strip, gen->playthrough_marked, (size_t)wgen->word_length);
      group->base_equity = equity - best.score - best.adjustment;
      group->base_score = score - best.score;
      MachineLetter pins[RACK_SIZE];
      memset(pins, WORDSMOG_PIN_FREE, sizeof(pins));
      WordsmogGraphNode root;
      const bool legal = wordsmog_graph_node_init(graph, graph->num_groups, 0,
                                                  pins, &root, &best);
      assert(legal);
      (void)legal;
      group->best_equity = root.equity;
      group->best_score = root.score;
      graph->num_groups++;
      wordsmog_graph_push(graph, root);
    }
    if (tiles_used) {
      graph->num_tiles++;
    }
  }
}

// The children of node: one more square pinned to each tile the node has left
// that has a legal arrangement there. Returns how many.
static int wordsmog_graph_children(const WordsmogPlayGraph *graph,
                                   const WordsmogGraphNode *node,
                                   WordsmogGraphNode children[RACK_SIZE + 1]) {
  const WordsmogGraphGroup *group = &graph->groups[node->group_idx];
  const WordsmogTiles *tiles = &graph->tiles[group->tiles_idx];
  int counts[RACK_SIZE];
  int blanks = tiles->num_blanks;
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    counts[letter_idx] = tiles->counts[letter_idx];
  }
  for (int square_idx = 0; square_idx < node->num_pinned; square_idx++) {
    if (node->pins[square_idx] == WORDSMOG_PIN_BLANK) {
      blanks--;
      continue;
    }
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      if (tiles->letters[letter_idx] == node->pins[square_idx]) {
        counts[letter_idx]--;
      }
    }
  }
  MachineLetter pins[RACK_SIZE];
  memcpy(pins, node->pins, sizeof(pins));
  const int square_idx = node->num_pinned;
  const uint64_t cross_set = group->squares[square_idx].cross_set;
  WordsmogArrangement arrangement;
  int num_children = 0;
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    const MachineLetter ml = tiles->letters[letter_idx];
    if (counts[letter_idx] == 0 || ((cross_set >> ml) & 1U) == 0) {
      continue;
    }
    pins[square_idx] = ml;
    if (wordsmog_graph_node_init(graph, node->group_idx, square_idx + 1, pins,
                                 &children[num_children], &arrangement)) {
      num_children++;
    }
  }
  if (blanks > 0) {
    pins[square_idx] = WORDSMOG_PIN_BLANK;
    if (wordsmog_graph_node_init(graph, node->group_idx, square_idx + 1, pins,
                                 &children[num_children], &arrangement)) {
      num_children++;
    }
  }
  return num_children;
}

static void wordsmog_graph_expand_node(WordsmogPlayGraph *graph,
                                       const WordsmogGraphNode *node) {
  WordsmogGraphNode children[RACK_SIZE + 1];
  const int num_children = wordsmog_graph_children(graph, node, children);
  for (int child_idx = 0; child_idx < num_children; child_idx++) {
    wordsmog_graph_push(graph, children[child_idx]);
  }
}

// Writes the best play of node.
static void wordsmog_graph_write_play(const WordsmogPlayGraph *graph,
                                      const WordsmogGraphNode *node,
                                      Move *move) {
  const WordsmogGraphGroup *group = &graph->groups[node->group_idx];
  WordsmogArrangement arrangement;
  const bool legal = wordsmog_best_arrangement(
      group->squares, group->num_squares, &graph->tiles[group->tiles_idx],
      node->pins, &arrangement);
  assert(legal);
  (void)legal;
  MachineLetter strip[BOARD_DIM];
  memcpy(strip, group->strip, (size_t)group->word_length);
  for (int square_idx = 0; square_idx < group->num_squares; square_idx++) {
    strip[group->square_letter_idxs[square_idx]] =
        arrangement.tiles[square_idx];
  }
  move_set_all_except_equity(move, strip, 0, group->word_length - 1,
                             group->base_score + arrangement.score, group->row,
                             group->start_col, group->tiles_played, group->dir,
                             GAME_EVENT_TILE_PLACEMENT_MOVE);
  if (board_is_dir_vertical(group->dir)) {
    move_set_row_start(move, group->start_col);
    move_set_col_start(move, group->row);
  }
  move_set_equity(move, group->base_equity + arrangement.score +
                            arrangement.adjustment);
}

WordsmogPlayGraph *wordsmog_play_graph_create(const Game *game,
                                              move_sort_t move_sort_type) {
  WordsmogPlayGraph *graph = calloc_or_die(1, sizeof(WordsmogPlayGraph));
  graph->gen = calloc_or_die(1, sizeof(MoveGen));
  graph->move_list = move_list_create(1);
  // The graph ranks plays by their arrangements, without the PAT term, which
  // would rank the arrangements of a group differently (see
  // WordsmogPATGroup).
  const MoveGenArgs args = {.game = game,
                            .move_list = graph->move_list,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = move_sort_type,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .disable_pat = true};
  MoveGen *gen = graph->gen;
  gen_load_position(gen, &args);
  if (!gen->is_wordsmog || !wmp_move_gen_is_active(&gen->wmp_move_gen)) {
    log_fatal("the wordsmog play graph needs a wordsmog game with a WMP");
  }
  // As generate_moves: the leaves, the rack's subracks with their WMP
  // entries, and the shadow's anchors. The exchanges recorded on the way are
  // not part of the graph.
  gen_look_up_leaves_and_record_exchanges(gen);
  const bool check_leaves =
      gen->number_of_tiles_in_bag > 0 && gen->move_sort_type != MOVE_SORT_SCORE;
  wmp_move_gen_check_nonplaythrough_existence(&gen->wmp_move_gen, check_leaves,
                                              &gen->leave_map, false, false);
  gen_shadow(gen);
  gen->tiles_played = 0;
  // Values outside their valid ranges, so the first anchor loads its lane.
  gen->current_row_index = -1;
  gen->dir = -1;
  return graph;
}

bool wordsmog_play_graph_next(WordsmogPlayGraph *graph, Move *move) {
  AnchorHeap *anchors = &graph->gen->anchor_heap;
  for (;;) {
    if (anchors->count > 0 && (graph->heap_count == 0 ||
                               anchors->anchors[0].highest_possible_equity >=
                                   graph->heap[0].equity)) {
      const Anchor anchor = anchor_heap_extract_max(anchors);
      wordsmog_graph_expand_anchor(graph, &anchor);
      continue;
    }
    if (graph->heap_count == 0) {
      return false;
    }
    const WordsmogGraphNode node = wordsmog_graph_pop(graph);
    if (node.num_pinned == graph->groups[node.group_idx].num_squares) {
      wordsmog_graph_write_play(graph, &node, move);
      return true;
    }
    wordsmog_graph_expand_node(graph, &node);
  }
}

void wordsmog_play_graph_count(WordsmogPlayGraph *graph, uint64_t *arrangements,
                               uint64_t *plays) {
  AnchorHeap *anchors = &graph->gen->anchor_heap;
  while (anchors->count > 0) {
    const Anchor anchor = anchor_heap_extract_max(anchors);
    wordsmog_graph_expand_anchor(graph, &anchor);
  }
  *arrangements = 0;
  *plays = 0;
  for (int group_idx = 0; group_idx < graph->num_groups; group_idx++) {
    const WordsmogGraphGroup *group = &graph->groups[group_idx];
    uint64_t group_arrangements;
    uint64_t group_plays;
    wordsmog_count_arrangements(group->squares, group->num_squares,
                                &graph->tiles[group->tiles_idx], NULL,
                                &group_arrangements, &group_plays);
    *arrangements += group_arrangements;
    *plays += group_plays;
  }
}

// Exploring the graph's nodes (see move_gen.h).

static void wordsmog_graph_node_to_public(const WordsmogPlayGraph *graph,
                                          const WordsmogGraphNode *node,
                                          WordsmogPlayNode *public_node) {
  public_node->group_idx = node->group_idx;
  public_node->num_squares = graph->groups[node->group_idx].num_squares;
  public_node->num_pinned = node->num_pinned;
  memcpy(public_node->pins, node->pins, sizeof(public_node->pins));
  public_node->equity = node->equity;
  public_node->score = node->score;
}

static void wordsmog_graph_node_from_public(const WordsmogPlayNode *public_node,
                                            WordsmogGraphNode *node) {
  node->equity = public_node->equity;
  node->score = public_node->score;
  node->order = 0;
  node->group_idx = public_node->group_idx;
  node->num_pinned = public_node->num_pinned;
  memcpy(node->pins, public_node->pins, sizeof(node->pins));
}

// A group's rank key: its best play's equity and score, then its index.
typedef struct WordsmogGroupRank {
  Equity equity;
  Equity score;
  int group_idx;
} WordsmogGroupRank;

static int compare_wordsmog_group_ranks(const void *a, const void *b) {
  const WordsmogGroupRank *rank_a = a;
  const WordsmogGroupRank *rank_b = b;
  if (rank_a->equity != rank_b->equity) {
    return rank_a->equity > rank_b->equity ? -1 : 1;
  }
  if (rank_a->score != rank_b->score) {
    return rank_a->score > rank_b->score ? -1 : 1;
  }
  return rank_a->group_idx - rank_b->group_idx;
}

int wordsmog_play_graph_rank_groups(WordsmogPlayGraph *graph) {
  AnchorHeap *anchors = &graph->gen->anchor_heap;
  while (anchors->count > 0) {
    const Anchor anchor = anchor_heap_extract_max(anchors);
    wordsmog_graph_expand_anchor(graph, &anchor);
  }
  WordsmogGroupRank *ranks = malloc_or_die((size_t)(graph->num_groups + 1) *
                                           sizeof(WordsmogGroupRank));
  for (int group_idx = 0; group_idx < graph->num_groups; group_idx++) {
    ranks[group_idx].equity = graph->groups[group_idx].best_equity;
    ranks[group_idx].score = graph->groups[group_idx].best_score;
    ranks[group_idx].group_idx = group_idx;
  }
  qsort(ranks, (size_t)graph->num_groups, sizeof(WordsmogGroupRank),
        compare_wordsmog_group_ranks);
  free(graph->group_ranks);
  graph->group_ranks =
      malloc_or_die((size_t)(graph->num_groups + 1) * sizeof(int));
  for (int rank = 0; rank < graph->num_groups; rank++) {
    graph->group_ranks[rank] = ranks[rank].group_idx;
  }
  free(ranks);
  return graph->num_groups;
}

void wordsmog_play_graph_get_group(const WordsmogPlayGraph *graph, int rank,
                                   WordsmogPlayNode *node) {
  assert(graph->group_ranks != NULL);
  assert(rank >= 0 && rank < graph->num_groups);
  const int group_idx = graph->group_ranks[rank];
  const WordsmogGraphGroup *group = &graph->groups[group_idx];
  node->group_idx = group_idx;
  node->num_squares = group->num_squares;
  node->num_pinned = 0;
  memset(node->pins, WORDSMOG_PIN_FREE, sizeof(node->pins));
  node->equity = group->best_equity;
  node->score = group->best_score;
}

void wordsmog_play_graph_get_play(const WordsmogPlayGraph *graph,
                                  const WordsmogPlayNode *node, Move *move) {
  WordsmogGraphNode internal_node;
  wordsmog_graph_node_from_public(node, &internal_node);
  wordsmog_graph_write_play(graph, &internal_node, move);
}

void wordsmog_play_graph_count_node(const WordsmogPlayGraph *graph,
                                    const WordsmogPlayNode *node,
                                    uint64_t *arrangements, uint64_t *plays) {
  const WordsmogGraphGroup *group = &graph->groups[node->group_idx];
  wordsmog_count_arrangements(group->squares, group->num_squares,
                              &graph->tiles[group->tiles_idx], node->pins,
                              arrangements, plays);
}

int wordsmog_play_graph_get_children(const WordsmogPlayGraph *graph,
                                     const WordsmogPlayNode *node,
                                     WordsmogPlayNode children[RACK_SIZE + 1]) {
  if (node->num_pinned == node->num_squares) {
    return 0;
  }
  WordsmogGraphNode internal_node;
  wordsmog_graph_node_from_public(node, &internal_node);
  WordsmogGraphNode internal_children[RACK_SIZE + 1];
  const int num_children =
      wordsmog_graph_children(graph, &internal_node, internal_children);
  // Best first, as the graph would pop them.
  for (int child_idx = 1; child_idx < num_children; child_idx++) {
    const WordsmogGraphNode child = internal_children[child_idx];
    int insert_idx = child_idx;
    while (insert_idx > 0 && wordsmog_graph_node_ranks_above(
                                 &child, &internal_children[insert_idx - 1])) {
      internal_children[insert_idx] = internal_children[insert_idx - 1];
      insert_idx--;
    }
    internal_children[insert_idx] = child;
  }
  for (int child_idx = 0; child_idx < num_children; child_idx++) {
    wordsmog_graph_node_to_public(graph, &internal_children[child_idx],
                                  &children[child_idx]);
  }
  return num_children;
}

void wordsmog_play_graph_get_next_square(const WordsmogPlayGraph *graph,
                                         const WordsmogPlayNode *node, int *row,
                                         int *col) {
  assert(node->num_pinned < node->num_squares);
  const WordsmogGraphGroup *group = &graph->groups[node->group_idx];
  const int lane_position =
      group->start_col + group->square_letter_idxs[node->num_pinned];
  if (board_is_dir_vertical(group->dir)) {
    *row = lane_position;
    *col = group->row;
  } else {
    *row = group->row;
    *col = lane_position;
  }
}

void wordsmog_play_graph_destroy(WordsmogPlayGraph *graph) {
  if (graph == NULL) {
    return;
  }
  free(graph->heap);
  free(graph->groups);
  free(graph->tiles);
  free(graph->group_ranks);
  move_list_destroy(graph->move_list);
  generator_destroy(graph->gen);
  free(graph);
}

void generate_moves(const MoveGenArgs *args) {
  MoveGen *gen = get_movegen();
  gen_count_movegen(gen);
  gen_load_position(gen, args);
  if (gen->move_record_type == MOVE_RECORD_ALL_SMALL ||
      gen->move_record_type == MOVE_RECORD_TILES_PLAYED) {
    if (gen->move_record_type == MOVE_RECORD_TILES_PLAYED) {
      gen_init_tiles_played(gen, args);
    }
    if (!gen->threshold_exceeded) {
      gen_record_scoring_plays_unordered(gen, true);
    }
    if (gen->move_record_type == MOVE_RECORD_TILES_PLAYED) {
      // Write the bitvector to the caller's output pointer
      if (args->tiles_played_bv) {
        *args->tiles_played_bv = gen->tiles_played_bv;
      }
      return; // No pass recording needed
    }
  } else if (gen->move_record_type == MOVE_RECORD_BEST_SMALL) {
    // BEST_SMALL uses small shadow and small recursive_gen paths that skip
    // leave_map, WMP, and wordsmog operations entirely.
    gen_shadow_small(gen);
    gen_record_scoring_plays(gen);
  } else {
    gen_look_up_leaves_and_record_exchanges(gen);

    if (wmp_move_gen_is_active(&gen->wmp_move_gen)) {
      const bool check_leaves = (gen->number_of_tiles_in_bag > 0) &&
                                (gen->move_sort_type != MOVE_SORT_SCORE);
      // Per-thread subrack enumeration cache. The enumeration output is
      // rack-determined, so we hit on repeat racks (common in sims).
      // IMPORTANT: leave_values written by the enumeration read from
      // leave_map.leave_values, which is only populated when
      // gen_look_up_leaves_and_record_exchanges actually walked the leave
      // space -- i.e. when rit_entry != NULL (RIT unpacked leaves) or when
      // (check_leaves || add_exchange) (KLV walk ran generate_exchange_moves).
      // Otherwise leave_map.leave_values retains stale values from a prior
      // rack and enumerating reads garbage. We only use the cache when
      // those values are known-fresh, on both write and read.
      WMPMoveGen *wgen = &gen->wmp_move_gen;
      // Mirror the condition in gen_look_up_leaves_and_record_exchanges:
      // add_exchange=true iff there are enough unseen tiles for the
      // exchange walk to make sense.
      const bool add_exchange =
          gen->number_of_tiles_in_bag +
              rack_get_total_letters(&gen->opponent_rack) >=
          (RACK_SIZE * 2);
      const bool leaves_are_populated =
          (gen->rit_entry != NULL) || check_leaves || add_exchange;
      const uint32_t subrack_slot = bit_rack_get_bucket_index(
          &wgen->player_bit_rack, MOVEGEN_SUBRACK_CACHE_SIZE);
      SubrackEnumCacheEntry *subrack_entry = &gen->subrack_cache[subrack_slot];
      const bool subrack_cache_hit =
          leaves_are_populated && subrack_entry->valid &&
          bit_rack_equals(&subrack_entry->key, &wgen->player_bit_rack);
      if (subrack_cache_hit) {
        // Restore the enumeration output and whichever per-subrack WMP
        // entries prior occurrences of this rack actually needed.
        memcpy(wgen->count_by_size, subrack_entry->count_by_size,
               sizeof(wgen->count_by_size));
        for (int i = 0; i < MOVEGEN_SUBRACK_CACHE_ENTRIES; i++) {
          wgen->nonplaythrough_infos[i].subrack = subrack_entry->subracks[i];
          wgen->nonplaythrough_infos[i].leave_value =
              subrack_entry->leave_values[i];
          wgen->nonplaythrough_infos[i].wmp_entry =
              subrack_entry->wmp_entries[i];
        }
      }
      if (leaves_are_populated) {
        wgen->nonplaythrough_wmp_entry_cache = subrack_entry->wmp_entries;
      }
      if (gen->rit_entry != NULL) {
        // The RIT supplies existence and best-leave bounds. Defer each
        // per-subrack WMP lookup until wordmap generation survives those
        // bounds and actually needs that subrack's words.
        wmp_move_gen_check_nonplaythrough_existence_with_rit(
            wgen, check_leaves, &gen->leave_map, gen->rit_entry,
            /*subracks_precomputed=*/subrack_cache_hit);
      } else {
        wmp_move_gen_check_nonplaythrough_existence(
            wgen, check_leaves, &gen->leave_map,
            /*subracks_precomputed=*/subrack_cache_hit,
            /*wmp_entries_precomputed=*/subrack_cache_hit);
      }
      if (!subrack_cache_hit && leaves_are_populated) {
        // Store the newly computed enumeration and any WMP entries that have
        // already been resolved. Only cache when leaves_are_populated so we
        // don't stash garbage leave_values from an uninitialized leave_map.
        subrack_entry->key = wgen->player_bit_rack;
        subrack_entry->valid = true;
        memcpy(subrack_entry->count_by_size, wgen->count_by_size,
               sizeof(wgen->count_by_size));
        for (int i = 0; i < MOVEGEN_SUBRACK_CACHE_ENTRIES; i++) {
          subrack_entry->subracks[i] = wgen->nonplaythrough_infos[i].subrack;
          subrack_entry->leave_values[i] =
              wgen->nonplaythrough_infos[i].leave_value;
          subrack_entry->wmp_entries[i] =
              wgen->nonplaythrough_infos[i].wmp_entry;
        }
      }
    }

    if (gen->stop_on_threshold && gen->threshold_exceeded) {
      gen_record_pass(gen);
      return;
    }

    if (gen->move_record_type == MOVE_RECORD_ALL && !gen->stop_on_threshold &&
        !gen->is_wordsmog && !wmp_move_gen_is_active(&gen->wmp_move_gen)) {
      // Exhaustive traversal needs no shadow bounds or anchor ordering. The
      // MoveList still applies its usual capacity and score/equity tie breaks.
      leave_map_set_current_index(
          &gen->leave_map,
          (1 << rack_get_total_letters(&gen->player_rack)) - 1);
      gen_record_scoring_plays_unordered(gen, false);
      gen_record_pass(gen);
      return;
    }

    const bool carry_best_moves = gen_can_carry_best_moves(gen, args);
    uint64_t lane_hash_storage[2 * BOARD_DIM];
    const uint64_t *lane_hashes = NULL;
    uint64_t skipped_lanes = 0;
    if (carry_best_moves) {
      // With PAT a move's equity reads more of the board than its own lane,
      // so no lane is skipped or bounded. Hashed before
      // gen_seed_carried_best_move clears anchor counts, so that skipped
      // lanes keep their hashes.
      if (!gen_pat_is_active(gen) &&
          best_move_carry_skips_lanes(args->best_move_carry)) {
        gen_hash_lanes(gen, lane_hash_storage);
        lane_hashes = lane_hash_storage;
      }
      skipped_lanes = gen_seed_carried_best_move(
          gen, args->best_move_carry, lane_hashes, &gen->player_rack);
    }
    gen_shadow(gen);
    if (gen->threshold_exceeded) {
      gen_record_pass(gen);
      return;
    }
    gen_record_scoring_plays(gen);
    if (gen->wordsmog_pat != NULL && gen->wordsmog_pat->num_groups > 0) {
      wordsmog_pat_refine(gen);
    }
    if (carry_best_moves) {
      gen_remember_best_move(gen, args->best_move_carry, lane_hashes,
                             skipped_lanes, &gen->player_rack);
    }
  }
  gen_record_pass(gen);
}
