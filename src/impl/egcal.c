#include "egcal.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/egcal_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/data_filepaths.h"
#include "../ent/egcal_table.h"
#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../ent/xoshiro.h"
#include "../util/string_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "move_gen.h"
#include <stdlib.h>
#include <string.h>

enum {
  CONSERVATION_TILE_WEIGHT = 7,
  CONSERVATION_VALUE_WEIGHT = 2,
};

// --- Shared data for worker coordination ---

typedef struct EgcalSharedData {
  int num_threads;
  int print_interval;
  Timer timer;
  uint64_t max_iter_count;
  uint64_t iter_count;
  cpthread_mutex_t iter_mutex;
  uint64_t iter_count_completed;
  cpthread_mutex_t iter_completed_mutex;
  ThreadControl *thread_control;
  XoshiroPRNG *prng;
} EgcalSharedData;

typedef struct EgcalIterOutput {
  uint64_t seed;
  uint64_t iter_count;
} EgcalIterOutput;

// --- Per-thread worker ---

typedef struct EgcalWorker {
  int worker_index;
  EgcalArgs args;
  EgcalSharedData *shared_data;
  Game *game;
  MoveList *move_list;
  EndgameSolver *solver;
  EndgameResults *endgame_results;
  ThreadControl *solver_thread_control; // per-worker, for endgame solver
  EgcalTable *local_greedy_table;       // (exact - greedy) per position
  EgcalTable *local_est_table;          // (exact - move_score) per move
  ErrorStack *error_stack;
} EgcalWorker;

// --- Iteration coordination ---

static bool egcal_get_next_iter(EgcalSharedData *shared_data,
                                EgcalIterOutput *output) {
  cpthread_mutex_lock(&shared_data->iter_mutex);
  if (shared_data->iter_count >= shared_data->max_iter_count) {
    cpthread_mutex_unlock(&shared_data->iter_mutex);
    return true;
  }
  output->seed = prng_next(shared_data->prng);
  output->iter_count = shared_data->iter_count;
  shared_data->iter_count++;
  cpthread_mutex_unlock(&shared_data->iter_mutex);
  return false;
}

static void egcal_complete_iter(EgcalSharedData *shared_data) {
  cpthread_mutex_lock(&shared_data->iter_completed_mutex);
  shared_data->iter_count_completed++;
  uint64_t completed = shared_data->iter_count_completed;
  cpthread_mutex_unlock(&shared_data->iter_completed_mutex);

  if (shared_data->print_interval > 0 &&
      completed % (uint64_t)shared_data->print_interval == 0) {
    double elapsed = ctimer_elapsed_seconds(&shared_data->timer);
    char *status = get_formatted_string(
        "egcal: %llu / %llu positions (%.1f sec)",
        (unsigned long long)completed,
        (unsigned long long)shared_data->max_iter_count, elapsed);
    thread_control_print(shared_data->thread_control, status);
    free(status);
  }
}

// --- Feature computation helpers ---

static int compute_max_tile_value(const Rack *rack,
                                  const LetterDistribution *ld) {
  int max_val = 0;
  int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    if (rack_get_letter(rack, ml) > 0) {
      int score = equity_to_int(ld_get_score(ld, ml));
      if (score > max_val) {
        max_val = score;
      }
    }
  }
  return max_val > 0 ? max_val : 1;
}

static int bucket_max_tile_value(int max_val) {
  if (max_val <= 3) {
    return 0;
  }
  if (max_val <= 6) {
    return 1;
  }
  return 2;
}

static int bucket_stuck_frac(float frac) {
  if (frac <= 0.0F) {
    return 0;
  }
  if (frac <= 0.25F) {
    return 1;
  }
  if (frac <= 0.5F) {
    return 2;
  }
  if (frac <= 0.75F) {
    return 3;
  }
  return 4;
}

static int bucket_num_legal_moves(int count) {
  if (count <= 1) {
    return 0;
  }
  if (count <= 5) {
    return 1;
  }
  if (count <= 15) {
    return 2;
  }
  if (count <= 50) {
    return 3;
  }
  if (count <= 200) {
    return 4;
  }
  return 5;
}

