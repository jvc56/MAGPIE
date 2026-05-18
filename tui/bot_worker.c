#include "bot_worker.h"

#include "../src/compat/memory_info.h"
#include "../src/def/bai_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "game_render.h"
#include "game_state.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  SIM_CANDIDATES = 50,
  SIM_PLIES = 4,
  ENDGAME_PLIES = 25,
  // Untimed games still want a noticeable pause per turn so the player
  // can read the previous move; 5s gives the engine real work to do
  // without dragging.
  UNTIMED_BUDGET_SEC = 5,
  // Minimum effective budget so degenerate clock states don't make the
  // engine do almost-no-work and pick a near-random move.
  MIN_BUDGET_MS = 250,
  // The watchdog polls bot_stop while the engine is running and signals
  // the thread_control if a quit is requested.
  WATCHDOG_POLL_NS = 50 * 1000 * 1000L,
};

static void copy_str(char *dst, size_t dst_size, const char *src) {
  if (dst_size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

// Snapshot the on-turn player's rack into entry->rack_str before the
// engine has computed the move. The clock value reflects what the
// player had when the turn began.
//
// Caller must hold state->mutex.
static int append_pending_history(TuiGameState *state, int player_idx,
                                  const Rack *rack_at_start,
                                  int clock_at_start) {
  // If the user's history cursor was sitting on the previous last
  // entry, the new pending turn is "the current one" they were
  // watching — slide the cursor forward to keep it on the live
  // turn. If they parked the cursor on an older entry (or back at
  // the [4>] label) we leave it alone.
  const bool follow_to_new_pending =
      state->history_cursor >= 0 &&
      state->history_cursor == state->history_count - 1;
  if (state->history_count >= TUI_HISTORY_MAX) {
    // Ring-buffer eviction: drop the oldest entry's owned state
    // before we shift the array left so we don't leak anything.
    // Shift the cursor down by one too so it tracks the same
    // logical move; if it was on the evicted entry, it falls back
    // to the [4>] label.
    if (state->history[0].board_before != NULL) {
      board_destroy(state->history[0].board_before);
    }
    if (state->history[0].rack_before != NULL) {
      rack_destroy(state->history[0].rack_before);
    }
    if (state->history[0].sim_results_saved != NULL) {
      sim_results_destroy(state->history[0].sim_results_saved);
    }
    memmove(&state->history[0], &state->history[1],
            sizeof(state->history[0]) * (TUI_HISTORY_MAX - 1));
    state->history_count = TUI_HISTORY_MAX - 1;
    if (state->history_cursor >= 0) {
      state->history_cursor--; // -1 = label, which is fine if 0 → -1
    }
  }
  TuiHistoryEntry *entry = &state->history[state->history_count++];
  memset(entry, 0, sizeof(*entry));
  entry->player_idx = player_idx;
  entry->clock_at_start = clock_at_start;
  entry->pending = true;
  // Snapshot the board AND rack NOW (pre-move) so the History
  // cursor can replay the position this player saw at the moment
  // of decision. Both are owned by the entry; destroyed when the
  // entry is dropped or the game state is torn down. The rack is
  // the engine-friendly Rack object — analysis resumption needs
  // it; the text rack_str field on the entry is just for display.
  entry->board_before = board_duplicate(game_get_board(state->game));
  if (rack_at_start != NULL) {
    entry->rack_before = rack_duplicate(rack_at_start);
  } else {
    entry->rack_before = NULL;
  }
  if (follow_to_new_pending) {
    state->history_cursor = state->history_count - 1;
  }

  if (rack_at_start != NULL) {
    StringBuilder *rsb = string_builder_create();
    string_builder_add_rack(rsb, rack_at_start, state->ld, false);
    size_t rack_len = 0;
    char *rack_dump = string_builder_dump(rsb, &rack_len);
    copy_str(entry->rack_str, sizeof(entry->rack_str), rack_dump);
    free(rack_dump);
    string_builder_destroy(rsb);
  }
  return state->history_count - 1;
}

// Fill in move_str, score, total_after on an entry that was created
// pending. Clears the pending flag.
//
// Caller must hold state->mutex.
static void finalize_history(TuiGameState *state, int idx, const Move *move,
                             int score, int total_after, const Rack *leave) {
  if (idx < 0 || idx >= state->history_count) {
    return;
  }
  TuiHistoryEntry *entry = &state->history[idx];
  entry->score = score;
  entry->total_after = total_after;

  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(state->game), move, state->ld,
                          false);
  size_t move_len = 0;
  char *move_dump = string_builder_dump(sb, &move_len);
  copy_str(entry->move_str, sizeof(entry->move_str), move_dump);
  free(move_dump);
  string_builder_destroy(sb);

  // Compact exchanges to "-ABCD" form, matching the analysis panel.
  // The renderer (render_move_styled) already special-cases a leading
  // "-" so it renders non-bold like the legacy "(exch ...)" group.
  if (strncmp(entry->move_str, "(exch ", 6) == 0) {
    char *close_paren = strchr(entry->move_str, ')');
    if (close_paren != NULL) {
      const int letters_len = (int)(close_paren - (entry->move_str + 6));
      char tmp[sizeof(entry->move_str)];
      tmp[0] = '-';
      const int cap = (int)sizeof(tmp) - 2;
      const int copy = letters_len < cap ? letters_len : cap;
      memcpy(tmp + 1, entry->move_str + 6, (size_t)copy);
      tmp[1 + copy] = '\0';
      copy_str(entry->move_str, sizeof(entry->move_str), tmp);
    }
  }

  // Leave goes in its own right-aligned column rather than inline; the
  // renderer pulls it from entry->leave_str. Empty leaves (bingos /
  // outplays) leave the string empty and render as a "·" placeholder.
  entry->leave_str[0] = '\0';
  if (leave != NULL && !rack_is_empty(leave)) {
    StringBuilder *lsb = string_builder_create();
    string_builder_add_rack(lsb, leave, state->ld, false);
    size_t leave_len = 0;
    char *leave_dump = string_builder_dump(lsb, &leave_len);
    if (leave_dump != NULL) {
      copy_str(entry->leave_str, sizeof(entry->leave_str), leave_dump);
      free(leave_dump);
    }
    string_builder_destroy(lsb);
  }

  entry->pending = false;
}

// Drop the most recent history entry (used when computation is aborted
// before a move is selected).
//
// Caller must hold state->mutex.
static void pop_history(TuiGameState *state) {
  if (state->history_count > 0) {
    TuiHistoryEntry *entry = &state->history[state->history_count - 1];
    if (entry->board_before != NULL) {
      board_destroy(entry->board_before);
      entry->board_before = NULL;
    }
    if (entry->rack_before != NULL) {
      rack_destroy(entry->rack_before);
      entry->rack_before = NULL;
    }
    if (entry->sim_results_saved != NULL) {
      sim_results_destroy(entry->sim_results_saved);
      entry->sim_results_saved = NULL;
    }
    state->history_count--;
  }
}

// Budget for the on-turn player's current move. Strategy: estimate
// remaining plays in the game (assuming ~4 tiles per play, averaged
// across both players) and split the player's remaining clock evenly
// across them. For untimed games the budget collapses to a flat
// UNTIMED_BUDGET_SEC.
//
// Caller must hold state->mutex.
static double compute_budget_sec(const TuiGameState *state, int player_idx) {
  if (state->time_per_side_seconds <= 0) {
    return (double)UNTIMED_BUDGET_SEC;
  }
  const Bag *bag = game_get_bag(state->game);
  const int bag_tiles = bag_get_letters(bag);
  const int p1_rack =
      rack_get_total_letters(player_get_rack(game_get_player(state->game, 0)));
  const int p2_rack =
      rack_get_total_letters(player_get_rack(game_get_player(state->game, 1)));
  const int total_tiles = bag_tiles + p1_rack + p2_rack;
  // ~4 tiles per play means total_plays_remaining = total_tiles / 4
  // and the current player has roughly half of those. Round up so a
  // sparse remaining-tile count doesn't push n to 0.
  int plays_for_this_player = (total_tiles + 7) / 8;
  if (plays_for_this_player < 1) {
    plays_for_this_player = 1;
  }
  double remaining_clock =
      (double)state->time_per_side_seconds - state->seconds_used[player_idx];
  if (remaining_clock < 0.0) {
    remaining_clock = 0.0;
  }
  double budget = remaining_clock / (double)plays_for_this_player;
  if (budget < (double)MIN_BUDGET_MS / 1000.0) {
    budget = (double)MIN_BUDGET_MS / 1000.0;
  }
  return budget;
}

// Watchdog: spins while the engine is running and translates a
// bot_stop request into a thread_control USER_INTERRUPT so the engine
// can bail out early.
typedef struct {
  ThreadControl *tc;
  _Atomic bool *bot_stop;
  _Atomic bool finished;
} Watchdog;

static void *watchdog_main(void *arg) {
  Watchdog *w = (Watchdog *)arg;
  while (!atomic_load(&w->finished)) {
    if (atomic_load(w->bot_stop)) {
      thread_control_set_status(w->tc, THREAD_CONTROL_STATUS_USER_INTERRUPT);
      break;
    }
    struct timespec ts = {0, WATCHDOG_POLL_NS};
    nanosleep(&ts, NULL);
  }
  return NULL;
}

// Plain equity-best generation (the original behaviour). Used as a
// fallback when sim/endgame fail to return a move, or when WinPct
// wasn't loaded and bag still has tiles.
//
// `scratch` should be a MoveList created with capacity 1 (the standard
// MOVE_RECORD_BEST recorder).
static bool find_equity_best(const Game *game, MoveList *scratch,
                             Move *out_move) {
  const MoveGenArgs args = {
      .game = game,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = 0,
      .move_list = scratch,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves_for_game(&args);
  if (move_list_get_count(scratch) == 0) {
    return false;
  }
  move_copy(out_move, move_list_get_move(scratch, 0));
  return true;
}

// Generate up to N top candidates by equity into a fresh capacity-N
// MoveList. Returns the list (caller must destroy via
// moves_for_move_list_destroy + free) or NULL when no moves exist.
//
// We call generate_moves() directly rather than
// generate_moves_for_game(): the latter overrides move_record_type
// with the player's configured record type (MOVE_RECORD_BEST in our
// case), which collapses the candidate list to a single move.
static MoveList *generate_top_candidates(const Game *game, int n) {
  MoveList *list = move_list_create(n);
  const MoveGenArgs args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = 0,
      .move_list = list,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&args);
  if (move_list_get_count(list) == 0) {
    moves_for_move_list_destroy(list);
    free(list);
    return NULL;
  }
  return list;
}

// Run a Monte Carlo simulation against the current position with a
// time budget. Returns true and writes the chosen move on success.
// Falls back to equity-best on any internal failure.
static bool run_sim(TuiGameState *state, double budget_sec, Move *out_move) {
  MoveList *candidates = generate_top_candidates(state->game, SIM_CANDIDATES);
  if (candidates == NULL) {
    return false;
  }
  // Single-candidate position: no point simming a one-horse race.
  if (move_list_get_count(candidates) <= 1) {
    move_copy(out_move, move_list_get_move(candidates, 0));
    moves_for_move_list_destroy(candidates);
    free(candidates);
    return true;
  }

  ThreadControl *tc = thread_control_create();
  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

  Watchdog wd = {.tc = tc, .bot_stop = &state->bot_stop, .finished = false};
  pthread_t wd_thread;
  const bool wd_started =
      (pthread_create(&wd_thread, NULL, watchdog_main, &wd) == 0);

  const int num_threads = get_num_cores();
  SimArgs args = {0};
  args.num_plies = SIM_PLIES;
  args.move_list = candidates;
  args.num_plays = move_list_get_count(candidates);
  args.known_opp_rack = NULL;
  args.win_pcts = state->win_pcts;
  args.use_inference = false;
  args.use_heat_map = false;
  args.inference_results = NULL;
  args.num_threads = num_threads;
  args.print_interval = 0;
  args.max_num_display_plays = SIM_CANDIDATES;
  args.max_num_display_plies = SIM_PLIES;
  args.seed = (uint64_t)time(NULL);
  args.thread_control = tc;
  args.bai_options.sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS;
  args.bai_options.threshold = BAI_THRESHOLD_NONE;
  args.bai_options.sample_limit = (uint64_t)1e15;
  args.bai_options.sample_minimum = 1;
  args.bai_options.time_limit_seconds = (uint64_t)budget_sec;
  args.bai_options.num_threads = num_threads;
  args.bai_options.cutoff = 0.005;
  args.bai_options.parent_worker_thread_index = 0;
  args.bai_options.arm_avoid_prune = NULL;
  args.bai_options.num_arm_avoid_prune = 0;
  args.bai_options.delta = 1.0;
  // Game pointer must be a Game* (not const) when passed through the
  // SimArgs.game field; the sim doesn't mutate it but the struct's
  // declaration is `const Game *`.
  args.game = state->game;

  // The analysis panel reads state->sim_results in real time while the
  // sim is running. Flip the active flag and stamp the current turn
  // index BEFORE the simmer kicks off so the renderer doesn't show
  // stale data from a previous turn.
  atomic_store(&state->sim_results_turn_idx, state->history_count);
  atomic_store(&state->sim_results_active, true);

  SimResults *results = state->sim_results;
  ErrorStack *err = error_stack_create();
  simulate_without_ctx(&args, results, err);

  atomic_store(&wd.finished, true);
  if (wd_started) {
    pthread_join(wd_thread, NULL);
  }

  bool got_move = false;
  const Move *best = sim_results_get_best_move(results);
  if (best != NULL) {
    move_copy(out_move, best);
    got_move = true;
  } else {
    // Sim couldn't pick a winner — fall back to the top equity move
    // from the candidate list we already generated.
    move_copy(out_move, move_list_get_move(candidates, 0));
    got_move = true;
  }

  atomic_store(&state->sim_results_active, false);

  error_stack_destroy(err);
  thread_control_destroy(tc);
  moves_for_move_list_destroy(candidates);
  free(candidates);
  return got_move;
}

// Run the endgame solver against the current position with a time
// budget. Returns true and writes the chosen move on success.
// Build a TuiEndgameSnapshot from a freshly-computed PV array. Holds
// state->mutex for the duration and bumps render_version so the
// analysis panel picks up the change on the next frame. Called both
// per-ply (incremental from the engine's callback, ranked_pvs path)
// and post-solve (full multi_pvs leaderboard).
static void endgame_snapshot_from_pvs(TuiGameState *state, const PVLine *pvs,
                                      int num_pvs, int depth,
                                      int initial_spread, int solving_player,
                                      const Game *game, bool exhaustive) {
  pthread_mutex_lock(&state->mutex);
  tui_endgame_snapshot_clear(&state->endgame_snapshot);
  TuiEndgameSnapshot *snap = &state->endgame_snapshot;
  snap->board = board_duplicate(game_get_board(game));
  snap->solve_rack =
      rack_duplicate(player_get_rack(game_get_player(game, solving_player)));
  snap->initial_spread = initial_spread;
  snap->depth = depth;
  snap->solving_player = solving_player;
  snap->exhaustive = exhaustive;
  if (num_pvs > 0 && pvs != NULL) {
    snap->moves = (Move **)calloc((size_t)num_pvs, sizeof(Move *));
    snap->values = (int *)calloc((size_t)num_pvs, sizeof(int));
    if (snap->moves != NULL && snap->values != NULL) {
      int filled = 0;
      for (int i = 0; i < num_pvs; i++) {
        const PVLine *pvi = &pvs[i];
        if (pvi->num_moves <= 0) {
          continue;
        }
        Move *candidate = move_create();
        small_move_to_move(candidate, &pvi->moves[0], snap->board);
        // Dedupe on board-effect equivalence: same move_type, same
        // anchor + direction + tiles, regardless of move->score. The
        // engine's compare_moves_without_equity checks score first
        // which lets identical plays slip through when the re-search
        // updated one PV's score but not the other.
        bool duplicate = false;
        for (int j = 0; j < filled; j++) {
          const Move *other = snap->moves[j];
          if (candidate->move_type != other->move_type) {
            continue;
          }
          if (candidate->move_type == GAME_EVENT_PASS) {
            duplicate = true; // only one "pass" play, no further fields
            break;
          }
          if (candidate->row_start != other->row_start ||
              candidate->col_start != other->col_start ||
              candidate->dir != other->dir ||
              candidate->tiles_played != other->tiles_played ||
              candidate->tiles_length != other->tiles_length) {
            continue;
          }
          bool tiles_match = true;
          for (int t = 0; t < candidate->tiles_length; t++) {
            if (candidate->tiles[t] != other->tiles[t]) {
              tiles_match = false;
              break;
            }
          }
          if (tiles_match) {
            duplicate = true;
            break;
          }
        }
        if (duplicate) {
          move_destroy(candidate);
          continue;
        }
        snap->moves[filled] = candidate;
        snap->values[filled] = (int)pvi->score;
        filled++;
      }
      // Sort by value descending so the leaderboard reads top-down.
      // Simple insertion sort — num_entries is tiny.
      for (int i = 1; i < filled; i++) {
        Move *km = snap->moves[i];
        int kv = snap->values[i];
        int j = i - 1;
        while (j >= 0 && snap->values[j] < kv) {
          snap->moves[j + 1] = snap->moves[j];
          snap->values[j + 1] = snap->values[j];
          j--;
        }
        snap->moves[j + 1] = km;
        snap->values[j + 1] = kv;
      }
      snap->num_entries = filled;
      snap->valid = filled > 0;
    }
  }
  atomic_fetch_add(&state->render_version, 1);
  pthread_mutex_unlock(&state->mutex);
}

// Engine callback fired by the endgame solver after each iterative-
// deepening pass completes. user_data is the TuiGameState pointer set
// in EndgameArgs. The engine guarantees this only fires from worker
// thread index 0, so we can mutate the snapshot under state->mutex
// without worrying about concurrent calls.
static void endgame_per_ply_cb(int depth, int32_t value, const PVLine *pv_line,
                               const Game *game, const PVLine *ranked_pvs,
                               int num_ranked_pvs, void *user_data) {
  (void)value;
  (void)pv_line;
  TuiGameState *state = (TuiGameState *)user_data;
  if (state == NULL) {
    return;
  }
  // Throttle the snapshot rebuild to ~10 Hz. The rebuild duplicates
  // the whole Board (~29 KB) under state->mutex; firing it 25× per
  // ply iteration during a small endgame starves the main render
  // thread, which is what was causing the ~4 fps freeze when a game
  // hit endgame phase. The final solve still publishes a full
  // snapshot from run_endgame after the search returns, so the
  // displayed leaderboard converges either way.
  static _Atomic uint64_t last_ns;
  enum { THROTTLE_NS = 100 * 1000 * 1000 }; // 100 ms
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  const uint64_t now_ns =
      (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
  const uint64_t last = atomic_load(&last_ns);
  if (last != 0 && now_ns - last < THROTTLE_NS) {
    return;
  }
  atomic_store(&last_ns, now_ns);
  const int solving_player = atomic_load(&state->endgame_results_turn_idx) >= 0
                                 ? game_get_player_on_turn_index(game)
                                 : 0;
  const int initial_spread = atomic_load(&state->endgame_initial_spread);
  // Partial snapshot — search is still running, so not exhaustive
  // yet. Final snapshot in run_endgame may flip exhaustive=true once
  // endgame_solve returns with status FINISHED.
  endgame_snapshot_from_pvs(state, ranked_pvs, num_ranked_pvs, depth,
                            initial_spread, solving_player, game,
                            /*exhaustive=*/false);
}

// Engine callback fired once, before any worker spawns, with the
// initial root-move list sorted by static estimate. Gives us a d=0
// snapshot within sub-ms of endgame_solve start — the analysis panel
// can show *something* fresh before any negamax work has happened.
static void endgame_before_search_cb(const Game *game,
                                     const SmallMove *initial_moves,
                                     int initial_move_count, int initial_spread,
                                     int solving_player, void *user_data) {
  TuiGameState *state = (TuiGameState *)user_data;
  if (state == NULL || initial_moves == NULL || initial_move_count <= 0) {
    return;
  }
  // Build a temporary PVLine array on the stack so we can reuse
  // endgame_snapshot_from_pvs. Each PVLine wraps one SmallMove with
  // its current estimated value (in PVLine.score's spread-adjusted
  // sign convention: value - initial_spread).
  const int n = initial_move_count > MAX_ENDGAME_DISPLAY_PVS
                    ? MAX_ENDGAME_DISPLAY_PVS
                    : initial_move_count;
  PVLine pvs[MAX_ENDGAME_DISPLAY_PVS];
  for (int i = 0; i < n; i++) {
    pvs[i].moves[0] = initial_moves[i];
    pvs[i].num_moves = 1;
    pvs[i].negamax_depth = 0;
    pvs[i].game = NULL;
    pvs[i].score =
        small_move_get_estimated_value(&initial_moves[i]) - initial_spread;
  }
  endgame_snapshot_from_pvs(state, pvs, n, /*depth=*/0, initial_spread,
                            solving_player, game, /*exhaustive=*/false);
}

static bool run_endgame(TuiGameState *state, double budget_sec,
                        Move *out_move) {
  ThreadControl *tc = thread_control_create();
  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

  Watchdog wd = {.tc = tc, .bot_stop = &state->bot_stop, .finished = false};
  pthread_t wd_thread;
  const bool wd_started =
      (pthread_create(&wd_thread, NULL, watchdog_main, &wd) == 0);

  EndgameArgs args = {0};
  args.thread_control = tc;
  args.game = state->game;
  args.tt_fraction_of_mem = 0.10;
  args.plies = ENDGAME_PLIES;
  args.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  args.num_threads = get_num_cores();
  args.use_heuristics = true;
  // Ask the solver for a top-K leaderboard. Post-#530 the negamax
  // search uses topk_values[MAX_ENDGAME_DISPLAY_PVS], so that's the
  // hard ceiling.
  int top_k = SIM_CANDIDATES;
  if (top_k > MAX_ENDGAME_DISPLAY_PVS) {
    top_k = MAX_ENDGAME_DISPLAY_PVS;
  }
  args.num_top_moves = top_k;
  args.enable_pv_display = true;
  // Iterative deepening is REQUIRED for the per-ply callback to fire
  // incrementally. Without it thread 0 jumps straight to the
  // requested ply depth (25) and only the final post-solve update
  // ever lands in the snapshot — which is why the panel used to feel
  // stuck on stale data until the search fully completed.
  args.enable_iterative_deepening = true;
  // Live updates:
  //   before_search_callback fires once before any worker spawns,
  //     populating the snapshot with the d=0 leaderboard (root moves
  //     sorted by static estimate).
  //   per_ply_callback fires after each iterative-deepening pass
  //     completes, updating the snapshot with that depth's ranked
  //     leaderboard.
  args.before_search_callback = endgame_before_search_cb;
  args.before_search_callback_data = state;
  args.per_ply_callback = endgame_per_ply_cb;
  args.per_ply_callback_data = state;
  args.forced_pass_bypass = false;
  args.soft_time_limit = budget_sec * 0.9;
  args.hard_time_limit = budget_sec;
  args.seed = (uint64_t)time(NULL);

  // Capture the solver's initial spread so the renderer can show
  // W/T/L from the correct side once the bot has played and the
  // on-turn flag has flipped.
  const int solving_player = game_get_player_on_turn_index(state->game);
  const int p0_score =
      equity_to_int(player_get_score(game_get_player(state->game, 0)));
  const int p1_score =
      equity_to_int(player_get_score(game_get_player(state->game, 1)));
  const int initial_spread =
      (solving_player == 0) ? p0_score - p1_score : p1_score - p0_score;

  atomic_store(&state->endgame_initial_spread, initial_spread);
  atomic_store(&state->endgame_results_turn_idx, state->history_count);

  // Clear the previous turn's snapshot now so the renderer doesn't
  // keep showing it during the gap between activation and the engine's
  // first before_search_callback. The before_search_callback will
  // populate a fresh d=0 snapshot within sub-ms.
  pthread_mutex_lock(&state->mutex);
  tui_endgame_snapshot_clear(&state->endgame_snapshot);
  atomic_fetch_add(&state->render_version, 1);
  pthread_mutex_unlock(&state->mutex);

  atomic_store(&state->endgame_results_active, true);

  EndgameResults *results = state->endgame_results;
  ErrorStack *err = error_stack_create();
  // Reuse state->endgame_ctx across turns. endgame_solve allocates
  // on first call (*ctx == NULL) and reuses afterward — including
  // the transposition table, which is large enough (10% of system
  // memory by default) that recreating it every turn was the most
  // likely source of the SIGABRT we kept hitting under fragmentation.
  endgame_solve(&state->endgame_ctx, &args, results, err);

  atomic_store(&wd.finished, true);
  if (wd_started) {
    pthread_join(wd_thread, NULL);
  }

  bool got_move = false;
  const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  if (pv != NULL && pv->num_moves > 0) {
    small_move_to_move(out_move, &pv->moves[0], game_get_board(state->game));
    got_move = true;
  }

  // Finalize the snapshot with all multi-PVs from the completed
  // solve, and mark exhaustive when the search ran to completion
  // (no time interrupt). Per-ply callback updates above only see
  // ranked_pvs (capped at 10); this is the full leaderboard.
  const bool exhaustive =
      endgame_results_get_status(results) == ENDGAME_RESULT_STATUS_FINISHED;
  endgame_results_lock(results, ENDGAME_RESULT_DISPLAY);
  const int num_pvs = endgame_results_get_num_pvs(results);
  PVLine *multi = endgame_results_get_multi_pvs(results);
  endgame_snapshot_from_pvs(
      state, multi, num_pvs,
      endgame_results_get_depth(results, ENDGAME_RESULT_BEST), initial_spread,
      solving_player, state->game, exhaustive);
  endgame_results_unlock(results, ENDGAME_RESULT_DISPLAY);

  atomic_store(&state->endgame_results_active, false);

  // state->endgame_ctx is owned by the game state and freed in
  // tui_game_state_destroy — don't tear it down here.
  error_stack_destroy(err);
  thread_control_destroy(tc);
  return got_move;
}

static void *bot_thread_main(void *arg) {
  TuiGameState *state = (TuiGameState *)arg;
  // Scratch list for the equity-best fallback path.
  MoveList *fallback_list = move_list_create(1);
  // Scratch move buffer that engine routines write into. Lives across
  // turns so we don't churn the small-move arena on every turn.
  Move *chosen = move_create();

  while (!atomic_load(&state->bot_stop)) {
    // ── Snapshot turn state, append pending history ──────────────────
    int pending_idx = -1;
    bool empty_bag = false;
    double budget_sec = 0.0;
    int player_idx = -1;

    pthread_mutex_lock(&state->mutex);
    if (game_over(state->game)) {
      pthread_mutex_unlock(&state->mutex);
      break;
    }
    player_idx = game_get_player_on_turn_index(state->game);
    const int clock_at_start =
        state->time_per_side_seconds - (int)state->seconds_used[player_idx];

    Rack rack_at_start;
    rack_set_dist_size(&rack_at_start, ld_get_size(state->ld));
    rack_copy(&rack_at_start,
              player_get_rack(game_get_player(state->game, player_idx)));

    pending_idx = append_pending_history(state, player_idx, &rack_at_start,
                                         clock_at_start);
    budget_sec = compute_budget_sec(state, player_idx);
    empty_bag = (bag_get_letters(game_get_bag(state->game)) == 0);
    // Render the pending entry's spinner immediately.
    atomic_fetch_add(&state->render_version, 1);
    pthread_mutex_unlock(&state->mutex);

    // ── Run the engine with the mutex released ───────────────────────
    bool got_move = false;
    if (empty_bag) {
      got_move = run_endgame(state, budget_sec, chosen);
    } else if (state->win_pcts != NULL) {
      got_move = run_sim(state, budget_sec, chosen);
    }
    if (!got_move) {
      // Either we don't have win_pcts (no sim possible) or the engine
      // failed. Fall back to plain best-equity generation.
      pthread_mutex_lock(&state->mutex);
      got_move = find_equity_best(state->game, fallback_list, chosen);
      pthread_mutex_unlock(&state->mutex);
    }

    // ── Apply the move and finalize history ──────────────────────────
    pthread_mutex_lock(&state->mutex);
    if (atomic_load(&state->bot_stop) || game_over(state->game) || !got_move) {
      pop_history(state);
      atomic_fetch_add(&state->render_version, 1);
      pthread_mutex_unlock(&state->mutex);
      break;
    }

    const int move_score = equity_to_int(move_get_score(chosen));
    // Snapshot the Analysis-panel state BEFORE the move is played
    // (so the move-string formatting and leave column read off the
    // pre-move board + rack). The pending history entry already
    // exists at pending_idx; stash the snapshot into it so the
    // History cursor can replay this turn's analysis later.
    //
    // For sim turns ALSO deep-copy the live SimResults — every
    // Stat / PRNG / BAIResult / SimmedPlay — so a future "/resume"
    // can pick up iterating onto these samples rather than restart
    // from zero. The duplicate is owned by the entry; destroyed on
    // pop / reset / state-destroy.
    if (pending_idx >= 0 && pending_idx < state->history_count) {
      TuiHistoryEntry *finalized = &state->history[pending_idx];
      tui_capture_analysis_snapshot(state, &finalized->analysis_snapshot);
      if (finalized->analysis_snapshot.is_sim && state->sim_results != NULL) {
        if (finalized->sim_results_saved != NULL) {
          sim_results_destroy(finalized->sim_results_saved);
        }
        finalized->sim_results_saved =
            sim_results_duplicate(state->sim_results);
      }
    }
    Rack leave;
    rack_set_dist_size(&leave, ld_get_size(state->ld));
    play_move(chosen, state->game, &leave);

    // Tag the squares that got tiles placed on them with this
    // player's index so the renderer can colour them P1-green or
    // P2-amber later. Only happens here in the "live" game loop —
    // sim/endgame board copies leave the owner bits untouched.
    if (move_get_type(chosen) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      const int dir = move_get_dir(chosen);
      int r = move_get_row_start(chosen);
      int c = move_get_col_start(chosen);
      const int tile_count = move_get_tiles_length(chosen);
      for (int t = 0; t < tile_count; t++) {
        if (move_get_tile(chosen, t) != PLAYED_THROUGH_MARKER) {
          board_set_square_owner(game_get_board(state->game), r, c, player_idx);
        }
        if (board_is_dir_vertical(dir)) {
          r++;
        } else {
          c++;
        }
      }
    }

    const int post_play_score = equity_to_int(
        player_get_score(game_get_player(state->game, player_idx)));
    int bonus = 0;
    const Rack *opp_rack = NULL;
    if (game_over(state->game)) {
      opp_rack = player_get_rack(game_get_player(state->game, 1 - player_idx));
      if (!rack_is_empty(opp_rack)) {
        bonus = equity_to_int(calculate_end_rack_points(opp_rack, state->ld));
      }
    }
    const int total_after_move = post_play_score - bonus;
    finalize_history(state, pending_idx, chosen, move_score, total_after_move,
                     &leave);

    // Charge wall time used to the on-turn player. Clamp negative
    // deltas (clock skew / bad turn_started) to 0 so a single bad
    // reading can't poison the running total.
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (double)(now.tv_sec - state->turn_started.tv_sec) +
                     (double)(now.tv_nsec - state->turn_started.tv_nsec) / 1e9;
    if (elapsed < 0.0) {
      elapsed = 0.0;
    }
    state->seconds_used[player_idx] += elapsed;
    state->turn_started = now;

    // Surface end-of-game bonus on the same entry that just went out.
    bool finished = false;
    if (game_over(state->game)) {
      if (bonus != 0 && opp_rack != NULL && pending_idx >= 0 &&
          pending_idx < state->history_count) {
        TuiHistoryEntry *entry = &state->history[pending_idx];
        entry->end_bonus = bonus;
        StringBuilder *rsb = string_builder_create();
        string_builder_add_rack(rsb, opp_rack, state->ld, false);
        size_t rlen = 0;
        char *rdump = string_builder_dump(rsb, &rlen);
        copy_str(entry->end_rack_str, sizeof(entry->end_rack_str), rdump);
        free(rdump);
        string_builder_destroy(rsb);
      }
      finished = true;
    }

    atomic_fetch_add(&state->render_version, 1);
    pthread_mutex_unlock(&state->mutex);

    if (finished) {
      break;
    }
  }

  move_destroy(chosen);
  moves_for_move_list_destroy(fallback_list);
  free(fallback_list);
  return NULL;
}

void tui_bot_worker_start(TuiGameState *state) {
  if (state == NULL || state->bot_started) {
    return;
  }
  if (pthread_create(&state->bot_thread, NULL, bot_thread_main, state) == 0) {
    state->bot_started = true;
  }
}

void *tui_pixel_worker_main(void *arg);

void tui_pixel_worker_start(TuiGameState *state) {
  if (state == NULL || state->pixel_started) {
    return;
  }
  if (pthread_create(&state->pixel_thread, NULL, tui_pixel_worker_main,
                     state) == 0) {
    state->pixel_started = true;
  }
}