// --- Stuck fraction computation (standalone, no EndgameSolver dependency) ---

static float stuck_tile_fraction_from_bv(const LetterDistribution *ld,
                                         const Rack *rack,
                                         uint64_t tiles_played_bv) {
  int total_score = 0;
  int stuck_score = 0;
  int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    int count = rack_get_letter(rack, ml);
    if (count > 0) {
      int score = count * equity_to_int(ld_get_score(ld, ml));
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

static float egcal_compute_stuck_fraction(Game *game, MoveList *move_list,
                                          int player_idx, int thread_index) {
  int saved_on_turn = game_get_player_on_turn_index(game);
  if (saved_on_turn != player_idx) {
    game_set_player_on_turn_index(game, player_idx);
  }
  const Rack *rack = player_get_rack(game_get_player(game, player_idx));

  // Cross-set scan fast path
  uint64_t tiles_bv = 0;
  const Board *board = game_get_board(game);
  if (board_get_cross_sets_valid(board)) {
    bool kwgs_shared =
        game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
    int ci = board_get_cross_set_index(kwgs_shared, player_idx);
    const LetterDistribution *ld = game_get_ld(game);
    int ld_size = ld_get_size(ld);
    uint64_t rack_tiles_bv = 0;
    for (int ml = 0; ml < ld_size; ml++) {
      if (rack_get_letter(rack, ml) > 0) {
        rack_tiles_bv |= ((uint64_t)1 << ml);
      }
    }
    uint64_t rack_non_blank = rack_tiles_bv & ~(uint64_t)1;
    uint64_t playable_bv =
        board_get_playable_tiles_bv(board, ci, rack_non_blank);
    tiles_bv = playable_bv & rack_tiles_bv;
    if ((rack_tiles_bv & 1) && (playable_bv >> 1)) {
      tiles_bv |= 1;
    }
    bool all_playable = (tiles_bv == rack_tiles_bv);
    if (all_playable || rack_get_total_letters(rack) == 1) {
      float frac =
          all_playable ? 0.0F
                       : stuck_tile_fraction_from_bv(ld, rack, tiles_bv);
      if (saved_on_turn != player_idx) {
        game_set_player_on_turn_index(game, saved_on_turn);
      }
      return frac;
    }
  }

  // Full movegen fallback
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_TILES_PLAYED,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .thread_index = thread_index,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = &tiles_bv,
      .initial_tiles_bv = tiles_bv,
  };
  generate_moves(&gen_args);
  float result = stuck_tile_fraction_from_bv(game_get_ld(game), rack, tiles_bv);
  if (saved_on_turn != player_idx) {
    game_set_player_on_turn_index(game, saved_on_turn);
  }
  return result;
}

// --- Conservation bonus for Move API ---

static int compute_move_conservation_bonus(const Move *move,
                                           const LetterDistribution *ld,
                                           float opp_stuck_frac) {
  int tiles_played = move_get_tiles_played(move);
  int face_value = 0;
  int tiles_length = move_get_tiles_length(move);
  for (int tile_idx = 0; tile_idx < tiles_length; tile_idx++) {
    MachineLetter tile = move_get_tile(move, tile_idx);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    MachineLetter ml =
        get_is_blanked(tile) ? BLANK_MACHINE_LETTER : (tile & ~BLANK_MASK);
    face_value += equity_to_int(ld_get_score(ld, ml));
  }
  return (int)((float)(CONSERVATION_TILE_WEIGHT * tiles_played +
                       CONSERVATION_VALUE_WEIGHT * face_value) *
               opp_stuck_frac);
}

// --- Build chain computation adapted for Move API ---

static int *compute_move_build_chain_values(const MoveList *move_list,
                                            int move_count,
                                            float opp_stuck_frac) {
  if (opp_stuck_frac <= 0.0F || move_count <= 1) {
    return NULL;
  }

  int *build_values = malloc_or_die(move_count * sizeof(int));
  int *order = malloc_or_die(move_count * sizeof(int));
  for (int move_idx = 0; move_idx < move_count; move_idx++) {
    order[move_idx] = move_idx;
  }

  // Insertion sort by tiles_played descending
  for (int sort_idx = 1; sort_idx < move_count; sort_idx++) {
    int key = order[sort_idx];
    const Move *key_move = move_list_get_move(move_list, key);
    int tp_key = move_get_tiles_played(key_move);
    int insert_idx = sort_idx - 1;
    while (insert_idx >= 0) {
      const Move *cmp_move = move_list_get_move(move_list, order[insert_idx]);
      if (move_get_tiles_played(cmp_move) >= tp_key) {
        break;
      }
      order[insert_idx + 1] = order[insert_idx];
      insert_idx--;
    }
    order[insert_idx + 1] = key;
  }

  // Bottom-up: process moves from most tiles to fewest
  for (int order_idx = 0; order_idx < move_count; order_idx++) {
    int move_a_idx = order[order_idx];
    const Move *move_a = move_list_get_move(move_list, move_a_idx);
    build_values[move_a_idx] = equity_to_int(move_get_score(move_a));

    if (move_get_type(move_a) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }

    int dir_a = move_get_dir(move_a);
    int row_a = move_get_row_start(move_a);
    int col_a = move_get_col_start(move_a);
    int len_a = move_get_tiles_length(move_a);
    int tp_a = move_get_tiles_played(move_a);

    int best_extension = 0;
    for (int ext_idx = 0; ext_idx < order_idx; ext_idx++) {
      int move_b_idx = order[ext_idx];
      const Move *move_b = move_list_get_move(move_list, move_b_idx);

      if (move_get_type(move_b) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        continue;
      }
      if (move_get_tiles_played(move_b) <= tp_a) {
        continue;
      }
      if (move_get_dir(move_b) != dir_a) {
        continue;
      }

      int row_b = move_get_row_start(move_b);
      int col_b = move_get_col_start(move_b);
      int len_b = move_get_tiles_length(move_b);

      bool contained;
      if (dir_a == 0) {
        contained = (row_a == row_b) && (col_a >= col_b) &&
                    (col_a + len_a <= col_b + len_b);
      } else {
        contained = (col_a == col_b) && (row_a >= row_b) &&
                    (row_a + len_a <= row_b + len_b);
      }
      if (!contained) {
        continue;
      }

      int offset_in_b = (dir_a == 0) ? (col_a - col_b) : (row_a - row_b);
      bool tiles_match = true;
      for (int tile_idx = 0; tile_idx < len_a; tile_idx++) {
        MachineLetter tile_a = move_get_tile(move_a, tile_idx);
        if (tile_a == PLAYED_THROUGH_MARKER) {
          continue;
        }
        MachineLetter tile_b = move_get_tile(move_b, tile_idx + offset_in_b);
        if (tile_a != tile_b) {
          tiles_match = false;
          break;
        }
      }
      if (!tiles_match) {
        continue;
      }

      if (build_values[move_b_idx] > best_extension) {
        best_extension = build_values[move_b_idx];
      }
    }
    if (best_extension > 0) {
      build_values[move_a_idx] += best_extension;
    }
  }
  free(order);
  return build_values;
}

// --- Standalone greedy playout ---

// Plays greedily to completion on a duplicate game. Returns spread from
// the perspective of solving_player in millipoints.
static int32_t egcal_greedy_playout(Game *game, MoveList *move_list,
                                    int solving_player, int thread_index) {
  const LetterDistribution *ld = game_get_ld(game);
  int playout_depth = 0;

  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
         playout_depth < EGCAL_MAX_PLAYOUT_DEPTH) {
    int on_turn = game_get_player_on_turn_index(game);
    int opp = 1 - on_turn;

    // Compute opponent stuck fraction BEFORE generating moves for on_turn,
    // since both use the same move_list.
    float opp_stuck_frac = egcal_compute_stuck_fraction(game, move_list, opp,
                                                        thread_index);

    // Generate all moves for the player on turn
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = thread_index,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
    int move_count = move_list_get_count(move_list);

    // Select best move
    const Move *best_move = NULL;
    int best_adjusted = INT32_MIN;
    bool conserve = (opp_stuck_frac > 0.0F && on_turn == solving_player);

    int *build_values = NULL;
    if (conserve) {
      build_values =
          compute_move_build_chain_values(move_list, move_count, opp_stuck_frac);
    }

    for (int move_idx = 0; move_idx < move_count; move_idx++) {
      const Move *move = move_list_get_move(move_list, move_idx);
      int score = equity_to_int(move_get_score(move));
      int adjusted;

      if (conserve) {
        int conservation = compute_move_conservation_bonus(move, ld,
                                                           opp_stuck_frac);
        if (build_values) {
          // Prorate build chain boost by stuck fraction
          adjusted =
              score + (int)(opp_stuck_frac *
                            (float)(build_values[move_idx] - score)) -
              conservation;
        } else {
          adjusted = score - conservation;
        }
      } else {
        adjusted = score;
      }

      if (adjusted > best_adjusted) {
        best_adjusted = adjusted;
        best_move = move;
      }
    }
    free(build_values);

    play_move_without_drawing_tiles(best_move, game);
    playout_depth++;
  }

  // Compute spread from solving_player's perspective (in points)
  int score_solving =
      equity_to_int(player_get_score(game_get_player(game, solving_player)));
  int score_opp =
      equity_to_int(player_get_score(game_get_player(game, 1 - solving_player)));

  // If game didn't end naturally, add rack adjustments
  if (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    const Rack *rack_solving =
        player_get_rack(game_get_player(game, solving_player));
    const Rack *rack_opp =
        player_get_rack(game_get_player(game, 1 - solving_player));
    score_solving -= equity_to_int(rack_get_score(ld, rack_solving));
    score_opp -= equity_to_int(rack_get_score(ld, rack_opp));
  }

  return score_solving - score_opp;
}

// --- Position sampling ---

static void egcal_sample_position(EgcalWorker *worker, Game *sample_game) {
  const LetterDistribution *ld = game_get_ld(sample_game);
  int on_turn = game_get_player_on_turn_index(sample_game);
  int opp = 1 - on_turn;
  const Rack *rack_otk =
      player_get_rack(game_get_player(sample_game, on_turn));
  const Rack *rack_ott = player_get_rack(game_get_player(sample_game, opp));
  int tiles_otk = rack_get_total_letters(rack_otk);
  int tiles_ott = rack_get_total_letters(rack_ott);
  int total_tiles = tiles_otk + tiles_ott;

  if (total_tiles < EGCAL_TOTAL_TILES_MIN ||
      total_tiles > EGCAL_TOTAL_TILES_MAX || tiles_otk == 0 ||
      tiles_ott == 0) {
    return;
  }

  // Compute features
  int max_val_otk = compute_max_tile_value(rack_otk, ld);
  int max_val_ott = compute_max_tile_value(rack_ott, ld);

  // Ensure cross-sets are valid for stuck fraction computation
  game_gen_all_cross_sets(sample_game);

  float stuck_frac_otk = egcal_compute_stuck_fraction(
      sample_game, worker->move_list, on_turn, worker->worker_index);
  float stuck_frac_ott = egcal_compute_stuck_fraction(
      sample_game, worker->move_list, opp, worker->worker_index);

  // Generate moves to get legal move count
  const MoveGenArgs gen_args = {
      .game = sample_game,
      .move_list = worker->move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .thread_index = worker->worker_index,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
  int num_legal_moves = move_list_get_count(worker->move_list);

  // Run greedy playout on a copy
  Game *greedy_game = game_duplicate(sample_game);
  int32_t greedy_spread =
      egcal_greedy_playout(greedy_game, worker->move_list, on_turn,
                           worker->worker_index);
  game_destroy(greedy_game);

  // Run exact solver with a time limit to avoid spending minutes on deep
  // positions. Positions that don't complete within the limit are still
  // recorded with whatever partial result the solver found (IDS gives
  // progressively better estimates).
  endgame_results_reset(worker->endgame_results);
  EndgameArgs endgame_args = {0};
  endgame_args.game = sample_game;
  // Cap plies at 4 for tractable solve times. The greedy leaf playout
  // extends evaluation to full game depth, so 4-ply values are reasonable
  // even for positions with 14 total tiles.
  endgame_args.plies = total_tiles < 4 ? total_tiles : 4;
  endgame_args.num_threads = 1;
  endgame_args.tt_fraction_of_mem = 0.01;
  endgame_args.use_heuristics = true;
  endgame_args.thread_control = worker->solver_thread_control;
  endgame_args.num_top_moves = 1;
  endgame_args.thread_index_offset = worker->worker_index;
  endgame_args.hard_time_limit = 10.0; // 10 seconds per position

  endgame_solve(worker->solver, &endgame_args, worker->endgame_results,
                worker->error_stack);

  if (!error_stack_is_empty(worker->error_stack)) {
    // Skip this position on error
    error_stack_print_and_reset(worker->error_stack);
    return;
  }

  // exact_value is spread delta in points from on_turn perspective.
  int exact_delta =
      endgame_results_get_value(worker->endgame_results, ENDGAME_RESULT_BEST);

  // greedy_spread is absolute final spread in points. Convert to delta.
  int initial_spread =
      equity_to_int(player_get_score(game_get_player(sample_game, on_turn)) -
                    player_get_score(game_get_player(sample_game, opp)));
  int greedy_delta = greedy_spread - initial_spread;

  int mv_otk_bucket = bucket_max_tile_value(max_val_otk);
  int mv_ott_bucket = bucket_max_tile_value(max_val_ott);
  int sf_otk_bucket = bucket_stuck_frac(stuck_frac_otk);
  int sf_ott_bucket = bucket_stuck_frac(stuck_frac_ott);
  int lm_bucket = bucket_num_legal_moves(num_legal_moves);

  // Greedy table: one observation per position (exact_delta - greedy_delta)
  egcal_table_add(worker->local_greedy_table, total_tiles, tiles_otk,
                  mv_otk_bucket, mv_ott_bucket, sf_otk_bucket, sf_ott_bucket,
                  lm_bucket, greedy_delta, exact_delta);

  // Estimated-value table: one observation per move. For each move, record
  // (exact_position_value - move_score). This OVERESTIMATES the error for
  // non-best moves (since their exact values < position value), making the
  // margins conservative (safe).
  // Re-generate moves since the greedy playout may have used the move_list.
  generate_moves(&gen_args);
  int re_move_count = move_list_get_count(worker->move_list);
  for (int move_idx = 0; move_idx < re_move_count; move_idx++) {
    const Move *move = move_list_get_move(worker->move_list, move_idx);
    int move_score = equity_to_int(move_get_score(move));
    egcal_table_add(worker->local_est_table, total_tiles, tiles_otk,
                    mv_otk_bucket, mv_ott_bucket, sf_otk_bucket, sf_ott_bucket,
                    lm_bucket, move_score, exact_delta);
  }
}

// --- Worker loop ---

static void *egcal_worker_loop(void *arg) {
  EgcalWorker *worker = (EgcalWorker *)arg;
  EgcalSharedData *shared_data = worker->shared_data;
  EgcalIterOutput iter_output;

  while (thread_control_get_status(shared_data->thread_control) !=
             THREAD_CONTROL_STATUS_USER_INTERRUPT &&
         !egcal_get_next_iter(shared_data, &iter_output)) {
    // Play a game to the endgame (bag empty)
    game_reset(worker->game);
    game_seed(worker->game, iter_output.seed);
    draw_starting_racks(worker->game);

    while (!game_over(worker->game) &&
           !bag_is_empty(game_get_bag(worker->game))) {
      const Move *move = get_top_equity_move(worker->game,
                                              worker->worker_index,
                                              worker->move_list);
      play_move(move, worker->game, NULL);
    }

    // Skip if game ended before bag empty or bag still has tiles
    if (!bag_is_empty(game_get_bag(worker->game)) ||
        game_over(worker->game)) {
      egcal_complete_iter(shared_data);
      continue;
    }

    // Sample the endgame starting position and sub-positions
    Game *sample_game = game_duplicate(worker->game);

    int prev_total = 0;
    while (game_get_game_end_reason(sample_game) == GAME_END_REASON_NONE) {
      int on_turn = game_get_player_on_turn_index(sample_game);
      int opp = 1 - on_turn;
      int tiles_otk =
          rack_get_total_letters(
              player_get_rack(game_get_player(sample_game, on_turn)));
      int tiles_ott =
          rack_get_total_letters(
              player_get_rack(game_get_player(sample_game, opp)));
      int total = tiles_otk + tiles_ott;

      if (total < EGCAL_TOTAL_TILES_MIN) {
        break;
      }

      // Avoid sampling the same total_tiles twice in a row (e.g., after a
      // pass). Skip exact solving for positions with too many tiles — the
      // solver can spend minutes on deep positions.
      if (total != prev_total) {
        egcal_sample_position(worker, sample_game);
        prev_total = total;
      }

      // Check for interrupt
      if (thread_control_get_status(shared_data->thread_control) ==
          THREAD_CONTROL_STATUS_USER_INTERRUPT) {
        break;
      }

      // Play one greedy move to reduce tile count
      game_gen_all_cross_sets(sample_game);
      const MoveGenArgs gen_args = {
          .game = sample_game,
          .move_list = worker->move_list,
          .move_record_type = MOVE_RECORD_BEST,
          .move_sort_type = MOVE_SORT_SCORE,
          .override_kwg = NULL,
          .thread_index = worker->worker_index,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&gen_args);
      const Move *step_move = move_list_get_move(worker->move_list, 0);
      play_move_without_drawing_tiles(step_move, sample_game);
    }

    game_destroy(sample_game);
    egcal_complete_iter(shared_data);
  }
  return NULL;
}

// --- Worker create/destroy ---

static EgcalWorker *egcal_worker_create(const EgcalArgs *args,
                                         EgcalSharedData *shared_data,
                                         int worker_index) {
  EgcalWorker *worker = malloc_or_die(sizeof(EgcalWorker));
  worker->worker_index = worker_index;
  worker->args = *args;
  worker->shared_data = shared_data;
  worker->game = game_create(args->game_args);
  worker->move_list = move_list_create(500);
  worker->solver = endgame_solver_create();
  worker->endgame_results = endgame_results_create();
  worker->solver_thread_control = thread_control_create();
  worker->local_greedy_table = egcal_table_create();
  worker->local_est_table = egcal_table_create();
  worker->error_stack = error_stack_create();
  return worker;
}

static void egcal_worker_destroy(EgcalWorker *worker) {
  if (!worker) {
    return;
  }
  game_destroy(worker->game);
  move_list_destroy(worker->move_list);
  endgame_solver_destroy(worker->solver);
  endgame_results_destroy(worker->endgame_results);
  thread_control_destroy(worker->solver_thread_control);
  egcal_table_destroy(worker->local_greedy_table);
  egcal_table_destroy(worker->local_est_table);
  error_stack_destroy(worker->error_stack);
  free(worker);
}

// --- Summary output ---

static void egcal_print_summary(const EgcalTable *table,
                                const EgcalSharedData *shared_data) {
  StringBuilder *sb = string_builder_create();
  double elapsed = ctimer_elapsed_seconds(&shared_data->timer);
  int total_obs = egcal_table_get_total_observations(table);
  int populated_bins = egcal_table_get_populated_bin_count(table);

  string_builder_add_formatted_string(
      sb, "\negcal complete: %d observations in %d bins (%.1f sec)\n\n",
      total_obs, populated_bins, elapsed);

  string_builder_add_string(
      sb,
      "tiles | count  |  mean  |  p90   |  p99   | p99.9  | p99.99\n");
  string_builder_add_string(
      sb,
      "------+--------+--------+--------+--------+--------+--------\n");

  for (int tile_count = EGCAL_TOTAL_TILES_MIN;
       tile_count <= EGCAL_TOTAL_TILES_MAX; tile_count++) {
    int32_t mean;
    int32_t percentiles[EGCAL_NUM_PERCENTILES];
    uint32_t count =
        egcal_table_get_tile_count_stats(table, tile_count, &mean, percentiles);
    if (count == 0) {
      continue;
    }
    // Values are in points (not millipoints)
    string_builder_add_formatted_string(
        sb, "  %2d  | %6u | %+5d | %+5d | %+5d | %+5d | %+5d\n", tile_count,
        count, mean, percentiles[0], percentiles[2], percentiles[4],
        percentiles[6]);
  }

  thread_control_print(shared_data->thread_control, string_builder_peek(sb));
  string_builder_destroy(sb);
}

// --- Main entry point ---

void egcal(const EgcalArgs *args, ErrorStack *error_stack) {
  // Create shared data
  EgcalSharedData shared_data;
  shared_data.num_threads = args->num_threads;
  shared_data.print_interval = args->print_interval;
  shared_data.max_iter_count = args->num_positions;
  shared_data.iter_count = 0;
  shared_data.iter_count_completed = 0;
  shared_data.thread_control = args->thread_control;
  shared_data.prng = prng_create(args->seed);
  cpthread_mutex_init(&shared_data.iter_mutex);
  cpthread_mutex_init(&shared_data.iter_completed_mutex);
  ctimer_start(&shared_data.timer);

  // Create and spawn workers
  EgcalWorker **workers =
      malloc_or_die(sizeof(EgcalWorker *) * (size_t)args->num_threads);
  cpthread_t *worker_ids =
      malloc_or_die(sizeof(cpthread_t) * (size_t)args->num_threads);

  for (int thread_idx = 0; thread_idx < args->num_threads; thread_idx++) {
    workers[thread_idx] =
        egcal_worker_create(args, &shared_data, thread_idx);
    cpthread_create(&worker_ids[thread_idx], egcal_worker_loop,
                    workers[thread_idx]);
  }

  // Wait for all threads
  for (int thread_idx = 0; thread_idx < args->num_threads; thread_idx++) {
    cpthread_join(worker_ids[thread_idx]);
  }

  // Merge per-thread tables
  EgcalTable *merged_greedy = egcal_table_create();
  EgcalTable *merged_est = egcal_table_create();
  for (int thread_idx = 0; thread_idx < args->num_threads; thread_idx++) {
    egcal_table_merge(merged_greedy, workers[thread_idx]->local_greedy_table);
    egcal_table_merge(merged_est, workers[thread_idx]->local_est_table);
  }

  // Finalize
  egcal_table_finalize(merged_greedy);
  egcal_table_finalize(merged_est);

  // Write output files: greedy (.egcal) and estimated (.egcal_est)
  char *greedy_filename = data_filepaths_get_writable_filename(
      args->data_paths, args->lexicon_name, DATA_FILEPATH_TYPE_EGCAL,
      error_stack);
  if (!error_stack_is_empty(error_stack)) {
    egcal_table_destroy(merged_greedy);
    egcal_table_destroy(merged_est);
    goto cleanup;
  }
  egcal_table_write(merged_greedy, greedy_filename, error_stack);
  free(greedy_filename);

  // Write est table with _est suffix
  char *est_name = get_formatted_string("%s_est", args->lexicon_name);
  char *est_filename = data_filepaths_get_writable_filename(
      args->data_paths, est_name, DATA_FILEPATH_TYPE_EGCAL, error_stack);
  free(est_name);
  if (error_stack_is_empty(error_stack)) {
    egcal_table_write(merged_est, est_filename, error_stack);
    free(est_filename);
  }

  // Print summaries
  {
    char *header = get_formatted_string("\n--- Greedy playout calibration ---");
    thread_control_print(shared_data.thread_control, header);
    free(header);
  }
  egcal_print_summary(merged_greedy, &shared_data);
  {
    char *header = get_formatted_string("\n--- Estimated value calibration ---");
    thread_control_print(shared_data.thread_control, header);
    free(header);
  }
  egcal_print_summary(merged_est, &shared_data);
  egcal_table_destroy(merged_greedy);
  egcal_table_destroy(merged_est);

cleanup:
  for (int thread_idx = 0; thread_idx < args->num_threads; thread_idx++) {
    egcal_worker_destroy(workers[thread_idx]);
  }
  free(workers);
  free(worker_ids);
  prng_destroy(shared_data.prng);
}
