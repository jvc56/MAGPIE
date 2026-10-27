#include "peg_net_test.h"

#include "../src/compat/cfile.h"
#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/compat/endian_io.h"
#include "../src/def/bai_defs.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/sim_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/bag.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/encoded_rack.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_timer.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/transposition_table.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/value_net_history.h"
#include "../src/ent/win_pct.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/random_variable.h"
#include "../src/impl/simmer.h"
#include "../src/impl/value_net_features.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Creates path for writing (mode "w" or "wb"); dies if it exists or cannot be
// created, as C11's "x" modes do.
static FILE *vnt_fopen_new_or_die(const char *path, const char *mode) {
  FILE *stream = cfile_open(path, mode, true);
  if (stream == NULL) {
    log_fatal("failed to create file '%s': %s", path, strerror(errno));
  }
  return stream;
}

enum {
  VNT_MAX_THROUGHPUT_THREADS = 32,
  VALUE_NET_PARITY_ROWS = 64,
  VALUE_NET_PARITY_REPEATS = 20,
  VNT_MAX_TURNS = 256,
  // Distillation records (vnt_distill): the board's 0/1 planes as bits,
  // padded to a multiple of 8 bytes.
  VNT_DISTILL_BOARD_BYTES = ((VALUE_NET_BOARD_FLOATS + 63) / 64) * 8,
  VNT_DISTILL_CANDIDATES = 50,
  VNT_DISTILL_PROGRESS_GAMES = 200,
  // A pass's static equity in distillation records and exploration (its
  // equity is a sentinel, not points).
  VNT_DISTILL_PASS_EQUITY = -1000,
};

static uint64_t vnt_mix(uint64_t value) {
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

// The top static-equity move for the player on turn.
static const Move *vnt_static_move(const Game *game, MoveList *list) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  return move_list_get_move(list, 0);
}

// Fixed-depth match endgames have no timer and no transposition table. Each
// call owns its search state, so neither arm can inherit another arm's cache.
static int vnt_fixed_endgame(const Game *game, Move *move, int plies) {
  EndgameCtx *ctx = NULL;
  ThreadControl *control = thread_control_create();
  EndgameResults *results = endgame_results_create();
  EndgameArgs args = {
      .thread_control = control,
      .game = game,
      .plies = plies,
      .tt_fraction_of_mem = 0,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .incremental_movegen = true,
      .enable_pv_display = true,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
  };
  endgame_solve_inline(&ctx, &args, results);
  const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  const int completed_depth =
      endgame_results_get_depth(results, ENDGAME_RESULT_BEST);
  if (endgame_results_get_status(results) != ENDGAME_RESULT_STATUS_FINISHED ||
      completed_depth != plies || pv->num_moves < 1) {
    log_fatal("fixed-depth match endgame did not finish");
  }
  small_move_to_move(move, &pv->moves[0], game_get_board(game));
  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  thread_control_destroy(control);
  return completed_depth;
}

// Both definitions used by the existing endgame tools: tile-count fraction
// and rack-point fraction. The cross-set shortcut only returns early when
// authoritative; partial multi-tile results still require move generation.
static void vnt_eg_stuck(Game *game, MoveList *moves, int seat,
                         float *count_fraction, float *point_fraction) {
  const int saved = game_get_player_on_turn_index(game);
  game_set_player_on_turn_index(game, seat);
  const Rack *rack = player_get_rack(game_get_player(game, seat));
  const LetterDistribution *ld = game_get_ld(game);
  uint64_t rack_bits = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    if (rack_get_letter(rack, ml) > 0) {
      rack_bits |= UINT64_C(1) << ml;
    }
  }
  uint64_t played = 0;
  const Board *board = game_get_board(game);
  if (board_get_cross_sets_valid(board)) {
    const bool shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
    const int cross_idx = board_get_cross_set_index(shared, seat);
    const uint64_t playable =
        board_get_playable_tiles_bv(board, cross_idx, rack_bits & ~UINT64_C(1));
    played = playable & rack_bits;
    if ((rack_bits & 1) && (playable >> 1)) {
      played |= 1;
    }
  }
  if (played != rack_bits && (!board_get_cross_sets_valid(board) ||
                              rack_get_total_letters(rack) > 1)) {
    move_list_reset(moves);
    const MoveGenArgs args = {.game = game,
                              .move_list = moves,
                              .move_record_type = MOVE_RECORD_TILES_PLAYED,
                              .move_sort_type = MOVE_SORT_SCORE,
                              .override_kwg = NULL,
                              .eq_margin_movegen = 0,
                              .target_equity = EQUITY_MAX_VALUE,
                              .tiles_played_bv = &played,
                              .initial_tiles_bv = played};
    generate_moves(&args);
  }
  int total = 0, stuck = 0, points = 0, stuck_points = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const int count = rack_get_letter(rack, ml);
    const int score = count * ld_get_score(ld, ml);
    total += count;
    points += score;
    if (!(played & (UINT64_C(1) << ml))) {
      stuck += count;
      stuck_points += score;
    }
  }
  *count_fraction = total ? (float)stuck / total : 0;
  *point_fraction = points ? (float)stuck_points / points : 0;
  game_set_player_on_turn_index(game, saved);
}

enum {
  VNT_PEG1_POOL = 64,
  VNT_PEG1_EXTRA = 10,
  VNT_PEG1_INPUT = VALUE_NET_SCALARS + 4 * VNT_PEG1_EXTRA
};

static double vnt_peg1_margin(const Game *game, int mover) {
  return equity_to_double(player_get_score(game_get_player(game, mover)) -
                          player_get_score(game_get_player(game, 1 - mover)));
}

static int vnt_peg1_pool(const Game *game, MoveList *pool, Move *candidates,
                         int *pass_rank) {
  move_list_reset(pool);
  const MoveGenArgs args = {.game = game,
                            .move_list = pool,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  move_list_sort_moves(pool);
  *pass_rank = -1;
  int count = 0;
  for (int rank = 0; rank < move_list_get_count(pool); rank++) {
    const Move *move = move_list_get_move(pool, rank);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      *pass_rank = rank;
    } else if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      // Exchanges are never candidates; they are legal only with a full
      // rack's worth of tiles in the bag.
      if (bag_get_letters(game_get_bag(game)) < RACK_SIZE) {
        log_fatal("peg1 unexpected exchange");
      }
    } else if (count < VNT_PEG1_POOL) {
      move_copy(&candidates[count++], move);
    }
  }
  return count;
}

// Construct a hypothetical world from PUBLIC unseen counts. This is only
// for feature aggregation and counterfactual targets, never to reveal the
// rollout's actual hidden tile to the move picker.
static void vnt_peg1_world(const Game *root, const int *unseen, int tile,
                           Game *scenario) {
  // The racks and bag are set directly rather than by returning the hidden
  // rack to the bag and redrawing: a bag laid out by bag_set_to_tiles (PEG
  // scenarios, 2PEG worlds) cannot take a returned tile on player 1's side.
  const int mover = game_get_player_on_turn_index(root);
  const int letters = ld_get_size(game_get_ld(root));
  game_copy(scenario, root);
  Rack *opponent = player_get_rack(game_get_player(scenario, 1 - mover));
  rack_reset(opponent);
  for (int ml = 0; ml < letters; ml++) {
    for (int copy = 0; copy < unseen[ml] - (ml == tile); copy++) {
      rack_add_letter(opponent, (MachineLetter)ml);
    }
  }
  const MachineLetter last = (MachineLetter)tile;
  bag_set_to_tiles(game_get_bag(scenario), &last, 1);
  if (unseen[tile] < 1 || rack_get_total_letters(opponent) != RACK_SIZE) {
    log_fatal("peg1 invalid public scenario");
  }
}

static void vnt_peg1_extras(Game *after, int mover, MoveList *stuck,
                            float *extra, double *cheap_ms, double *own_ms,
                            double *opp_ms, bool compute_own,
                            bool compute_opp) {
  const int64_t started = ctimer_monotonic_ns();
  memset(extra, 0, sizeof(float) * VNT_PEG1_EXTRA);
  const LetterDistribution *ld = game_get_ld(after);
  const Player *own = game_get_player(after, mover);
  const Player *opponent = game_get_player(after, 1 - mover);
  const Rack *own_rack = player_get_rack(own);
  const Rack *opp_rack = player_get_rack(opponent);
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const double points = equity_to_double(ld_get_score(ld, ml));
    extra[0] += rack_get_letter(own_rack, ml) * points;
    extra[1] += rack_get_letter(opp_rack, ml) * points;
  }
  extra[2] = rack_is_empty(own_rack) ? 0
                                     : equity_to_double(klv_get_leave_value(
                                           player_get_klv(own), own_rack));
  extra[3] = rack_is_empty(opp_rack) ? 0
                                     : equity_to_double(klv_get_leave_value(
                                           player_get_klv(opponent), opp_rack));
  extra[4] = rack_get_total_letters(own_rack);
  extra[5] = rack_get_total_letters(opp_rack);
  *cheap_ms = (double)(ctimer_monotonic_ns() - started) / 1e6;
  const int64_t own_start = ctimer_monotonic_ns();
  if (compute_own && !game_over(after)) {
    vnt_eg_stuck(after, stuck, mover, &extra[6], &extra[8]);
  }
  *own_ms = (double)(ctimer_monotonic_ns() - own_start) / 1e6;
  const int64_t opp_start = ctimer_monotonic_ns();
  if (compute_opp && !game_over(after)) {
    vnt_eg_stuck(after, stuck, 1 - mover, &extra[7], &extra[9]);
  }
  *opp_ms = (double)(ctimer_monotonic_ns() - opp_start) / 1e6;
}

static double vnt_peg1_label(Game *after, int mover, EndgameCtx **ctx,
                             EndgameResults *results, ThreadControl *control) {
  double margin = vnt_peg1_margin(after, mover);
  if (!game_over(after)) {
    const EndgameArgs args = {.game = after,
                              .thread_control = control,
                              .plies = 2,
                              .tt_fraction_of_mem = 0,
                              .initial_small_move_arena_size =
                                  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                              .num_threads = 1,
                              .use_heuristics = true,
                              .num_top_moves = 1,
                              .incremental_movegen = true,
                              .enable_pv_display = true,
                              .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT};
    endgame_solve_inline(ctx, &args, results);
    if (endgame_results_get_status(results) != ENDGAME_RESULT_STATUS_FINISHED ||
        endgame_results_get_depth(results, ENDGAME_RESULT_BEST) != 2) {
      log_fatal("peg1 incomplete depth2 target");
    }
    margin -= endgame_results_get_value(results, ENDGAME_RESULT_BEST);
  }
  return margin;
}

static void vnt_peg1_data(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1data needs count, offset, seed and prefix");
  }
  const int roots = atoi(string_splitter_get_item(fields, 1));
  const int offset = atoi(string_splitter_get_item(fields, 2));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *prefix = string_splitter_get_item(fields, 4);
  if (roots < 1 || offset < 0) {
    log_fatal("peg1 invalid count or offset");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true "
      "-wit true -rit true -ritmmap true -threads 1 "
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *scenario = game_duplicate(game);
  Game *hidden_swap = game_duplicate(game);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  MoveList *stuck = move_list_create(1);
  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  const char *suffixes[] = {"positions.csv", "labels.csv", "scenarios.csv",
                            "inputs.f32"};
  FILE *files[4];
  for (int idx = 0; idx < 4; idx++) {
    char *path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    files[idx] = vnt_fopen_new_or_die(path, "wb");
    free(path);
  }
  fprintf(files[0], "root,source_seed,source_attempt,turn,seat,bag,first_row,"
                    "rows,pass_rank,movegen_ms,cgp\n");
  fprintf(files[1], "row,root,candidate,candidates,after,static_equity,win,"
                    "margin,utility,scenario_count,feature_ms,cheap_ms,own_"
                    "stuck_ms,opp_stuck_ms,solve_ms,move\n");
  fprintf(files[2], "row,root,candidate,tile,multiplicity,depth,win,margin,own_"
                    "score,opp_score,own_klv,opp_klv,own_count,opp_count,own_"
                    "stuck,opp_stuck,own_stuck_points,opp_stuck_points\n");
  StringBuilder *name = string_builder_create();
  uint64_t row = 0;
  for (int local = 0; local < roots; local++) {
    const int root_id = offset + local;
    ValueNetHistory histories[2];
    int source_turns = 0;
    uint64_t source_seed = 0;
    int attempt = 0;
    bool ready = false;
    Move candidates[VNT_PEG1_POOL];
    int count = 0;
    int pass_rank = -1;
    double movegen_ms = 0;
    while (!ready) {
      if (++attempt > 1000) {
        log_fatal("peg1 source generation failed");
      }
      source_seed = vnt_mix(seed ^ vnt_mix((uint64_t)root_id) ^
                            vnt_mix((uint64_t)attempt + UINT64_C(748238)));
      game_reset(game);
      game_seed(game, source_seed);
      game_set_starting_player_index(game, root_id % 2);
      draw_starting_racks(game);
      value_net_history_reset(&histories[0]);
      value_net_history_reset(&histories[1]);
      source_turns = 0;
      while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 1 &&
             source_turns < VNT_MAX_TURNS) {
        Move move;
        move_copy(&move, vnt_static_move(game, static_list));
        const int mover = game_get_player_on_turn_index(game);
        value_net_history_record_opponent_move(&histories[1 - mover], &move);
        play_move(&move, game, NULL);
        source_turns++;
      }
      if (!game_over(game) && bag_get_letters(game_get_bag(game)) == 1) {
        const int64_t start = ctimer_monotonic_ns();
        count = vnt_peg1_pool(game, pool, candidates, &pass_rank);
        movegen_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        ready = count > 0;
      }
    }
    const int mover = game_get_player_on_turn_index(game);
    int unseen[VALUE_NET_TILE_TYPES] = {0};
    int total = 0;
    const Rack *opponent = player_get_rack(game_get_player(game, 1 - mover));
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      unseen[ml] = rack_get_letter(opponent, ml) +
                   bag_get_letter(game_get_bag(game), (MachineLetter)ml);
      total += unseen[ml];
    }
    if (total != 8) {
      log_fatal("peg1 source does not have eight unseen tiles");
    }
    char *cgp = game_get_cgp(game, false);
    fprintf(files[0], "%d,%llu,%d,%d,%d,1,%llu,%d,%d,%.9g,\"%s\"\n", root_id,
            (unsigned long long)source_seed, attempt, source_turns, mover,
            (unsigned long long)row, count, pass_rank, movegen_ms, cgp);
    free(cgp);
    // Permute the PRIVATE split and require exactly identical public scalar
    // inputs and candidate ordering. All extra inputs below depend only on
    // those public counts and enumerated hypothetical worlds.
    int swapped_tile = 0;
    while (!unseen[swapped_tile]) {
      swapped_tile++;
    }
    vnt_peg1_world(game, unseen, swapped_tile, hidden_swap);
    Move permuted[VNT_PEG1_POOL];
    int swapped_pass;
    if (vnt_peg1_pool(hidden_swap, pool, permuted, &swapped_pass) != count ||
        swapped_pass != pass_rank) {
      log_fatal("peg1 private partition changes candidate pool");
    }
    for (int candidate = 0; candidate < count; candidate++, row++) {
      const Move *move = &candidates[candidate];
      if (compare_moves(move, &permuted[candidate], true) != -1) {
        log_fatal("peg1 private partition changes move order");
      }
      float inputs[VNT_PEG1_INPUT] = {0};
      float swapped[VALUE_NET_SCALARS];
      value_net_scalars_for_move(game, move, &histories[mover], inputs);
      value_net_scalars_for_move(hidden_swap, move, &histories[mover], swapped);
      if (memcmp(inputs, swapped, sizeof(swapped)) != 0) {
        log_fatal("peg1 private partition changes public scalars");
      }
      double sums[VNT_PEG1_EXTRA] = {0};
      double squared[VNT_PEG1_EXTRA] = {0};
      float low[VNT_PEG1_EXTRA], high[VNT_PEG1_EXTRA];
      for (int idx = 0; idx < VNT_PEG1_EXTRA; idx++) {
        low[idx] = INFINITY;
        high[idx] = -INFINITY;
      }
      double win_sum = 0, margin_sum = 0, utility_sum = 0;
      double feature_ms = 0, cheap_ms = 0, own_ms = 0, opp_ms = 0, solve_ms = 0;
      int scenarios = 0;
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (!unseen[tile]) {
          continue;
        }
        scenarios++;
        const int64_t feature_start = ctimer_monotonic_ns();
        vnt_peg1_world(game, unseen, tile, scenario);
        play_move(move, scenario, NULL);
        if (bag_get_letters(game_get_bag(scenario)) != 0 ||
            game_over(scenario)) {
          log_fatal("peg1 placement did not produce a nonterminal empty bag");
        }
        float extras[VNT_PEG1_EXTRA];
        double cheap, own, opp;
        vnt_peg1_extras(scenario, mover, stuck, extras, &cheap, &own, &opp,
                        true, true);
        feature_ms += (double)(ctimer_monotonic_ns() - feature_start) / 1e6;
        cheap_ms += cheap;
        own_ms += own;
        opp_ms += opp;
        const double probability = (double)unseen[tile] / total;
        for (int idx = 0; idx < VNT_PEG1_EXTRA; idx++) {
          sums[idx] += probability * extras[idx];
          squared[idx] += probability * extras[idx] * extras[idx];
          low[idx] = fminf(low[idx], extras[idx]);
          high[idx] = fmaxf(high[idx], extras[idx]);
        }
        const int64_t solve_start = ctimer_monotonic_ns();
        const double margin =
            vnt_peg1_label(scenario, mover, &ctx, results, control);
        solve_ms += (double)(ctimer_monotonic_ns() - solve_start) / 1e6;
        const double win = margin > 0 ? 1 : margin < 0 ? 0 : .5;
        const double utility = (win + .5 / (1 + exp(-margin / 100))) / 1.5;
        win_sum += probability * win;
        margin_sum += probability * margin;
        utility_sum += probability * utility;
        fprintf(files[2], "%llu,%d,%d,%d,%d,2,%.1f,%.9g",
                (unsigned long long)row, root_id, candidate, tile, unseen[tile],
                win, margin);
        for (int idx = 0; idx < VNT_PEG1_EXTRA; idx++) {
          fprintf(files[2], ",%.9g", extras[idx]);
        }
        fprintf(files[2], "\n");
      }
      for (int idx = 0; idx < VNT_PEG1_EXTRA; idx++) {
        inputs[VALUE_NET_SCALARS + idx] = sums[idx];
        inputs[VALUE_NET_SCALARS + VNT_PEG1_EXTRA + idx] =
            sqrt(fmax(0, squared[idx] - sums[idx] * sums[idx]));
        inputs[VALUE_NET_SCALARS + 2 * VNT_PEG1_EXTRA + idx] = low[idx];
        inputs[VALUE_NET_SCALARS + 3 * VNT_PEG1_EXTRA + idx] = high[idx];
      }
      if (!fwrite_le_floats(inputs, VNT_PEG1_INPUT, files[3])) {
        log_fatal("peg1 input write failed");
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(game), move,
                              game_get_ld(game), false);
      fprintf(files[1],
              "%llu,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%.9g,%.9g,%."
              "9g,\"%s\"\n",
              (unsigned long long)row, root_id, candidate, count,
              value_net_spread_after_move(game, move),
              equity_to_double(move_get_equity(move)), win_sum, margin_sum,
              utility_sum, scenarios, feature_ms, cheap_ms, own_ms, opp_ms,
              solve_ms, string_builder_peek(name));
    }
    for (int idx = 0; idx < 4; idx++) {
      fflush(files[idx]);
    }
    printf("peg1 root %d complete candidates %d\n", root_id, count);
    fflush(stdout);
  }
  for (int idx = 0; idx < 4; idx++) {
    fclose(files[idx]);
  }
  string_builder_destroy(name);
  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  thread_control_destroy(control);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  move_list_destroy(stuck);
  game_destroy(scenario);
  game_destroy(hidden_swap);
  config_destroy(config);
}

typedef struct VntPeg1Head {
  int width;
  bool extras;
  bool own_stuck;
  bool opp_stuck;
  float *blob;
  const float *first;
  const float *bias;
  const float *second;
  const float *out_bias;
} VntPeg1Head;

static VntPeg1Head *vnt_peg1_head_load(const char *path) {
  FILE *file = fopen_or_die(path, "rb");
  uint32_t shape[4];
  if (!fread_le_uint32s(shape, 4, file) || shape[0] != 0x50474131 ||
      shape[1] != VNT_PEG1_INPUT || shape[2] < 1 || shape[2] > 256 ||
      shape[3] != 4) {
    log_fatal("invalid aggregate1PEG head");
  }
  VntPeg1Head *head = calloc_or_die(1, sizeof(VntPeg1Head));
  head->width = (int)shape[2];
  const size_t count = head->width * (VNT_PEG1_INPUT + 5) + 4;
  uint32_t *bits = malloc_or_die(count * sizeof(uint32_t));
  head->blob = malloc_or_die(count * sizeof(float));
  if (!fread_le_uint32s(bits, count, file) || fgetc(file) != EOF) {
    log_fatal("invalid aggregate1PEG weights");
  }
  memcpy(head->blob, bits, count * sizeof(float));
  free(bits);
  fclose(file);
  head->first = head->blob;
  head->bias = head->first + head->width * VNT_PEG1_INPUT;
  head->second = head->bias + head->width;
  head->out_bias = head->second + 4 * head->width;
  for (int unit = 0; unit < head->width; unit++) {
    for (int col = VALUE_NET_SCALARS; col < VNT_PEG1_INPUT; col++) {
      if (head->first[unit * VNT_PEG1_INPUT + col] != 0) {
        head->extras = true;
        const int extra = (col - VALUE_NET_SCALARS) % VNT_PEG1_EXTRA;
        head->own_stuck |= extra == 6 || extra == 8;
        head->opp_stuck |= extra == 7 || extra == 9;
      }
    }
  }
  return head;
}

static void vnt_peg1_head_predict(const VntPeg1Head *head, const float *input,
                                  double after, float *raw, double *win,
                                  double *margin) {
  float activation[256];
  for (int unit = 0; unit < head->width; unit++) {
    float sum = head->bias[unit];
    for (int col = 0; col < VNT_PEG1_INPUT; col++) {
      sum += head->first[unit * VNT_PEG1_INPUT + col] * input[col];
    }
    activation[unit] = fmaxf(sum, 0);
  }
  for (int channel = 0; channel < 4; channel++) {
    float sum = head->out_bias[channel];
    for (int unit = 0; unit < head->width; unit++) {
      sum += head->second[channel * head->width + unit] * activation[unit];
    }
    raw[channel] = sum;
  }
  const double maximum = fmax(raw[0], fmax(raw[1], raw[2]));
  const double loss = exp(raw[0] - maximum);
  const double tie = exp(raw[1] - maximum);
  const double victory = exp(raw[2] - maximum);
  *win = (victory + .5 * tie) / (loss + tie + victory);
  *margin = after + 30 * raw[3];
}

static void vnt_peg1_public_inputs(const Game *root, const Move *move,
                                   const float *public_history,
                                   const VntPeg1Head *head, Game *scenario,
                                   MoveList *stuck, float *inputs) {
  memset(inputs, 0, sizeof(float) * VNT_PEG1_INPUT);
  ValueNetHistory history;
  value_net_history_reset(&history);
  value_net_scalars_for_move(root, move, &history, inputs);
  // These four candidate-invariant public-history scalars were saved during
  // the original self-play, and are not recoverable from a CGP alone.
  inputs[54] = public_history[54];
  inputs[55] = public_history[55];
  inputs[56] = public_history[56];
  inputs[67] = public_history[67];
  if (!head->extras) {
    return;
  }
  int unseen[VALUE_NET_TILE_TYPES];
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    unseen[ml] = (int)lroundf(inputs[27 + ml] * 100 * inputs[70]);
  }
  const int mover = game_get_player_on_turn_index(root);
  double sums[VNT_PEG1_EXTRA] = {0}, squared[VNT_PEG1_EXTRA] = {0};
  float low[VNT_PEG1_EXTRA], high[VNT_PEG1_EXTRA];
  for (int idx = 0; idx < VNT_PEG1_EXTRA; idx++) {
    low[idx] = INFINITY;
    high[idx] = -INFINITY;
  }
  for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
    if (unseen[tile] == 0) {
      continue;
    }
    vnt_peg1_world(root, unseen, tile, scenario);
    play_move(move, scenario, NULL);
    float extra[VNT_PEG1_EXTRA];
    double cheap, own, opp;
    vnt_peg1_extras(scenario, mover, stuck, extra, &cheap, &own, &opp,
                    head->own_stuck, head->opp_stuck);
    const double probability = unseen[tile] / 8.0;
    for (int idx = 0; idx < VNT_PEG1_EXTRA; idx++) {
      sums[idx] += probability * extra[idx];
      squared[idx] += probability * extra[idx] * extra[idx];
      low[idx] = fminf(low[idx], extra[idx]);
      high[idx] = fmaxf(high[idx], extra[idx]);
    }
  }
  for (int idx = 0; idx < VNT_PEG1_EXTRA; idx++) {
    inputs[VALUE_NET_SCALARS + idx] = sums[idx];
    inputs[VALUE_NET_SCALARS + VNT_PEG1_EXTRA + idx] =
        sqrt(fmax(0, squared[idx] - sums[idx] * sums[idx]));
    inputs[VALUE_NET_SCALARS + 2 * VNT_PEG1_EXTRA + idx] = low[idx];
    inputs[VALUE_NET_SCALARS + 3 * VNT_PEG1_EXTRA + idx] = high[idx];
  }
}

static void vnt_peg1_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1eval needs model, data prefix, output prefix, continuation "
              "depth0|2");
  }
  VntPeg1Head *head = vnt_peg1_head_load(string_splitter_get_item(fields, 1));
  const char *data = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const int depth = atoi(string_splitter_get_item(fields, 4));
  if (depth != 0 && depth != 2) {
    log_fatal("peg1eval invalid continuation");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *game = game_duplicate(root), *scenario = game_duplicate(root),
       *swapped = game_duplicate(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1),
           *stuck = move_list_create(1), *static_list = move_list_create(1);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  path = get_formatted_string("%s.inputs.f32", data);
  FILE *inputs_file = fopen_or_die(path, "rb");
  free(path);
  const char *suffixes[] = {"games.csv", "decisions.csv", "raw.f32"};
  FILE *output[3];
  for (int idx = 0; idx < 3; idx++) {
    path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    output[idx] = vnt_fopen_new_or_die(path, "wb");
    free(path);
  }
  fprintf(output[0],
          "root,game,net_seat,initial_spread,final_spread,win,chosen_candidate,"
          "actual_tile,turns,continuation_depth,ended\n");
  fprintf(output[1], "root,candidates,choice,pass_rank,movegen_ms,feature_ms,"
                     "head_ms,privacy_input_error\n");
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1eval invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1eval invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1eval CGP load failed");
    }
    float saved[VNT_PEG1_POOL * VNT_PEG1_INPUT];
    uint32_t bits[VNT_PEG1_POOL * VNT_PEG1_INPUT];
    if (!fread_le_uint32s(bits, expected * VNT_PEG1_INPUT, inputs_file)) {
      log_fatal("peg1eval cached input read failed");
    }
    memcpy(saved, bits, expected * VNT_PEG1_INPUT * sizeof(float));
    Move candidates[VNT_PEG1_POOL];
    int current_pass;
    const int64_t gen_start = ctimer_monotonic_ns();
    const int count = vnt_peg1_pool(root, pool, candidates, &current_pass);
    const double gen_ms = (double)(ctimer_monotonic_ns() - gen_start) / 1e6;
    if (count != expected || current_pass != pass_rank) {
      log_fatal("peg1eval candidate replay mismatch");
    }
    int unseen[VALUE_NET_TILE_TYPES];
    int actual_tile = -1, swapped_tile = -1;
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      unseen[ml] = (int)lroundf(saved[27 + ml] * 100 * saved[70]);
      if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
        actual_tile = ml;
      }
    }
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      if (unseen[ml] > 0 && ml != actual_tile) {
        swapped_tile = ml;
        break;
      }
    }
    if (swapped_tile < 0) {
      swapped_tile = actual_tile;
    }
    vnt_peg1_world(root, unseen, swapped_tile, swapped);
    double features_ms = 0, heads_ms = 0, best = -INFINITY;
    int choice = 0;
    for (int candidate = 0; candidate < count; candidate++) {
      float inputs[VNT_PEG1_INPUT], private_swap[VNT_PEG1_INPUT], raw[4];
      const int64_t feature_start = ctimer_monotonic_ns();
      vnt_peg1_public_inputs(root, &candidates[candidate], saved, head,
                             scenario, stuck, inputs);
      features_ms += (double)(ctimer_monotonic_ns() - feature_start) / 1e6;
      vnt_peg1_public_inputs(swapped, &candidates[candidate], saved, head,
                             scenario, stuck, private_swap);
      if (memcmp(inputs, private_swap, sizeof(inputs)) != 0) {
        log_fatal("peg1 policy input depends on actual hidden tile");
      }
      double win, margin;
      const int64_t head_start = ctimer_monotonic_ns();
      vnt_peg1_head_predict(
          head, inputs,
          value_net_spread_after_move(root, &candidates[candidate]), raw, &win,
          &margin);
      const double score = (win + .5 / (1 + exp(-margin / 100))) / 1.5;
      heads_ms += (double)(ctimer_monotonic_ns() - head_start) / 1e6;
      if (!fwrite_le_floats(raw, 4, output[2])) {
        log_fatal("peg1eval prediction write failed");
      }
      if (score > best) {
        best = score;
        choice = candidate;
      }
    }
    fprintf(output[1], "%d,%d,%d,%d,%.9g,%.9g,%.9g,0\n", root_id, count, choice,
            pass_rank, gen_ms, features_ms, heads_ms);
    // Choose without knowledge of actual_tile. Only the real play below
    // consumes it; all subsequent endgame decisions use then-public racks.
    for (int pair_game = 0; pair_game < 2; pair_game++) {
      const int net_seat = pair_game;
      const int picked = seat == net_seat ? choice : 0;
      game_copy(game, root);
      const double initial = vnt_peg1_margin(game, net_seat);
      play_move(&candidates[picked], game, NULL);
      if (bag_get_letters(game_get_bag(game)) != 0) {
        log_fatal("peg1eval actual draw not consumed");
      }
      int turns = 1;
      while (!game_over(game) && turns < VNT_MAX_TURNS) {
        Move move;
        if (depth == 2) {
          vnt_fixed_endgame(game, &move, 2);
        } else {
          move_copy(&move, vnt_static_move(game, static_list));
        }
        play_move(&move, game, NULL);
        turns++;
      }
      if (!game_over(game)) {
        log_fatal("peg1eval continuation did not reach game over");
      }
      const double final = vnt_peg1_margin(game, net_seat);
      fprintf(output[0], "%d,%d,%d,%.9g,%.9g,%.1f,%d,%d,%d,%d,1\n", root_id,
              pair_game, net_seat, initial, final,
              final > 0   ? 1.0
              : final < 0 ? 0.0
                          : .5,
              picked, actual_tile, turns, depth);
    }
    for (int idx = 0; idx < 3; idx++) {
      fflush(output[idx]);
    }
  }
  free(line);
  fclose(positions);
  fclose(inputs_file);
  for (int idx = 0; idx < 3; idx++) {
    fclose(output[idx]);
  }
  error_stack_destroy(errors);
  move_list_destroy(pool);
  move_list_destroy(stuck);
  move_list_destroy(static_list);
  game_destroy(game);
  game_destroy(scenario);
  game_destroy(swapped);
  config_destroy(config);
  free(head->blob);
  free(head);
}

// Reply features for stored 1PEG roots. For every candidate and every public
// hypothetical last tile, the world is built from public unseen counts only;
// the actual private split never enters these features.
enum {
  VNT_PEG1_REPLY = 10,
  VNT_PEG1_REPLY_HEAD = 3,
  VNT_PEG1_REPLY_ROW =
      VNT_PEG1_REPLY_HEAD + VALUE_NET_TILE_TYPES * VNT_PEG1_REPLY,
  VNT_PEG1_PLAYOUT_CAP = 32,
};

// The side on turn's greedy endgame choice: highest score, plus twice the
// opponent's rack when going out. Returns the greedy value; pass when no
// placement exists.
static int vnt_peg1_greedy(Game *game, MoveList *small, Move *move, bool *out,
                           int *tiles, bool *pass) {
  const int stm = game_get_player_on_turn_index(game);
  const LetterDistribution *ld = game_get_ld(game);
  const Rack *own = player_get_rack(game_get_player(game, stm));
  const Rack *opp = player_get_rack(game_get_player(game, 1 - stm));
  const Equity bonus = calculate_end_rack_points(opp, ld);
  const MoveGenArgs args = {.game = game,
                            .move_list = small,
                            .move_record_type = MOVE_RECORD_BEST_SMALL,
                            .move_sort_type = MOVE_SORT_SCORE,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .best_small_out_bonus = bonus};
  generate_moves(&args);
  const SmallMove *best = small->small_moves[0];
  if (small->count == 0 || small_move_is_pass(best)) {
    *out = false;
    *tiles = 0;
    *pass = true;
    move_set_as_pass(move);
    return 0;
  }
  *pass = false;
  *tiles = small_move_get_tiles_played(best);
  *out = *tiles == rack_get_total_letters(own);
  small_move_to_move(move, best, game_get_board(game));
  return small_move_get_score(best) + (*out ? equity_to_int(bonus) : 0);
}

static void vnt_peg1_reply_world(const Game *scenario, int mover, Game *work,
                                 MoveList *small, Move *move, float *feature,
                                 double *timing) {
  bool out, pass;
  int tiles;
  // Mover's immediate greedy value on the post-candidate board.
  int64_t start = ctimer_monotonic_ns();
  game_copy(work, scenario);
  game_set_player_on_turn_index(work, mover);
  const int now = vnt_peg1_greedy(work, small, move, &out, &tiles, &pass);
  feature[3] = (float)now;
  feature[4] = out;
  timing[1] += (double)(ctimer_monotonic_ns() - start) / 1e6;
  // Greedy playout from the opponent's reply to game over.
  start = ctimer_monotonic_ns();
  game_copy(work, scenario);
  int moves = 0;
  while (!game_over(work) && moves < VNT_PEG1_PLAYOUT_CAP) {
    const int value = vnt_peg1_greedy(work, small, move, &out, &tiles, &pass);
    if (moves == 0) {
      feature[0] = (float)value;
      feature[1] = out;
      feature[2] = (float)tiles;
      feature[9] = pass;
      timing[2] += (double)(ctimer_monotonic_ns() - start) / 1e6;
    } else if (moves == 1) {
      feature[5] = (float)value;
      feature[6] = out;
    }
    play_move(move, work, NULL);
    moves++;
  }
  if (!game_over(work)) {
    log_fatal("peg1feat greedy playout did not end");
  }
  feature[7] = (float)vnt_peg1_margin(work, mover);
  feature[8] = (float)moves;
  timing[3] += (double)(ctimer_monotonic_ns() - start) / 1e6;
}

static void vnt_peg1_features(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 4) {
    log_fatal("peg1feat needs data prefix, output prefix and inputs width");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  const int width = atoi(string_splitter_get_item(fields, 3));
  if (width != VNT_PEG1_INPUT) {
    log_fatal("peg1feat unexpected stored input width");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *scenario = game_duplicate(root), *work = game_duplicate(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *small = move_list_create_small(16);
  Move *move = move_create();
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  path = get_formatted_string("%s.inputs.f32", data);
  FILE *inputs_file = fopen_or_die(path, "rb");
  free(path);
  path = get_formatted_string("%s.reply.f32", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "wb");
  free(path);
  path = get_formatted_string("%s.reply-timing.csv", prefix);
  FILE *timing_file = vnt_fopen_new_or_die(path, "w");
  free(path);
  fprintf(timing_file,
          "root,candidates,worlds,movegen_ms,world_ms,mover_now_ms,"
          "opp_reply_ms,playout_ms\n");
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1feat invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1feat invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1feat CGP load failed");
    }
    float saved[VNT_PEG1_INPUT];
    uint32_t bits[VNT_PEG1_INPUT];
    for (int row = 0; row < expected; row++) {
      if (!fread_le_uint32s(bits, VNT_PEG1_INPUT, inputs_file)) {
        log_fatal("peg1feat cached input read failed");
      }
      if (row == 0) {
        memcpy(saved, bits, sizeof(saved));
      }
    }
    Move candidates[VNT_PEG1_POOL];
    int current_pass;
    const int64_t gen_start = ctimer_monotonic_ns();
    const int count = vnt_peg1_pool(root, pool, candidates, &current_pass);
    const double gen_ms = (double)(ctimer_monotonic_ns() - gen_start) / 1e6;
    if (count != expected || current_pass != pass_rank) {
      log_fatal("peg1feat candidate replay mismatch");
    }
    int unseen[VALUE_NET_TILE_TYPES];
    int worlds = 0;
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      unseen[ml] = (int)lroundf(saved[27 + ml] * 100 * saved[70]);
      worlds += unseen[ml] > 0;
    }
    const int mover = game_get_player_on_turn_index(root);
    double timing[4] = {0};
    for (int candidate = 0; candidate < count; candidate++) {
      const Move *cand = &candidates[candidate];
      float row[VNT_PEG1_REPLY_ROW] = {0};
      row[0] = (float)value_net_spread_after_move(root, cand);
      row[1] = (float)equity_to_double(move_get_equity(cand));
      row[2] = (float)move_get_tiles_played(cand);
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (unseen[tile] == 0) {
          continue;
        }
        const int64_t world_start = ctimer_monotonic_ns();
        vnt_peg1_world(root, unseen, tile, scenario);
        play_move(cand, scenario, NULL);
        if (bag_get_letters(game_get_bag(scenario)) != 0 ||
            game_over(scenario)) {
          log_fatal("peg1feat placement did not empty the bag");
        }
        timing[0] += (double)(ctimer_monotonic_ns() - world_start) / 1e6;
        vnt_peg1_reply_world(scenario, mover, work, small, move,
                             &row[VNT_PEG1_REPLY_HEAD + tile * VNT_PEG1_REPLY],
                             timing);
      }
      if (!fwrite_le_floats(row, VNT_PEG1_REPLY_ROW, output)) {
        log_fatal("peg1feat write failed");
      }
    }
    fprintf(timing_file, "%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g\n", root_id, count,
            worlds, gen_ms, timing[0], timing[1], timing[2], timing[3]);
    fflush(output);
    fflush(timing_file);
  }
  free(line);
  fclose(positions);
  fclose(inputs_file);
  fclose(output);
  fclose(timing_file);
  error_stack_destroy(errors);
  move_destroy(move);
  move_list_destroy(small);
  move_list_destroy(pool);
  game_destroy(scenario);
  game_destroy(work);
  config_destroy(config);
}

// Fresh 1PEG roots from static self-play (the peg1data source procedure),
// with public candidate scalars and per-world reply features but no labels.
static void vnt_peg1_roots(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1roots needs count, offset, seed and prefix");
  }
  const int roots = atoi(string_splitter_get_item(fields, 1));
  const int offset = atoi(string_splitter_get_item(fields, 2));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *prefix = string_splitter_get_item(fields, 4);
  if (roots < 1 || offset < 0) {
    log_fatal("peg1roots invalid count or offset");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true "
      "-wit true -rit true -ritmmap true -threads 1 "
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *scenario = game_duplicate(game), *work = game_duplicate(game);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  MoveList *small = move_list_create_small(16);
  Move *move = move_create();
  const char *suffixes[] = {"positions.csv", "candidates.csv", "scalars.f32",
                            "reply.f32", "reply-timing.csv"};
  FILE *files[5];
  for (int idx = 0; idx < 5; idx++) {
    char *path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    files[idx] = vnt_fopen_new_or_die(path, "wb");
    free(path);
  }
  fprintf(files[0], "root,source_seed,source_attempt,turn,seat,bag,first_row,"
                    "rows,pass_rank,movegen_ms,cgp\n");
  fprintf(files[1], "row,root,candidate,candidates,after,static_equity,tiles,"
                    "move\n");
  fprintf(files[4], "root,candidates,worlds,movegen_ms,world_ms,mover_now_ms,"
                    "opp_reply_ms,playout_ms\n");
  StringBuilder *name = string_builder_create();
  uint64_t row = 0;
  for (int local = 0; local < roots; local++) {
    const int root_id = offset + local;
    ValueNetHistory histories[2];
    int source_turns = 0;
    uint64_t source_seed = 0;
    int attempt = 0;
    bool ready = false;
    Move candidates[VNT_PEG1_POOL];
    int count = 0;
    int pass_rank = -1;
    double movegen_ms = 0;
    while (!ready) {
      if (++attempt > 1000) {
        log_fatal("peg1roots source generation failed");
      }
      source_seed = vnt_mix(seed ^ vnt_mix((uint64_t)root_id) ^
                            vnt_mix((uint64_t)attempt + UINT64_C(748238)));
      game_reset(game);
      game_seed(game, source_seed);
      game_set_starting_player_index(game, root_id % 2);
      draw_starting_racks(game);
      value_net_history_reset(&histories[0]);
      value_net_history_reset(&histories[1]);
      source_turns = 0;
      while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 1 &&
             source_turns < VNT_MAX_TURNS) {
        Move played;
        move_copy(&played, vnt_static_move(game, static_list));
        const int mover = game_get_player_on_turn_index(game);
        value_net_history_record_opponent_move(&histories[1 - mover], &played);
        play_move(&played, game, NULL);
        source_turns++;
      }
      if (!game_over(game) && bag_get_letters(game_get_bag(game)) == 1) {
        const int64_t start = ctimer_monotonic_ns();
        count = vnt_peg1_pool(game, pool, candidates, &pass_rank);
        movegen_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        ready = count > 0;
      }
    }
    const int mover = game_get_player_on_turn_index(game);
    int unseen[VALUE_NET_TILE_TYPES] = {0};
    int total = 0, worlds = 0;
    const Rack *opponent = player_get_rack(game_get_player(game, 1 - mover));
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      unseen[ml] = rack_get_letter(opponent, ml) +
                   bag_get_letter(game_get_bag(game), (MachineLetter)ml);
      total += unseen[ml];
      worlds += unseen[ml] > 0;
    }
    if (total != 8) {
      log_fatal("peg1roots source does not have eight unseen tiles");
    }
    char *cgp = game_get_cgp(game, false);
    fprintf(files[0], "%d,%llu,%d,%d,%d,1,%llu,%d,%d,%.9g,\"%s\"\n", root_id,
            (unsigned long long)source_seed, attempt, source_turns, mover,
            (unsigned long long)row, count, pass_rank, movegen_ms, cgp);
    free(cgp);
    double timing[4] = {0};
    for (int candidate = 0; candidate < count; candidate++, row++) {
      const Move *cand = &candidates[candidate];
      float scalars[VALUE_NET_SCALARS];
      value_net_scalars_for_move(game, cand, &histories[mover], scalars);
      if (!fwrite_le_floats(scalars, VALUE_NET_SCALARS, files[2])) {
        log_fatal("peg1roots scalar write failed");
      }
      float reply[VNT_PEG1_REPLY_ROW] = {0};
      reply[0] = (float)value_net_spread_after_move(game, cand);
      reply[1] = (float)equity_to_double(move_get_equity(cand));
      reply[2] = (float)move_get_tiles_played(cand);
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (unseen[tile] == 0) {
          continue;
        }
        const int64_t world_start = ctimer_monotonic_ns();
        vnt_peg1_world(game, unseen, tile, scenario);
        play_move(cand, scenario, NULL);
        if (bag_get_letters(game_get_bag(scenario)) != 0 ||
            game_over(scenario)) {
          log_fatal("peg1roots placement did not empty the bag");
        }
        timing[0] += (double)(ctimer_monotonic_ns() - world_start) / 1e6;
        vnt_peg1_reply_world(
            scenario, mover, work, small, move,
            &reply[VNT_PEG1_REPLY_HEAD + tile * VNT_PEG1_REPLY], timing);
      }
      if (!fwrite_le_floats(reply, VNT_PEG1_REPLY_ROW, files[3])) {
        log_fatal("peg1roots reply write failed");
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(game), cand,
                              game_get_ld(game), false);
      fprintf(files[1], "%llu,%d,%d,%d,%.9g,%.9g,%d,\"%s\"\n",
              (unsigned long long)row, root_id, candidate, count, reply[0],
              reply[1], move_get_tiles_played(cand), string_builder_peek(name));
    }
    fprintf(files[4], "%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g\n", root_id, count,
            worlds, movegen_ms, timing[0], timing[1], timing[2], timing[3]);
    for (int idx = 0; idx < 5; idx++) {
      fflush(files[idx]);
    }
  }
  for (int idx = 0; idx < 5; idx++) {
    fclose(files[idx]);
  }
  string_builder_destroy(name);
  move_destroy(move);
  move_list_destroy(small);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  game_destroy(scenario);
  game_destroy(work);
  config_destroy(config);
}

// Plays requested root candidates with the stored game's actual last tile,
// then a common continuation (0 = static top1, 2 = fixed two-ply endgame)
// to game over. Choices CSV: root,candidate (static rank), one per line.
static void vnt_peg1_play(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1play needs positions prefix, choices CSV, output prefix, "
              "continuation depth");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const int depth = atoi(string_splitter_get_item(fields, 4));
  if (depth != 0 && depth != 2) {
    log_fatal("peg1play invalid continuation");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *game = game_duplicate(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1),
           *static_list = move_list_create(1);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  FILE *choices = fopen_or_die(choices_path, "r");
  path = get_formatted_string("%s.games.csv", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "w");
  free(path);
  fprintf(output, "root,candidate,mover,actual_tile,initial_spread,final_"
                  "spread,win,turns,continuation_depth\n");
  char *line = NULL, *choice_line = NULL;
  size_t capacity = 0, choice_capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  int pending_root = -1, pending_candidate = -1;
  bool have_pending = false;
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1play invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1play invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1play CGP load failed");
    }
    Move candidates[VNT_PEG1_POOL];
    int current_pass;
    const int count = vnt_peg1_pool(root, pool, candidates, &current_pass);
    if (count != expected || current_pass != pass_rank) {
      log_fatal("peg1play candidate replay mismatch");
    }
    int actual_tile = -1;
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
        actual_tile = ml;
      }
    }
    // Consume this root's requested candidates (choices sorted by root).
    while (true) {
      if (!have_pending) {
        if (getline_ignore_carriage_return(&choice_line, &choice_capacity,
                                           choices) < 0) {
          break;
        }
        if (sscanf(choice_line, "%d,%d", &pending_root, &pending_candidate) !=
            2) {
          continue;
        }
        have_pending = true;
      }
      if (pending_root != root_id) {
        if (pending_root < root_id) {
          log_fatal("peg1play choices out of order");
        }
        break;
      }
      have_pending = false;
      if (pending_candidate < 0 || pending_candidate >= count) {
        log_fatal("peg1play invalid candidate");
      }
      game_copy(game, root);
      const double initial = vnt_peg1_margin(game, seat);
      play_move(&candidates[pending_candidate], game, NULL);
      if (bag_get_letters(game_get_bag(game)) != 0) {
        log_fatal("peg1play actual draw not consumed");
      }
      int turns = 1;
      while (!game_over(game) && turns < VNT_MAX_TURNS) {
        Move next;
        if (depth == 2) {
          vnt_fixed_endgame(game, &next, 2);
        } else {
          move_copy(&next, vnt_static_move(game, static_list));
        }
        play_move(&next, game, NULL);
        turns++;
      }
      if (!game_over(game)) {
        log_fatal("peg1play continuation did not reach game over");
      }
      const double final = vnt_peg1_margin(game, seat);
      fprintf(output, "%d,%d,%d,%d,%.9g,%.9g,%.1f,%d,%d\n", root_id,
              pending_candidate, seat, actual_tile, initial, final,
              final > 0   ? 1.0
              : final < 0 ? 0.0
                          : .5,
              turns, depth);
    }
    fflush(output);
  }
  if (have_pending) {
    log_fatal("peg1play choice for a root not in positions");
  }
  free(line);
  free(choice_line);
  fclose(positions);
  fclose(choices);
  fclose(output);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  game_destroy(game);
  config_destroy(config);
}

// Native 1PEG reply-feature network. The mover ranks its static top-K
// placements using only its own rack and public information: every
// hypothetical last tile is enumerated from the public unseen pool, the
// per-world reply features are averaged by multiplicity, and one forward
// pass scores each candidate. The actual hidden tile is never read.
enum {
  VNT_PEG1_NET_MAGIC = 0x50473152,
  VNT_PEG1_NET_MAX_INPUT = 160,
  VNT_PEG1_NET_MAX_WIDTH = 512,
  VNT_PEG1_GROUP_OPP = 1,
  VNT_PEG1_GROUP_MOVER_NOW = 2,
  VNT_PEG1_GROUP_PLAYOUT = 4,
};

typedef struct VntPeg1Net {
  int inputs;
  int width;
  int top_k;
  uint32_t groups;
  int reply_index[VNT_PEG1_REPLY];
  int reply_count;
  float *blob;
  const float *first;
  const float *bias;
  const float *out;
  float out_bias;
} VntPeg1Net;

typedef struct VntPeg1NetTiming {
  double movegen_ms;
  double scalar_ms;
  double world_ms;
  double opp_ms;
  double mover_ms;
  double playout_ms;
  double head_ms;
} VntPeg1NetTiming;

static VntPeg1Net *vnt_peg1_net_load(const char *path) {
  FILE *file = fopen_or_die(path, "rb");
  uint32_t shape[5];
  if (!fread_le_uint32s(shape, 5, file) || shape[0] != VNT_PEG1_NET_MAGIC ||
      shape[1] < 1 || shape[1] > VNT_PEG1_NET_MAX_INPUT || shape[2] < 1 ||
      shape[2] > VNT_PEG1_NET_MAX_WIDTH || shape[3] < 1 ||
      shape[3] > VNT_PEG1_POOL || shape[4] > 7) {
    log_fatal("invalid 1PEG reply network header");
  }
  VntPeg1Net *net = calloc_or_die(1, sizeof(VntPeg1Net));
  net->inputs = (int)shape[1];
  net->width = (int)shape[2];
  net->top_k = (int)shape[3];
  net->groups = shape[4];
  const size_t count = (size_t)net->width * (net->inputs + 2) + 1;
  uint32_t *bits = malloc_or_die(count * sizeof(uint32_t));
  net->blob = malloc_or_die(count * sizeof(float));
  if (!fread_le_uint32s(bits, count, file) || fgetc(file) != EOF) {
    log_fatal("invalid 1PEG reply network weights");
  }
  memcpy(net->blob, bits, count * sizeof(float));
  free(bits);
  fclose(file);
  net->first = net->blob;
  net->bias = net->first + (size_t)net->width * net->inputs;
  net->out = net->bias + net->width;
  net->out_bias = net->out[net->width];
  // Sorted reply-feature indices, matching the training feature builder.
  const bool opp = net->groups & VNT_PEG1_GROUP_OPP;
  const bool mover_now = net->groups & VNT_PEG1_GROUP_MOVER_NOW;
  const bool playout = net->groups & VNT_PEG1_GROUP_PLAYOUT;
  const bool use[VNT_PEG1_REPLY] = {opp,       opp,     opp,     mover_now,
                                    mover_now, playout, playout, playout,
                                    playout,   opp};
  for (int idx = 0; idx < VNT_PEG1_REPLY; idx++) {
    if (use[idx]) {
      net->reply_index[net->reply_count++] = idx;
    }
  }
  const int expected = (VALUE_NET_SCALARS - 4) + 2 + 4 * net->reply_count +
                       (playout ? 1 : 0) + (opp ? 1 : 0);
  if (expected != net->inputs) {
    log_fatal("1PEG reply network input width %d, expected %d", net->inputs,
              expected);
  }
  return net;
}

static void vnt_peg1_net_destroy(VntPeg1Net *net) {
  free(net->blob);
  free(net);
}

// Unseen counts from the mover's own view: the distribution less the board
// and the mover's own rack. Uses no hidden information.
static int vnt_peg1_public_unseen(const Game *game, int *unseen) {
  const LetterDistribution *ld = game_get_ld(game);
  const Rack *own = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    unseen[ml] = ld_get_dist(ld, (MachineLetter)ml) -
                 rack_get_letter(own, (MachineLetter)ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter ml = board_get_letter(board, row, col);
      if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      unseen[get_is_blanked(ml) ? BLANK_MACHINE_LETTER
                                : get_unblanked_machine_letter(ml)]--;
    }
  }
  int total = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    if (unseen[ml] < 0) {
      log_fatal("1PEG negative public unseen count");
    }
    total += unseen[ml];
  }
  return total;
}

// Per-world reply features for the groups the network uses. scenario is the
// post-candidate hypothetical world with the opponent on turn.
static void vnt_peg1_net_world(const VntPeg1Net *net, Game *scenario, int mover,
                               Game *work, MoveList *small, Move *move,
                               float *feature, VntPeg1NetTiming *timing) {
  bool out, pass;
  int tiles;
  if (net->groups & VNT_PEG1_GROUP_MOVER_NOW) {
    const int64_t start = ctimer_monotonic_ns();
    game_set_player_on_turn_index(scenario, mover);
    feature[3] =
        (float)vnt_peg1_greedy(scenario, small, move, &out, &tiles, &pass);
    feature[4] = out;
    game_set_player_on_turn_index(scenario, 1 - mover);
    timing->mover_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  }
  if (net->groups & VNT_PEG1_GROUP_PLAYOUT) {
    const int64_t start = ctimer_monotonic_ns();
    game_copy(work, scenario);
    int moves = 0;
    while (!game_over(work) && moves < VNT_PEG1_PLAYOUT_CAP) {
      const int value = vnt_peg1_greedy(work, small, move, &out, &tiles, &pass);
      if (moves == 0) {
        feature[0] = (float)value;
        feature[1] = out;
        feature[2] = (float)tiles;
        feature[9] = pass;
      } else if (moves == 1) {
        feature[5] = (float)value;
        feature[6] = out;
      }
      play_move(move, work, NULL);
      moves++;
    }
    if (!game_over(work)) {
      log_fatal("1PEG network greedy playout did not end");
    }
    feature[7] = (float)vnt_peg1_margin(work, mover);
    feature[8] = (float)moves;
    timing->playout_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  } else if (net->groups & VNT_PEG1_GROUP_OPP) {
    const int64_t start = ctimer_monotonic_ns();
    feature[0] =
        (float)vnt_peg1_greedy(scenario, small, move, &out, &tiles, &pass);
    feature[1] = out;
    feature[2] = (float)tiles;
    feature[9] = pass;
    timing->opp_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  }
}

// Fills inputs (K x net->inputs) for the root's static top-K placements.
static void vnt_peg1_net_inputs(const VntPeg1Net *net, const Game *root,
                                const Move *candidates, int count,
                                Game *scenario, Game *work, MoveList *small,
                                Move *move, float *inputs,
                                VntPeg1NetTiming *timing) {
  int unseen[VALUE_NET_TILE_TYPES];
  const int total = vnt_peg1_public_unseen(root, unseen);
  if (total != RACK_SIZE + 1) {
    log_fatal("1PEG network needs eight unseen tiles, found %d", total);
  }
  const int mover = game_get_player_on_turn_index(root);
  ValueNetHistory history;
  value_net_history_reset(&history);
  for (int candidate = 0; candidate < count; candidate++) {
    const Move *cand = &candidates[candidate];
    float *row = &inputs[candidate * net->inputs];
    int64_t start = ctimer_monotonic_ns();
    float scalars[VALUE_NET_SCALARS];
    value_net_scalars_for_move(root, cand, &history, scalars);
    int col = 0;
    for (int idx = 0; idx < VALUE_NET_SCALARS; idx++) {
      if (idx != 54 && idx != 55 && idx != 56 && idx != 67) {
        row[col++] = scalars[idx];
      }
    }
    const double after = value_net_spread_after_move(root, cand);
    row[col++] = (float)equity_to_double(move_get_equity(cand));
    row[col++] = (float)after;
    timing->scalar_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
    double sums[VNT_PEG1_REPLY] = {0}, squares[VNT_PEG1_REPLY] = {0};
    double low[VNT_PEG1_REPLY], high[VNT_PEG1_REPLY];
    for (int idx = 0; idx < VNT_PEG1_REPLY; idx++) {
      low[idx] = INFINITY;
      high[idx] = -INFINITY;
    }
    double playout_win = 0;
    for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
      if (unseen[tile] == 0) {
        continue;
      }
      start = ctimer_monotonic_ns();
      vnt_peg1_world(root, unseen, tile, scenario);
      play_move(cand, scenario, NULL);
      timing->world_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
      float feature[VNT_PEG1_REPLY] = {0};
      vnt_peg1_net_world(net, scenario, mover, work, small, move, feature,
                         timing);
      const double probability = (double)unseen[tile] / total;
      if (net->groups & VNT_PEG1_GROUP_PLAYOUT) {
        playout_win += probability * (feature[7] > 0    ? 1.0
                                      : feature[7] == 0 ? 0.5
                                                        : 0.0);
        feature[7] = (float)(feature[7] - after);
      }
      for (int idx = 0; idx < net->reply_count; idx++) {
        const double value = feature[net->reply_index[idx]];
        sums[idx] += probability * value;
        squares[idx] += probability * value * value;
        low[idx] = fmin(low[idx], value);
        high[idx] = fmax(high[idx], value);
      }
    }
    for (int idx = 0; idx < net->reply_count; idx++) {
      row[col + idx] = (float)sums[idx];
      row[col + net->reply_count + idx] =
          (float)sqrt(fmax(0, squares[idx] - sums[idx] * sums[idx]));
      row[col + 2 * net->reply_count + idx] = (float)low[idx];
      row[col + 3 * net->reply_count + idx] = (float)high[idx];
    }
    col += 4 * net->reply_count;
    if (net->groups & VNT_PEG1_GROUP_PLAYOUT) {
      row[col++] = (float)playout_win;
    }
    if (net->groups & VNT_PEG1_GROUP_OPP) {
      row[col++] = (float)(after - sums[0]);
    }
    if (col != net->inputs) {
      log_fatal("1PEG network input assembly mismatch");
    }
  }
}

static double vnt_peg1_net_score(const VntPeg1Net *net, const float *input) {
  float hidden[VNT_PEG1_NET_MAX_WIDTH];
  for (int unit = 0; unit < net->width; unit++) {
    const float *weights = &net->first[(size_t)unit * net->inputs];
    float sum = net->bias[unit];
    for (int col = 0; col < net->inputs; col++) {
      sum += weights[col] * input[col];
    }
    hidden[unit] = fmaxf(sum, 0);
  }
  float score = net->out_bias;
  for (int unit = 0; unit < net->width; unit++) {
    score += net->out[unit] * hidden[unit];
  }
  return score;
}

// Chooses among the static top-K placements; returns the static rank chosen,
// or -1 when the mover has no placement (the caller must pass).
static int vnt_peg1_net_choose(const VntPeg1Net *net, const Game *root,
                               MoveList *pool, Move *candidates, int *count,
                               Game *scenario, Game *work, MoveList *small,
                               Move *move, float *inputs, double *scores,
                               VntPeg1NetTiming *timing) {
  int64_t start = ctimer_monotonic_ns();
  int pass_rank;
  *count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
  timing->movegen_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (*count == 0) {
    return -1;
  }
  const int scored = *count < net->top_k ? *count : net->top_k;
  vnt_peg1_net_inputs(net, root, candidates, scored, scenario, work, small,
                      move, inputs, timing);
  start = ctimer_monotonic_ns();
  int choice = 0;
  for (int candidate = 0; candidate < scored; candidate++) {
    scores[candidate] =
        vnt_peg1_net_score(net, &inputs[candidate * net->inputs]);
    if (scores[candidate] > scores[choice]) {
      choice = candidate;
    }
  }
  timing->head_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  return choice;
}

// Evaluates the network on stored or fresh roots. Writes decisions.csv,
// choices.csv (static rank 0 and the network choice, for peg1play),
// inputs.f32 and scores.f32. check=1 also rebuilds each root with the
// actual last tile swapped for an opponent-rack tile and requires identical
// inputs and choice (excluded from timing).
static void vnt_peg1_net_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1net needs model, positions prefix, output prefix, check");
  }
  VntPeg1Net *net = vnt_peg1_net_load(string_splitter_get_item(fields, 1));
  const char *data = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const bool check = atoi(string_splitter_get_item(fields, 4)) != 0;
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *swapped = game_duplicate(root), *scenario = game_duplicate(root),
       *work = game_duplicate(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *small = move_list_create_small(16);
  Move *move = move_create();
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  const char *suffixes[] = {"decisions.csv", "choices.csv", "inputs.f32",
                            "scores.f32"};
  FILE *output[4];
  for (int idx = 0; idx < 4; idx++) {
    path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    output[idx] = vnt_fopen_new_or_die(path, "wb");
    free(path);
  }
  fprintf(output[0], "root,candidates,scored,choice,movegen_ms,scalar_ms,"
                     "world_ms,opp_ms,mover_ms,playout_ms,head_ms,total_ms,"
                     "privacy_checked\n");
  float *inputs = malloc_or_die(sizeof(float) * VNT_PEG1_POOL * net->inputs);
  float *private_inputs =
      malloc_or_die(sizeof(float) * VNT_PEG1_POOL * net->inputs);
  double scores[VNT_PEG1_POOL], private_scores[VNT_PEG1_POOL];
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1net invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1net invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1net CGP load failed");
    }
    Move candidates[VNT_PEG1_POOL];
    int count;
    VntPeg1NetTiming timing = {0};
    const int64_t start = ctimer_monotonic_ns();
    const int choice =
        vnt_peg1_net_choose(net, root, pool, candidates, &count, scenario, work,
                            small, move, inputs, scores, &timing);
    const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    if (count != expected) {
      log_fatal("peg1net candidate replay mismatch");
    }
    const int scored = count < net->top_k ? count : net->top_k;
    if (check) {
      int unseen[VALUE_NET_TILE_TYPES];
      vnt_peg1_public_unseen(root, unseen);
      int actual = -1, other = -1;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
        if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
          actual = ml;
        }
      }
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
        if (unseen[ml] > 0 && ml != actual) {
          other = ml;
        }
      }
      vnt_peg1_world(root, unseen, other < 0 ? actual : other, swapped);
      Move private_candidates[VNT_PEG1_POOL];
      int private_count;
      VntPeg1NetTiming ignored = {0};
      const int private_choice = vnt_peg1_net_choose(
          net, swapped, pool, private_candidates, &private_count, scenario,
          work, small, move, private_inputs, private_scores, &ignored);
      if (private_count != count || private_choice != choice ||
          memcmp(inputs, private_inputs,
                 sizeof(float) * scored * net->inputs) != 0) {
        log_fatal("1PEG network depends on the hidden last tile");
      }
      for (int candidate = 0; candidate < count; candidate++) {
        if (compare_moves(&candidates[candidate],
                          &private_candidates[candidate], true) != -1) {
          log_fatal("1PEG network candidate order depends on hidden tile");
        }
      }
    }
    fprintf(output[0],
            "%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d\n", root_id,
            count, scored, choice, timing.movegen_ms, timing.scalar_ms,
            timing.world_ms, timing.opp_ms, timing.mover_ms, timing.playout_ms,
            timing.head_ms, total_ms, check);
    fprintf(output[1], "%d,0\n", root_id);
    if (choice > 0) {
      fprintf(output[1], "%d,%d\n", root_id, choice);
    }
    float score_row[VNT_PEG1_POOL];
    for (int candidate = 0; candidate < scored; candidate++) {
      score_row[candidate] = (float)scores[candidate];
    }
    if (!fwrite_le_floats(inputs, (size_t)scored * net->inputs, output[2]) ||
        !fwrite_le_floats(score_row, scored, output[3])) {
      log_fatal("peg1net write failed");
    }
    for (int idx = 0; idx < 4; idx++) {
      fflush(output[idx]);
    }
  }
  free(line);
  free(inputs);
  free(private_inputs);
  fclose(positions);
  for (int idx = 0; idx < 4; idx++) {
    fclose(output[idx]);
  }
  error_stack_destroy(errors);
  move_destroy(move);
  move_list_destroy(small);
  move_list_destroy(pool);
  game_destroy(swapped);
  game_destroy(scenario);
  game_destroy(work);
  config_destroy(config);
  vnt_peg1_net_destroy(net);
}

// Lanes whose plays can change when move lands on board (before the move):
// the lane-touch set of path_move_lists.c, duplicated for this harness.
static uint64_t vnt_peg1_lane_influence(const Board *board, const Move *move) {
  static const int deltas[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  uint64_t touched = 0;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = vertical ? row_start + tile_idx : row_start;
    const int col = vertical ? col_start : col_start + tile_idx;
    touched |= (UINT64_C(1) << row) | (UINT64_C(1) << (BOARD_DIM + col));
    for (int delta_idx = 0; delta_idx < 4; delta_idx++) {
      int walk_row = row + deltas[delta_idx][0];
      int walk_col = col + deltas[delta_idx][1];
      while (board_is_position_in_bounds(walk_row, walk_col) &&
             board_is_nonempty_or_bricked(board, walk_row, walk_col)) {
        walk_row += deltas[delta_idx][0];
        walk_col += deltas[delta_idx][1];
      }
      if (board_is_position_in_bounds(walk_row, walk_col)) {
        touched |=
            (UINT64_C(1) << walk_row) | (UINT64_C(1) << (BOARD_DIM + walk_col));
      }
    }
  }
  return touched;
}

static int vnt_peg1_move_lane(const Move *move) {
  return board_is_dir_vertical(move_get_dir(move))
             ? BOARD_DIM + move_get_col_start(move)
             : move_get_row_start(move);
}

// Highest-scoring play of the side on turn (WMP when available), optionally
// restricted to lanes and seeded with a known legal play. Writes the move.
static const KWG *vnt_peg1_override_kwg = NULL;

static void vnt_peg1_best_scoring(const Game *game, MoveList *list,
                                  uint64_t lanes, const Move *seed,
                                  Move *best) {
  const MoveGenArgs args = {.game = game,
                            .move_list = list,
                            .move_record_type = MOVE_RECORD_BEST,
                            .move_sort_type = MOVE_SORT_SCORE,
                            .override_kwg = vnt_peg1_override_kwg,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .lane_mask = lanes,
                            .initial_best_move = seed,
                            .disable_pat = true};
  generate_moves(&args);
  move_copy(best, move_list_get_move(list, 0));
}

enum { VNT_PEG1_ROOT_LANES = 3 };

// Benchmarks opponent-reply methods on stored roots' static top-K.
static void vnt_peg1_bench(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1bench needs positions prefix, output CSV, top K, wmp");
  }
  const bool use_wmp = atoi(string_splitter_get_item(fields, 4)) != 0;
  const char *data = string_splitter_get_item(fields, 1);
  const int top_k = atoi(string_splitter_get_item(fields, 3));
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  vnt_peg1_override_kwg =
      use_wmp ? NULL : player_get_kwg(game_get_player(root, 0));
  Game *scenario = game_duplicate(root), *opp_root = game_duplicate(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *small = move_list_create_small(16);
  MoveList *best_list = move_list_create(1);
  Move *move = move_create(), *found = move_create(),
       *touched_best = move_create();
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  FILE *output = vnt_fopen_new_or_die(string_splitter_get_item(fields, 2), "w");
  fprintf(output, "root,worlds,scored,blank_unseen,small_ms,best_ms,"
                  "root_prep_ms,lane_ms,lane_fallbacks,score_mismatch_best,"
                  "score_mismatch_lane,value_mismatch_small_vs_best,"
                  "mover_small_ms,mover_best_ms,mover_mismatch\n");
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9) {
      log_fatal("peg1bench invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors)) {
      log_fatal("peg1bench CGP load failed");
    }
    Move candidates[VNT_PEG1_POOL];
    int current_pass;
    const int count = vnt_peg1_pool(root, pool, candidates, &current_pass);
    const int scored = count < top_k ? count : top_k;
    int unseen[VALUE_NET_TILE_TYPES];
    vnt_peg1_public_unseen(root, unseen);
    const int mover = seat;
    // Per world: the opponent's best plays on the root board in its three
    // best lanes (each excluding the previous lanes).
    Move lane_best[VALUE_NET_TILE_TYPES][VNT_PEG1_ROOT_LANES];
    int64_t start = ctimer_monotonic_ns();
    int worlds = 0;
    for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
      if (unseen[tile] == 0) {
        continue;
      }
      worlds++;
      vnt_peg1_world(root, unseen, tile, opp_root);
      game_set_player_on_turn_index(opp_root, 1 - mover);
      uint64_t lanes = ~UINT64_C(0) >> (64 - 2 * BOARD_DIM);
      for (int idx = 0; idx < VNT_PEG1_ROOT_LANES; idx++) {
        vnt_peg1_best_scoring(opp_root, best_list, lanes, NULL,
                              &lane_best[tile][idx]);
        if (move_get_type(&lane_best[tile][idx]) ==
            GAME_EVENT_TILE_PLACEMENT_MOVE) {
          lanes &= ~(UINT64_C(1) << vnt_peg1_move_lane(&lane_best[tile][idx]));
        }
      }
    }
    const double root_prep_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    double small_ms = 0, best_ms = 0, lane_ms = 0, mover_small_ms = 0,
           mover_best_ms = 0;
    int fallbacks = 0, mismatch_best = 0, mismatch_lane = 0, value_mismatch = 0,
        mover_mismatch = 0;
    for (int candidate = 0; candidate < scored; candidate++) {
      const Move *cand = &candidates[candidate];
      const uint64_t touched =
          vnt_peg1_lane_influence(game_get_board(root), cand);
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (unseen[tile] == 0) {
          continue;
        }
        vnt_peg1_world(root, unseen, tile, scenario);
        play_move(cand, scenario, NULL);
        bool out, pass;
        int tiles;
        start = ctimer_monotonic_ns();
        const int value_small =
            vnt_peg1_greedy(scenario, small, move, &out, &tiles, &pass);
        small_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
        start = ctimer_monotonic_ns();
        vnt_peg1_best_scoring(scenario, best_list, 0, NULL, found);
        best_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
        const int best_score = equity_to_int(move_get_score(found));
        // Lane-incremental: first root lane best whose lane is untouched
        // remains legal with its score; regenerate touched lanes only.
        start = ctimer_monotonic_ns();
        const Move *base = NULL;
        for (int idx = 0; idx < VNT_PEG1_ROOT_LANES; idx++) {
          const Move *lb = &lane_best[tile][idx];
          if (move_get_type(lb) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
            break;
          }
          if (!((touched >> vnt_peg1_move_lane(lb)) & 1)) {
            base = lb;
            break;
          }
        }
        int lane_score;
        if (base == NULL) {
          fallbacks++;
          vnt_peg1_best_scoring(scenario, best_list, 0, NULL, touched_best);
          lane_score = equity_to_int(move_get_score(touched_best));
        } else {
          vnt_peg1_best_scoring(scenario, best_list, touched, base,
                                touched_best);
          lane_score = equity_to_int(move_get_score(touched_best));
        }
        lane_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
        mismatch_lane += lane_score != best_score;
        // BEST_SMALL greedy value ignores the bonus only when not going out.
        const int small_score =
            pass ? 0
                 : value_small - (out ? equity_to_int(calculate_end_rack_points(
                                            player_get_rack(game_get_player(
                                                scenario, mover)),
                                            game_get_ld(scenario)))
                                      : 0);
        mismatch_best += small_score != best_score;
        const bool best_out =
            move_get_type(found) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
            move_get_tiles_played(found) ==
                rack_get_total_letters(
                    player_get_rack(game_get_player(scenario, 1 - mover)));
        const int best_value =
            best_score +
            (best_out ? equity_to_int(calculate_end_rack_points(
                            player_get_rack(game_get_player(scenario, mover)),
                            game_get_ld(scenario)))
                      : 0);
        value_mismatch += best_value != value_small;
        // Mover's immediate reply both ways.
        game_set_player_on_turn_index(scenario, mover);
        start = ctimer_monotonic_ns();
        const int mover_small =
            vnt_peg1_greedy(scenario, small, move, &out, &tiles, &pass);
        mover_small_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
        start = ctimer_monotonic_ns();
        vnt_peg1_best_scoring(scenario, best_list, 0, NULL, found);
        mover_best_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
        const bool mover_out =
            move_get_type(found) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
            move_get_tiles_played(found) ==
                rack_get_total_letters(
                    player_get_rack(game_get_player(scenario, mover)));
        const int mover_best =
            equity_to_int(move_get_score(found)) +
            (mover_out
                 ? equity_to_int(calculate_end_rack_points(
                       player_get_rack(game_get_player(scenario, 1 - mover)),
                       game_get_ld(scenario)))
                 : 0);
        mover_mismatch += mover_best != mover_small;
      }
    }
    fprintf(output,
            "%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%d,%.9g,%.9g,%d\n",
            root_id, worlds, scored, unseen[0], small_ms, best_ms, root_prep_ms,
            lane_ms, fallbacks, mismatch_best, mismatch_lane, value_mismatch,
            mover_small_ms, mover_best_ms, mover_mismatch);
    fflush(output);
  }
  free(line);
  fclose(positions);
  fclose(output);
  error_stack_destroy(errors);
  move_destroy(move);
  move_destroy(found);
  move_destroy(touched_best);
  move_list_destroy(best_list);
  move_list_destroy(small);
  move_list_destroy(pool);
  game_destroy(scenario);
  game_destroy(opp_root);
  config_destroy(config);
}

// ---- 1PEG reply features v2 (shared by extraction and the served policy) --
// Opponent reply: the opponent's highest-scoring play on the post-candidate
// board, found incrementally. Per hypothetical world the opponent's best
// plays on the root board in its three best lanes are generated once; for a
// candidate, the best of those whose lane the candidate does not touch keeps
// its score, and only touched lanes are searched (seeded with it). Exactly
// equals a full best-scoring search. Value adds twice the mover's rack when
// the play goes out. Mover follow-up: BEST_SMALL greedy (score plus twice the
// opponent's rack when going out), optionally only for short leaves.
typedef struct VntPeg1Prep {
  int unseen[VALUE_NET_TILE_TYPES];
  int total;
  Move lane_best[VALUE_NET_TILE_TYPES][VNT_PEG1_ROOT_LANES];
} VntPeg1Prep;

typedef struct VntPeg1Work {
  Game *scenario;
  Game *opp_root;
  MoveList *best_list;
  MoveList *small;
  Move *move;
  Move *found;
} VntPeg1Work;

static VntPeg1Work vnt_peg1_work_create(const Game *game) {
  VntPeg1Work work = {.scenario = game_duplicate(game),
                      .opp_root = game_duplicate(game),
                      .best_list = move_list_create(1),
                      .small = move_list_create_small(16),
                      .move = move_create(),
                      .found = move_create()};
  return work;
}

static void vnt_peg1_work_destroy(VntPeg1Work *work) {
  game_destroy(work->scenario);
  game_destroy(work->opp_root);
  move_list_destroy(work->best_list);
  move_list_destroy(work->small);
  move_destroy(work->move);
  move_destroy(work->found);
}

static void vnt_peg1_prep(const Game *root, VntPeg1Work *work,
                          VntPeg1Prep *prep, bool opp) {
  prep->total = vnt_peg1_public_unseen(root, prep->unseen);
  if (prep->total != RACK_SIZE + 1) {
    log_fatal("1PEG v2 needs eight unseen tiles, found %d", prep->total);
  }
  if (!opp) {
    return;
  }
  const int mover = game_get_player_on_turn_index(root);
  for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
    if (prep->unseen[tile] == 0) {
      continue;
    }
    vnt_peg1_world(root, prep->unseen, tile, work->opp_root);
    game_set_player_on_turn_index(work->opp_root, 1 - mover);
    uint64_t lanes = ~UINT64_C(0) >> (64 - 2 * BOARD_DIM);
    for (int idx = 0; idx < VNT_PEG1_ROOT_LANES; idx++) {
      Move *best = &prep->lane_best[tile][idx];
      vnt_peg1_best_scoring(work->opp_root, work->best_list, lanes, NULL, best);
      if (move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        lanes &= ~(UINT64_C(1) << vnt_peg1_move_lane(best));
      }
    }
  }
}

// Per-world v2 features of candidate cand in world tile, written to
// feature[0..VNT_PEG1_REPLY) in the v1 layout (playout slots stay zero).
static void vnt_peg1_v2_world(const Game *root, const Move *cand,
                              uint64_t touched, int tile,
                              const VntPeg1Prep *prep, VntPeg1Work *work,
                              bool opp, bool mover_now, float *feature,
                              VntPeg1NetTiming *timing) {
  const int mover = game_get_player_on_turn_index(root);
  int64_t start = ctimer_monotonic_ns();
  vnt_peg1_world(root, prep->unseen, tile, work->scenario);
  play_move(cand, work->scenario, NULL);
  timing->world_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  const LetterDistribution *ld = game_get_ld(root);
  if (opp) {
    start = ctimer_monotonic_ns();
    const Move *base = NULL;
    for (int idx = 0; idx < VNT_PEG1_ROOT_LANES; idx++) {
      const Move *lb = &prep->lane_best[tile][idx];
      if (move_get_type(lb) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        break;
      }
      if (!((touched >> vnt_peg1_move_lane(lb)) & 1)) {
        base = lb;
        break;
      }
    }
    vnt_peg1_best_scoring(work->scenario, work->best_list,
                          base == NULL ? 0 : touched, base, work->found);
    const bool placement =
        move_get_type(work->found) == GAME_EVENT_TILE_PLACEMENT_MOVE;
    const int tiles = placement ? move_get_tiles_played(work->found) : 0;
    const bool out =
        placement && tiles == rack_get_total_letters(player_get_rack(
                                  game_get_player(work->scenario, 1 - mover)));
    feature[0] = (float)(equity_to_int(move_get_score(work->found)) +
                         (out ? equity_to_int(calculate_end_rack_points(
                                    player_get_rack(
                                        game_get_player(work->scenario, mover)),
                                    ld))
                              : 0));
    feature[1] = out;
    feature[2] = (float)tiles;
    feature[9] = !placement;
    timing->opp_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  }
  if (mover_now) {
    start = ctimer_monotonic_ns();
    bool out, pass;
    int tiles;
    game_set_player_on_turn_index(work->scenario, mover);
    feature[3] = (float)vnt_peg1_greedy(work->scenario, work->small, work->move,
                                        &out, &tiles, &pass);
    feature[4] = out;
    game_set_player_on_turn_index(work->scenario, 1 - mover);
    timing->mover_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  }
}

// Stored-shard extraction with the v2 definitions (reply2.f32, v1 layout).
static void vnt_peg1_features_v2(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 3) {
    log_fatal("peg1feat2 needs data prefix and output prefix");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  VntPeg1Work work = vnt_peg1_work_create(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  path = get_formatted_string("%s.reply2.f32", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "wb");
  free(path);
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1feat2 invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1feat2 invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1feat2 CGP load failed");
    }
    Move candidates[VNT_PEG1_POOL];
    int current_pass;
    const int count = vnt_peg1_pool(root, pool, candidates, &current_pass);
    if (count != expected || current_pass != pass_rank) {
      log_fatal("peg1feat2 candidate replay mismatch");
    }
    VntPeg1Prep prep;
    vnt_peg1_prep(root, &work, &prep, true);
    VntPeg1NetTiming timing = {0};
    for (int candidate = 0; candidate < count; candidate++) {
      const Move *cand = &candidates[candidate];
      const uint64_t touched =
          vnt_peg1_lane_influence(game_get_board(root), cand);
      float row[VNT_PEG1_REPLY_ROW] = {0};
      row[0] = (float)value_net_spread_after_move(root, cand);
      row[1] = (float)equity_to_double(move_get_equity(cand));
      row[2] = (float)move_get_tiles_played(cand);
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (prep.unseen[tile] == 0) {
          continue;
        }
        vnt_peg1_v2_world(root, cand, touched, tile, &prep, &work, true, true,
                          &row[VNT_PEG1_REPLY_HEAD + tile * VNT_PEG1_REPLY],
                          &timing);
      }
      if (!fwrite_le_floats(row, VNT_PEG1_REPLY_ROW, output)) {
        log_fatal("peg1feat2 write failed");
      }
    }
    fflush(output);
  }
  free(line);
  fclose(positions);
  fclose(output);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  vnt_peg1_work_destroy(&work);
  config_destroy(config);
}

// ---- Served v2 policy ------------------------------------------------------
enum {
  VNT_PEG1_NET2_MAGIC = 0x50473153,
};

typedef struct VntPeg1Net2 {
  VntPeg1Net base;
  int mover_max_leave;
} VntPeg1Net2;

static VntPeg1Net2 *vnt_peg1_net2_load(const char *path) {
  FILE *file = fopen_or_die(path, "rb");
  uint32_t shape[6];
  if (!fread_le_uint32s(shape, 6, file) || shape[0] != VNT_PEG1_NET2_MAGIC ||
      shape[1] < 1 || shape[1] > VNT_PEG1_NET_MAX_INPUT || shape[2] < 1 ||
      shape[2] > VNT_PEG1_NET_MAX_WIDTH || shape[3] < 1 ||
      shape[3] > VNT_PEG1_POOL || (shape[4] & ~UINT32_C(3)) != 0 ||
      (shape[4] & VNT_PEG1_GROUP_OPP) == 0 || shape[5] > RACK_SIZE) {
    log_fatal("invalid 1PEG v2 network header");
  }
  VntPeg1Net2 *net = calloc_or_die(1, sizeof(VntPeg1Net2));
  VntPeg1Net *base = &net->base;
  base->inputs = (int)shape[1];
  base->width = (int)shape[2];
  base->top_k = (int)shape[3];
  base->groups = shape[4];
  net->mover_max_leave = (int)shape[5];
  const size_t count = (size_t)base->width * (base->inputs + 2) + 1;
  uint32_t *bits = malloc_or_die(count * sizeof(uint32_t));
  base->blob = malloc_or_die(count * sizeof(float));
  if (!fread_le_uint32s(bits, count, file) || fgetc(file) != EOF) {
    log_fatal("invalid 1PEG v2 network weights");
  }
  memcpy(base->blob, bits, count * sizeof(float));
  free(bits);
  fclose(file);
  base->first = base->blob;
  base->bias = base->first + (size_t)base->width * base->inputs;
  base->out = base->bias + base->width;
  base->out_bias = base->out[base->width];
  const bool mover_now = base->groups & VNT_PEG1_GROUP_MOVER_NOW;
  const bool use[VNT_PEG1_REPLY] = {true,  true,  true,  mover_now, mover_now,
                                    false, false, false, false,     true};
  for (int idx = 0; idx < VNT_PEG1_REPLY; idx++) {
    if (use[idx]) {
      base->reply_index[base->reply_count++] = idx;
    }
  }
  const bool restricted = mover_now && net->mover_max_leave < RACK_SIZE;
  const int expected = (VALUE_NET_SCALARS - 4) + 2 + 4 * base->reply_count + 1 +
                       (restricted ? 1 : 0);
  if (expected != base->inputs) {
    log_fatal("1PEG v2 network input width %d, expected %d", base->inputs,
              expected);
  }
  return net;
}

static void vnt_peg1_net2_inputs(const VntPeg1Net2 *net, const Game *root,
                                 const Move *candidates, int count,
                                 VntPeg1Work *work, const VntPeg1Prep *shared,
                                 float *inputs, VntPeg1NetTiming *timing) {
  const VntPeg1Net *base = &net->base;
  const bool mover_now = base->groups & VNT_PEG1_GROUP_MOVER_NOW;
  const bool restricted = mover_now && net->mover_max_leave < RACK_SIZE;
  int64_t start = ctimer_monotonic_ns();
  VntPeg1Prep local;
  if (shared == NULL) {
    vnt_peg1_prep(root, work, &local, true);
  }
  const VntPeg1Prep *prep_ptr = shared == NULL ? &local : shared;
  const VntPeg1Prep prep = *prep_ptr;
  timing->opp_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  ValueNetHistory history;
  value_net_history_reset(&history);
  for (int candidate = 0; candidate < count; candidate++) {
    const Move *cand = &candidates[candidate];
    float *row = &inputs[candidate * base->inputs];
    start = ctimer_monotonic_ns();
    float scalars[VALUE_NET_SCALARS];
    value_net_scalars_for_move(root, cand, &history, scalars);
    int col = 0;
    for (int idx = 0; idx < VALUE_NET_SCALARS; idx++) {
      if (idx != 54 && idx != 55 && idx != 56 && idx != 67) {
        row[col++] = scalars[idx];
      }
    }
    const double after = value_net_spread_after_move(root, cand);
    row[col++] = (float)equity_to_double(move_get_equity(cand));
    row[col++] = (float)after;
    const int leave_size = RACK_SIZE - move_get_tiles_played(cand);
    const bool mover_here =
        mover_now && (!restricted || leave_size <= net->mover_max_leave);
    const uint64_t touched =
        vnt_peg1_lane_influence(game_get_board(root), cand);
    timing->scalar_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
    double sums[VNT_PEG1_REPLY] = {0}, squares[VNT_PEG1_REPLY] = {0};
    double low[VNT_PEG1_REPLY], high[VNT_PEG1_REPLY];
    for (int idx = 0; idx < VNT_PEG1_REPLY; idx++) {
      low[idx] = INFINITY;
      high[idx] = -INFINITY;
    }
    for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
      if (prep.unseen[tile] == 0) {
        continue;
      }
      float feature[VNT_PEG1_REPLY] = {0};
      vnt_peg1_v2_world(root, cand, touched, tile, &prep, work, true,
                        mover_here, feature, timing);
      const double probability = (double)prep.unseen[tile] / prep.total;
      for (int idx = 0; idx < base->reply_count; idx++) {
        const double value = feature[base->reply_index[idx]];
        sums[idx] += probability * value;
        squares[idx] += probability * value * value;
        low[idx] = fmin(low[idx], value);
        high[idx] = fmax(high[idx], value);
      }
    }
    for (int idx = 0; idx < base->reply_count; idx++) {
      row[col + idx] = (float)sums[idx];
      row[col + base->reply_count + idx] =
          (float)sqrt(fmax(0, squares[idx] - sums[idx] * sums[idx]));
      row[col + 2 * base->reply_count + idx] = (float)low[idx];
      row[col + 3 * base->reply_count + idx] = (float)high[idx];
    }
    col += 4 * base->reply_count;
    row[col++] = (float)(after - sums[0]);
    if (restricted) {
      row[col++] = mover_here;
    }
    if (col != base->inputs) {
      log_fatal("1PEG v2 input assembly mismatch");
    }
  }
}

// Returns the chosen static rank, or -1 when no placement exists (pass).
static int vnt_peg1_net2_choose(const VntPeg1Net2 *net, const Game *root,
                                MoveList *pool, Move *candidates, int *count,
                                VntPeg1Work *work, float *inputs,
                                double *scores, VntPeg1NetTiming *timing) {
  int64_t start = ctimer_monotonic_ns();
  int pass_rank;
  *count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
  timing->movegen_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (*count == 0) {
    return -1;
  }
  const int scored = *count < net->base.top_k ? *count : net->base.top_k;
  vnt_peg1_net2_inputs(net, root, candidates, scored, work, NULL, inputs,
                       timing);
  start = ctimer_monotonic_ns();
  int choice = 0;
  for (int candidate = 0; candidate < scored; candidate++) {
    scores[candidate] =
        vnt_peg1_net_score(&net->base, &inputs[candidate * net->base.inputs]);
    if (scores[candidate] > scores[choice]) {
      choice = candidate;
    }
  }
  timing->head_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  return choice;
}

static void vnt_peg1_net2_destroy(VntPeg1Net2 *net) {
  free(net->base.blob);
  free(net);
}

// v2 policy over stored or fresh roots: decisions/choices/inputs/scores, as
// peg1net, with the optional hidden-tile permutation check.
static void vnt_peg1_net2_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1net2 needs model, positions prefix, output prefix, check");
  }
  VntPeg1Net2 *net = vnt_peg1_net2_load(string_splitter_get_item(fields, 1));
  const char *data = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const bool check = atoi(string_splitter_get_item(fields, 4)) != 0;
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *swapped = game_duplicate(root);
  VntPeg1Work work = vnt_peg1_work_create(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  const char *suffixes[] = {"decisions.csv", "choices.csv", "inputs.f32",
                            "scores.f32"};
  FILE *output[4];
  for (int idx = 0; idx < 4; idx++) {
    path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    output[idx] = vnt_fopen_new_or_die(path, "wb");
    free(path);
  }
  fprintf(output[0], "root,candidates,scored,choice,movegen_ms,scalar_ms,"
                     "world_ms,opp_ms,mover_ms,playout_ms,head_ms,total_ms,"
                     "privacy_checked\n");
  const int inputs_width = net->base.inputs;
  float *inputs = malloc_or_die(sizeof(float) * VNT_PEG1_POOL * inputs_width);
  float *private_inputs =
      malloc_or_die(sizeof(float) * VNT_PEG1_POOL * inputs_width);
  double scores[VNT_PEG1_POOL], private_scores[VNT_PEG1_POOL];
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1net2 invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1net2 invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1net2 CGP load failed");
    }
    Move candidates[VNT_PEG1_POOL];
    int count;
    VntPeg1NetTiming timing = {0};
    const int64_t start = ctimer_monotonic_ns();
    const int choice = vnt_peg1_net2_choose(net, root, pool, candidates, &count,
                                            &work, inputs, scores, &timing);
    const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    if (count != expected) {
      log_fatal("peg1net2 candidate replay mismatch");
    }
    const int scored = count < net->base.top_k ? count : net->base.top_k;
    if (check) {
      int unseen[VALUE_NET_TILE_TYPES];
      vnt_peg1_public_unseen(root, unseen);
      int actual = -1, other = -1;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
        if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
          actual = ml;
        }
      }
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
        if (unseen[ml] > 0 && ml != actual) {
          other = ml;
        }
      }
      vnt_peg1_world(root, unseen, other < 0 ? actual : other, swapped);
      Move private_candidates[VNT_PEG1_POOL];
      int private_count;
      VntPeg1NetTiming ignored = {0};
      const int private_choice = vnt_peg1_net2_choose(
          net, swapped, pool, private_candidates, &private_count, &work,
          private_inputs, private_scores, &ignored);
      if (private_count != count || private_choice != choice ||
          memcmp(inputs, private_inputs,
                 sizeof(float) * scored * inputs_width) != 0) {
        log_fatal("1PEG v2 network depends on the hidden last tile");
      }
      for (int candidate = 0; candidate < count; candidate++) {
        if (compare_moves(&candidates[candidate],
                          &private_candidates[candidate], true) != -1) {
          log_fatal("1PEG v2 candidate order depends on hidden tile");
        }
      }
    }
    fprintf(output[0],
            "%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d\n", root_id,
            count, scored, choice, timing.movegen_ms, timing.scalar_ms,
            timing.world_ms, timing.opp_ms, timing.mover_ms, timing.playout_ms,
            timing.head_ms, total_ms, check);
    fprintf(output[1], "%d,0\n", root_id);
    if (choice > 0) {
      fprintf(output[1], "%d,%d\n", root_id, choice);
    }
    float score_row[VNT_PEG1_POOL];
    for (int candidate = 0; candidate < scored; candidate++) {
      score_row[candidate] = (float)scores[candidate];
    }
    if (!fwrite_le_floats(inputs, (size_t)scored * inputs_width, output[2]) ||
        !fwrite_le_floats(score_row, scored, output[3])) {
      log_fatal("peg1net2 write failed");
    }
    for (int idx = 0; idx < 4; idx++) {
      fflush(output[idx]);
    }
  }
  free(line);
  free(inputs);
  free(private_inputs);
  fclose(positions);
  for (int idx = 0; idx < 4; idx++) {
    fclose(output[idx]);
  }
  error_stack_destroy(errors);
  move_list_destroy(pool);
  vnt_peg1_work_destroy(&work);
  game_destroy(swapped);
  config_destroy(config);
  vnt_peg1_net2_destroy(net);
}

// Two-stage 1PEG chooser: the fast network ranks the static top-K by
// opponent-reply features; the strong network rescoring only the fast
// network's top shortlist entries adds the mover follow-up features.
// Returns the chosen static rank, or -1 when no placement exists (pass).
static int vnt_peg1_cascade_choose(
    const VntPeg1Net2 *fast, const VntPeg1Net2 *strong, int shortlist,
    const Game *root, MoveList *pool, Move *candidates, int *count,
    VntPeg1Work *work, float *fast_inputs, float *strong_inputs,
    double *fast_scores, double *strong_scores, int *short_ranks,
    VntPeg1NetTiming *stage1, VntPeg1NetTiming *stage2) {
  int64_t start = ctimer_monotonic_ns();
  int pass_rank;
  *count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
  stage1->movegen_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (*count == 0) {
    return -1;
  }
  const int scored = *count < fast->base.top_k ? *count : fast->base.top_k;
  start = ctimer_monotonic_ns();
  VntPeg1Prep prep;
  vnt_peg1_prep(root, work, &prep, true);
  stage1->opp_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  vnt_peg1_net2_inputs(fast, root, candidates, scored, work, &prep, fast_inputs,
                       stage1);
  start = ctimer_monotonic_ns();
  for (int candidate = 0; candidate < scored; candidate++) {
    fast_scores[candidate] = vnt_peg1_net_score(
        &fast->base, &fast_inputs[candidate * fast->base.inputs]);
  }
  // Shortlist: highest stage-1 scores, ties to the lower static rank.
  const int listed = scored < shortlist ? scored : shortlist;
  bool taken[VNT_PEG1_POOL] = {false};
  for (int slot = 0; slot < listed; slot++) {
    int best = -1;
    for (int candidate = 0; candidate < scored; candidate++) {
      if (!taken[candidate] &&
          (best < 0 || fast_scores[candidate] > fast_scores[best])) {
        best = candidate;
      }
    }
    taken[best] = true;
  }
  int filled = 0;
  Move short_moves[VNT_PEG1_POOL];
  for (int candidate = 0; candidate < scored; candidate++) {
    if (taken[candidate]) {
      short_ranks[filled] = candidate;
      move_copy(&short_moves[filled++], &candidates[candidate]);
    }
  }
  stage1->head_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  vnt_peg1_net2_inputs(strong, root, short_moves, filled, work, &prep,
                       strong_inputs, stage2);
  start = ctimer_monotonic_ns();
  int choice = 0;
  for (int slot = 0; slot < filled; slot++) {
    strong_scores[slot] = vnt_peg1_net_score(
        &strong->base, &strong_inputs[slot * strong->base.inputs]);
    if (strong_scores[slot] > strong_scores[choice]) {
      choice = slot;
    }
  }
  stage2->head_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  return short_ranks[choice];
}

// Full-game pairs. Each game is static self-play from the opening (the
// peg1data source procedure, without rejection). If a 1PEG arises, the
// mover's choice is made twice -- static top-1 and the v2 network -- and
// each branch is continued to game over with the common continuation
// (0 = static, 2 = fixed two-ply endgame); both final spreads come from the
// 1PEG mover's view. Games without a 1PEG are recorded as identical pairs.
static void vnt_peg1_games(const StringSplitter *fields) {
  const int items = string_splitter_get_number_of_items(fields);
  if (items != 7 && items != 9) {
    log_fatal("peg1games needs model, count, offset, seed, depth, prefix "
              "[, strong model, shortlist]");
  }
  VntPeg1Net2 *net = vnt_peg1_net2_load(string_splitter_get_item(fields, 1));
  // Optional cascade: net ranks, the strong model rescoring its shortlist.
  VntPeg1Net2 *strong =
      items == 9 ? vnt_peg1_net2_load(string_splitter_get_item(fields, 7))
                 : NULL;
  const int shortlist =
      items == 9 ? atoi(string_splitter_get_item(fields, 8)) : 0;
  const int games = atoi(string_splitter_get_item(fields, 2));
  const int offset = atoi(string_splitter_get_item(fields, 3));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 4), NULL, 10);
  const int depth = atoi(string_splitter_get_item(fields, 5));
  const char *prefix = string_splitter_get_item(fields, 6);
  if (games < 1 || offset < 0 || (depth != 0 && depth != 2)) {
    log_fatal("peg1games invalid arguments");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true "
      "-wit true -rit true -ritmmap true -threads 1 "
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *branch = game_duplicate(game);
  VntPeg1Work work = vnt_peg1_work_create(game);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  char *path = get_formatted_string("%s.fullgames.csv", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "w");
  free(path);
  fprintf(output, "game,source_seed,starter,peg1,mover,net_choice,candidates,"
                  "static_final,net_final,static_win,net_win,decision_ms,"
                  "continuation_depth\n");
  float *inputs =
      malloc_or_die(sizeof(float) * VNT_PEG1_POOL * net->base.inputs);
  float *strong_inputs =
      strong == NULL
          ? NULL
          : malloc_or_die(sizeof(float) * VNT_PEG1_POOL * strong->base.inputs);
  double scores[VNT_PEG1_POOL], strong_scores[VNT_PEG1_POOL];
  int short_ranks[VNT_PEG1_POOL];
  for (int local = 0; local < games; local++) {
    const int game_id = offset + local;
    const uint64_t source_seed =
        vnt_mix(seed ^ vnt_mix((uint64_t)game_id) ^ vnt_mix(UINT64_C(918273)));
    game_reset(game);
    game_seed(game, source_seed);
    game_set_starting_player_index(game, game_id % 2);
    draw_starting_racks(game);
    int turns = 0;
    while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 1 &&
           turns < VNT_MAX_TURNS) {
      Move played;
      move_copy(&played, vnt_static_move(game, static_list));
      play_move(&played, game, NULL);
      turns++;
    }
    const bool peg1 =
        !game_over(game) && bag_get_letters(game_get_bag(game)) == 1;
    double finals[2] = {0, 0};
    int count = 0, choice = 0, mover = -1;
    double decision_ms = 0;
    if (peg1) {
      mover = game_get_player_on_turn_index(game);
      Move candidates[VNT_PEG1_POOL];
      VntPeg1NetTiming timing = {0}, timing2 = {0};
      const int64_t start = ctimer_monotonic_ns();
      if (strong == NULL) {
        choice = vnt_peg1_net2_choose(net, game, pool, candidates, &count,
                                      &work, inputs, scores, &timing);
      } else {
        choice = vnt_peg1_cascade_choose(net, strong, shortlist, game, pool,
                                         candidates, &count, &work, inputs,
                                         strong_inputs, scores, strong_scores,
                                         short_ranks, &timing, &timing2);
      }
      decision_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
      for (int arm = 0; arm < 2; arm++) {
        game_copy(branch, game);
        Move first;
        if (choice < 0) {
          // No placement: both policies must pass.
          move_set_as_pass(&first);
        } else {
          move_copy(&first, &candidates[arm == 0 ? 0 : choice]);
        }
        play_move(&first, branch, NULL);
        int steps = 0;
        while (!game_over(branch) && steps < VNT_MAX_TURNS) {
          Move next;
          if (depth == 2 && bag_get_letters(game_get_bag(branch)) == 0) {
            vnt_fixed_endgame(branch, &next, 2);
          } else {
            move_copy(&next, vnt_static_move(branch, static_list));
          }
          play_move(&next, branch, NULL);
          steps++;
        }
        if (!game_over(branch)) {
          log_fatal("peg1games continuation did not end");
        }
        finals[arm] = vnt_peg1_margin(branch, mover);
      }
    } else {
      while (!game_over(game) && turns < VNT_MAX_TURNS) {
        Move played;
        move_copy(&played, vnt_static_move(game, static_list));
        play_move(&played, game, NULL);
        turns++;
      }
      finals[0] = finals[1] = vnt_peg1_margin(game, 0);
    }
    fprintf(output, "%d,%llu,%d,%d,%d,%d,%d,%.9g,%.9g,%.1f,%.1f,%.9g,%d\n",
            game_id, (unsigned long long)source_seed, game_id % 2, peg1, mover,
            choice, count, finals[0], finals[1],
            finals[0] > 0   ? 1.0
            : finals[0] < 0 ? 0.0
                            : .5,
            finals[1] > 0   ? 1.0
            : finals[1] < 0 ? 0.0
                            : .5,
            decision_ms, depth);
    fflush(output);
  }
  free(inputs);
  free(strong_inputs);
  fclose(output);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  vnt_peg1_work_destroy(&work);
  game_destroy(branch);
  config_destroy(config);
  vnt_peg1_net2_destroy(net);
  if (strong != NULL) {
    vnt_peg1_net2_destroy(strong);
  }
}

// Fresh 1PEG root positions only (the peg1data source procedure, no labels or
// features). Writes PREFIX.positions.csv in the peg1data format.
static void vnt_peg1_positions(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg1pos needs count, offset, seed and prefix");
  }
  const int roots = atoi(string_splitter_get_item(fields, 1));
  const int offset = atoi(string_splitter_get_item(fields, 2));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *prefix = string_splitter_get_item(fields, 4);
  if (roots < 1 || offset < 0) {
    log_fatal("peg1pos invalid count or offset");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true "
      "-wit true -rit true -ritmmap true -threads 1 "
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  char *path = get_formatted_string("%s.positions.csv", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "w");
  free(path);
  fprintf(output, "root,source_seed,source_attempt,turn,seat,bag,first_row,"
                  "rows,pass_rank,movegen_ms,cgp\n");
  uint64_t row = 0;
  for (int local = 0; local < roots; local++) {
    const int root_id = offset + local;
    uint64_t source_seed = 0;
    int attempt = 0, source_turns = 0, count = 0, pass_rank = -1;
    bool ready = false;
    Move candidates[VNT_PEG1_POOL];
    double movegen_ms = 0;
    while (!ready) {
      if (++attempt > 1000) {
        log_fatal("peg1pos source generation failed");
      }
      source_seed = vnt_mix(seed ^ vnt_mix((uint64_t)root_id) ^
                            vnt_mix((uint64_t)attempt + UINT64_C(748238)));
      game_reset(game);
      game_seed(game, source_seed);
      game_set_starting_player_index(game, root_id % 2);
      draw_starting_racks(game);
      source_turns = 0;
      while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 1 &&
             source_turns < VNT_MAX_TURNS) {
        Move played;
        move_copy(&played, vnt_static_move(game, static_list));
        play_move(&played, game, NULL);
        source_turns++;
      }
      if (!game_over(game) && bag_get_letters(game_get_bag(game)) == 1) {
        const int64_t start = ctimer_monotonic_ns();
        count = vnt_peg1_pool(game, pool, candidates, &pass_rank);
        movegen_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        ready = count > 0;
      }
    }
    char *cgp = game_get_cgp(game, false);
    fprintf(output, "%d,%llu,%d,%d,%d,1,%llu,%d,%d,%.9g,\"%s\"\n", root_id,
            (unsigned long long)source_seed, attempt, source_turns,
            game_get_player_on_turn_index(game), (unsigned long long)row, count,
            pass_rank, movegen_ms, cgp);
    free(cgp);
    row += count;
    fflush(output);
  }
  fclose(output);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  config_destroy(config);
}

// Deeper per-world labels for chosen candidates (teacher-quality check).
// Choices CSV rows: root,candidate (static rank), grouped by root in
// positions order. Every public hypothetical last tile is labelled with an
// endgame search of the requested plies (greedy leaf horizon, as depth2).
static void vnt_peg1_deep(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 6) {
    log_fatal("peg1deep needs positions prefix, choices CSV, output prefix, "
              "plies, tt fraction");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const int plies = atoi(string_splitter_get_item(fields, 4));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 5), NULL);
  if (plies < 1 || plies > 25 || tt_fraction < 0 || tt_fraction > 0.1) {
    log_fatal("peg1deep invalid plies or tt fraction");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *scenario = game_duplicate(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  ErrorStack *errors = error_stack_create();
  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  FILE *choices = fopen_or_die(choices_path, "r");
  path = get_formatted_string("%s.deep.csv", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "w");
  free(path);
  fprintf(output, "root,candidate,tile,multiplicity,plies,margin,solve_ms\n");
  char *line = NULL, *choice_line = NULL;
  size_t capacity = 0, choice_capacity = 0;
  int pending_root = -1, pending_candidate = -1;
  bool have_pending = false;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1deep invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1deep invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1deep CGP load failed");
    }
    Move candidates[VNT_PEG1_POOL];
    int current_pass;
    const int count = vnt_peg1_pool(root, pool, candidates, &current_pass);
    if (count != expected) {
      log_fatal("peg1deep candidate replay mismatch");
    }
    int unseen[VALUE_NET_TILE_TYPES];
    vnt_peg1_public_unseen(root, unseen);
    while (true) {
      if (!have_pending) {
        if (getline_ignore_carriage_return(&choice_line, &choice_capacity,
                                           choices) < 0) {
          break;
        }
        if (sscanf(choice_line, "%d,%d", &pending_root, &pending_candidate) !=
            2) {
          continue;
        }
        have_pending = true;
      }
      if (pending_root != root_id) {
        if (pending_root < root_id) {
          log_fatal("peg1deep choices out of order");
        }
        break;
      }
      have_pending = false;
      if (pending_candidate < 0 || pending_candidate >= count) {
        log_fatal("peg1deep invalid candidate");
      }
      for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
        if (unseen[tile] == 0) {
          continue;
        }
        vnt_peg1_world(root, unseen, tile, scenario);
        play_move(&candidates[pending_candidate], scenario, NULL);
        const int mover = seat;
        double margin = vnt_peg1_margin(scenario, mover);
        int depth_done = 0;
        const int64_t start = ctimer_monotonic_ns();
        if (!game_over(scenario)) {
          const EndgameArgs args = {.game = scenario,
                                    .thread_control = control,
                                    .plies = plies,
                                    .tt_fraction_of_mem = tt_fraction,
                                    .initial_small_move_arena_size =
                                        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                                    .num_threads = 1,
                                    .use_heuristics = true,
                                    .num_top_moves = 1,
                                    .incremental_movegen = true,
                                    .enable_pv_display = true,
                                    .dual_lexicon_mode =
                                        DUAL_LEXICON_MODE_IGNORANT};
          endgame_solve_inline(&ctx, &args, results);
          if (endgame_results_get_status(results) !=
              ENDGAME_RESULT_STATUS_FINISHED) {
            log_fatal("peg1deep search did not finish");
          }
          margin -= endgame_results_get_value(results, ENDGAME_RESULT_BEST);
          depth_done = endgame_results_get_depth(results, ENDGAME_RESULT_BEST);
        }
        fprintf(output, "%d,%d,%d,%d,%d,%.9g,%.9g\n", root_id,
                pending_candidate, tile, unseen[tile], depth_done, margin,
                (double)(ctimer_monotonic_ns() - start) / 1e6);
      }
      fflush(output);
    }
  }
  free(line);
  free(choice_line);
  fclose(positions);
  fclose(choices);
  fclose(output);
  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  thread_control_destroy(control);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  game_destroy(scenario);
  config_destroy(config);
}

static void vnt_peg1_cascade_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 7) {
    log_fatal("peg1cascade needs fast model, strong model, shortlist, "
              "positions prefix, output prefix, check");
  }
  VntPeg1Net2 *fast = vnt_peg1_net2_load(string_splitter_get_item(fields, 1));
  VntPeg1Net2 *strong = vnt_peg1_net2_load(string_splitter_get_item(fields, 2));
  const int shortlist = atoi(string_splitter_get_item(fields, 3));
  const char *data = string_splitter_get_item(fields, 4);
  const char *prefix = string_splitter_get_item(fields, 5);
  const bool check = atoi(string_splitter_get_item(fields, 6)) != 0;
  if (shortlist < 1 || shortlist > VNT_PEG1_POOL ||
      (strong->base.groups & VNT_PEG1_GROUP_MOVER_NOW) == 0) {
    log_fatal("peg1cascade invalid shortlist or stage-2 model");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *swapped = game_duplicate(root);
  VntPeg1Work work = vnt_peg1_work_create(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  const char *suffixes[] = {"decisions.csv", "choices.csv"};
  FILE *output[2];
  for (int idx = 0; idx < 2; idx++) {
    path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    output[idx] = vnt_fopen_new_or_die(path, "w");
    free(path);
  }
  fprintf(output[0],
          "root,candidates,choice,movegen_ms,stage1_world_ms,stage1_opp_ms,"
          "stage1_scalar_ms,stage1_head_ms,stage2_world_ms,stage2_opp_ms,"
          "stage2_mover_ms,stage2_scalar_ms,stage2_head_ms,total_ms,"
          "privacy_checked,static_ms,static_is_pool_top\n");
  MoveList *static_list = move_list_create(1);
  float *fast_inputs =
      malloc_or_die(sizeof(float) * VNT_PEG1_POOL * fast->base.inputs);
  float *strong_inputs =
      malloc_or_die(sizeof(float) * VNT_PEG1_POOL * strong->base.inputs);
  double fast_scores[VNT_PEG1_POOL], strong_scores[VNT_PEG1_POOL];
  int short_ranks[VNT_PEG1_POOL];
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1cascade invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1cascade invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1cascade CGP load failed");
    }
    // Reference cost of plain static selection on the same root.
    const int64_t static_start = ctimer_monotonic_ns();
    Move static_choice;
    move_copy(&static_choice, vnt_static_move(root, static_list));
    const double static_ms =
        (double)(ctimer_monotonic_ns() - static_start) / 1e6;
    Move candidates[VNT_PEG1_POOL];
    int count;
    VntPeg1NetTiming stage1 = {0}, stage2 = {0};
    const int64_t start = ctimer_monotonic_ns();
    const int choice = vnt_peg1_cascade_choose(
        fast, strong, shortlist, root, pool, candidates, &count, &work,
        fast_inputs, strong_inputs, fast_scores, strong_scores, short_ranks,
        &stage1, &stage2);
    const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    if (count != expected) {
      log_fatal("peg1cascade candidate replay mismatch");
    }
    if (check) {
      int unseen[VALUE_NET_TILE_TYPES];
      vnt_peg1_public_unseen(root, unseen);
      int actual = -1, other = -1;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
        if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
          actual = ml;
        }
      }
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
        if (unseen[ml] > 0 && ml != actual) {
          other = ml;
        }
      }
      vnt_peg1_world(root, unseen, other < 0 ? actual : other, swapped);
      Move private_candidates[VNT_PEG1_POOL];
      int private_count;
      VntPeg1NetTiming ignored1 = {0}, ignored2 = {0};
      double private_fast[VNT_PEG1_POOL], private_strong[VNT_PEG1_POOL];
      int private_ranks[VNT_PEG1_POOL];
      const int private_choice = vnt_peg1_cascade_choose(
          fast, strong, shortlist, swapped, pool, private_candidates,
          &private_count, &work, fast_inputs, strong_inputs, private_fast,
          private_strong, private_ranks, &ignored1, &ignored2);
      const int scored = count < fast->base.top_k ? count : fast->base.top_k;
      if (private_count != count || private_choice != choice ||
          memcmp(private_fast, fast_scores, sizeof(double) * scored) != 0) {
        log_fatal("1PEG cascade depends on the hidden last tile");
      }
    }
    fprintf(output[0],
            "%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
            "%d,%.9g,%d\n",
            root_id, count, choice, stage1.movegen_ms, stage1.world_ms,
            stage1.opp_ms, stage1.scalar_ms, stage1.head_ms, stage2.world_ms,
            stage2.opp_ms, stage2.mover_ms, stage2.scalar_ms, stage2.head_ms,
            total_ms, check, static_ms,
            count > 0 &&
                compare_moves(&static_choice, &candidates[0], true) == -1);
    fprintf(output[1], "%d,0\n", root_id);
    if (choice > 0) {
      fprintf(output[1], "%d,%d\n", root_id, choice);
    }
    fflush(output[0]);
    fflush(output[1]);
  }
  free(line);
  free(fast_inputs);
  free(strong_inputs);
  fclose(positions);
  fclose(output[0]);
  fclose(output[1]);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  vnt_peg1_work_destroy(&work);
  game_destroy(swapped);
  config_destroy(config);
  vnt_peg1_net2_destroy(fast);
  vnt_peg1_net2_destroy(strong);
}

// ---- Utility mode (v3): win probability and spread blended for choosing --
// A v3 network has three outputs per candidate: win logit z, absolute-spread
// residual r (spread = one-ply estimate + 20 r) and a ranking score. Inputs
// are the v2 features plus threshold features: per estimate (one-ply spread
// after the opponent's reply; with mover features also a crude two-ply that
// adds the mover's follow-up when the opponent does not go out), the
// multiplicity-weighted fraction of public hypothetical worlds above each
// threshold, then mean, standard deviation, minimum and maximum. Choosing
// maximizes sim_utility_blend(sigmoid(z), spread, w_win, w_spread, scale);
// a negative w_spread selects margin mode (the ranking score).
enum {
  VNT_PEG1_NET3_MAGIC = 0x50473155,
  VNT_PEG1_NET3_OUTPUTS = 3,
  VNT_PEG1_THRESHOLDS = 9,
  VNT_PEG1_THRESHOLD_BLOCK = VNT_PEG1_THRESHOLDS + 4,
};

static const double vnt_peg1_thresholds[VNT_PEG1_THRESHOLDS] = {
    -40, -20, -10, -5, 0, 5, 10, 20, 40};

typedef struct VntPeg1Net3 {
  VntPeg1Net2 v2;
  int v2_inputs;
  int estimates;
  const float *out3;
  float out_bias3[VNT_PEG1_NET3_OUTPUTS];
} VntPeg1Net3;

typedef struct VntPeg1Utility {
  double w_win;
  double w_spread;
  double scale;
} VntPeg1Utility;

static VntPeg1Net3 *vnt_peg1_net3_load(const char *path) {
  FILE *file = fopen_or_die(path, "rb");
  uint32_t shape[7];
  if (!fread_le_uint32s(shape, 7, file) || shape[0] != VNT_PEG1_NET3_MAGIC ||
      shape[1] < 1 || shape[1] > VNT_PEG1_NET_MAX_INPUT || shape[2] < 1 ||
      shape[2] > VNT_PEG1_NET_MAX_WIDTH || shape[3] < 1 ||
      shape[3] > VNT_PEG1_POOL || (shape[4] & ~UINT32_C(3)) != 0 ||
      (shape[4] & VNT_PEG1_GROUP_OPP) == 0 || shape[5] != RACK_SIZE ||
      shape[6] != VNT_PEG1_NET3_OUTPUTS) {
    log_fatal("invalid 1PEG v3 network header");
  }
  VntPeg1Net3 *net = calloc_or_die(1, sizeof(VntPeg1Net3));
  VntPeg1Net *base = &net->v2.base;
  base->inputs = (int)shape[1];
  base->width = (int)shape[2];
  base->top_k = (int)shape[3];
  base->groups = shape[4];
  net->v2.mover_max_leave = RACK_SIZE;
  const size_t count = (size_t)base->width * (base->inputs + 1) +
                       (size_t)VNT_PEG1_NET3_OUTPUTS * (base->width + 1);
  uint32_t *bits = malloc_or_die(count * sizeof(uint32_t));
  base->blob = malloc_or_die(count * sizeof(float));
  if (!fread_le_uint32s(bits, count, file) || fgetc(file) != EOF) {
    log_fatal("invalid 1PEG v3 network weights");
  }
  memcpy(base->blob, bits, count * sizeof(float));
  free(bits);
  fclose(file);
  base->first = base->blob;
  base->bias = base->first + (size_t)base->width * base->inputs;
  net->out3 = base->bias + base->width;
  for (int out = 0; out < VNT_PEG1_NET3_OUTPUTS; out++) {
    net->out_bias3[out] =
        net->out3[(size_t)VNT_PEG1_NET3_OUTPUTS * base->width + out];
  }
  const bool mover_now = base->groups & VNT_PEG1_GROUP_MOVER_NOW;
  const bool use[VNT_PEG1_REPLY] = {true,  true,  true,  mover_now, mover_now,
                                    false, false, false, false,     true};
  for (int idx = 0; idx < VNT_PEG1_REPLY; idx++) {
    if (use[idx]) {
      base->reply_index[base->reply_count++] = idx;
    }
  }
  net->estimates = mover_now ? 2 : 1;
  net->v2_inputs = (VALUE_NET_SCALARS - 4) + 2 + 4 * base->reply_count + 1;
  if (net->v2_inputs + net->estimates * VNT_PEG1_THRESHOLD_BLOCK !=
      base->inputs) {
    log_fatal("1PEG v3 network input width %d, expected %d", base->inputs,
              net->v2_inputs + net->estimates * VNT_PEG1_THRESHOLD_BLOCK);
  }
  return net;
}

static void vnt_peg1_net3_destroy(VntPeg1Net3 *net) {
  free(net->v2.base.blob);
  free(net);
}

// v3 inputs: the v2 row (built by the v2 code into a scratch row) followed
// by the threshold block(s), from the same per-world features.
static void vnt_peg1_net3_inputs(const VntPeg1Net3 *net, const Game *root,
                                 const Move *candidates, int count,
                                 VntPeg1Work *work, const VntPeg1Prep *prep,
                                 float *inputs, VntPeg1NetTiming *timing) {
  const VntPeg1Net *base = &net->v2.base;
  const bool mover_now = base->groups & VNT_PEG1_GROUP_MOVER_NOW;
  ValueNetHistory history;
  value_net_history_reset(&history);
  for (int candidate = 0; candidate < count; candidate++) {
    const Move *cand = &candidates[candidate];
    float *row = &inputs[candidate * base->inputs];
    int64_t start = ctimer_monotonic_ns();
    float scalars[VALUE_NET_SCALARS];
    value_net_scalars_for_move(root, cand, &history, scalars);
    int col = 0;
    for (int idx = 0; idx < VALUE_NET_SCALARS; idx++) {
      if (idx != 54 && idx != 55 && idx != 56 && idx != 67) {
        row[col++] = scalars[idx];
      }
    }
    const double after = value_net_spread_after_move(root, cand);
    row[col++] = (float)equity_to_double(move_get_equity(cand));
    row[col++] = (float)after;
    const uint64_t touched =
        vnt_peg1_lane_influence(game_get_board(root), cand);
    timing->scalar_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
    double sums[VNT_PEG1_REPLY] = {0}, squares[VNT_PEG1_REPLY] = {0};
    double low[VNT_PEG1_REPLY], high[VNT_PEG1_REPLY];
    for (int idx = 0; idx < VNT_PEG1_REPLY; idx++) {
      low[idx] = INFINITY;
      high[idx] = -INFINITY;
    }
    double est_value[2][VALUE_NET_TILE_TYPES];
    double est_prob[VALUE_NET_TILE_TYPES];
    int worlds = 0;
    for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
      if (prep->unseen[tile] == 0) {
        continue;
      }
      float feature[VNT_PEG1_REPLY] = {0};
      vnt_peg1_v2_world(root, cand, touched, tile, prep, work, true, mover_now,
                        feature, timing);
      const double probability = (double)prep->unseen[tile] / prep->total;
      for (int idx = 0; idx < base->reply_count; idx++) {
        const double value = feature[base->reply_index[idx]];
        sums[idx] += probability * value;
        squares[idx] += probability * value * value;
        low[idx] = fmin(low[idx], value);
        high[idx] = fmax(high[idx], value);
      }
      const double one_ply = after - (double)feature[0];
      est_value[0][worlds] = one_ply;
      est_value[1][worlds] =
          one_ply + (1.0 - (double)feature[1]) * (double)feature[3];
      est_prob[worlds++] = probability;
    }
    for (int idx = 0; idx < base->reply_count; idx++) {
      row[col + idx] = (float)sums[idx];
      row[col + base->reply_count + idx] =
          (float)sqrt(fmax(0, squares[idx] - sums[idx] * sums[idx]));
      row[col + 2 * base->reply_count + idx] = (float)low[idx];
      row[col + 3 * base->reply_count + idx] = (float)high[idx];
    }
    col += 4 * base->reply_count;
    row[col++] = (float)(after - sums[0]);
    for (int est = 0; est < net->estimates; est++) {
      double mean = 0, square = 0, lo = INFINITY, hi = -INFINITY;
      for (int threshold = 0; threshold < VNT_PEG1_THRESHOLDS; threshold++) {
        const double k = vnt_peg1_thresholds[threshold];
        double mass = 0;
        for (int world = 0; world < worlds; world++) {
          const double value = est_value[est][world];
          if (k == 0) {
            mass += est_prob[world] * (value > 0 ? 1.0 : value == 0 ? 0.5 : 0);
          } else if (value > k) {
            mass += est_prob[world];
          }
        }
        row[col++] = (float)mass;
      }
      for (int world = 0; world < worlds; world++) {
        const double value = est_value[est][world];
        mean += est_prob[world] * value;
        square += est_prob[world] * value * value;
        lo = fmin(lo, value);
        hi = fmax(hi, value);
      }
      row[col++] = (float)mean;
      row[col++] = (float)sqrt(fmax(0, square - mean * mean));
      row[col++] = (float)lo;
      row[col++] = (float)hi;
    }
    if (col != base->inputs) {
      log_fatal("1PEG v3 input assembly mismatch");
    }
  }
}

// Head outputs and the selection value for one v3 input row. base_spread is
// the row's one-ply spread estimate (its input column v2_inputs - 1).
static double vnt_peg1_net3_value(const VntPeg1Net3 *net, const float *input,
                                  const VntPeg1Utility *utility, double *win,
                                  double *spread) {
  const VntPeg1Net *base = &net->v2.base;
  float hidden[VNT_PEG1_NET_MAX_WIDTH];
  for (int unit = 0; unit < base->width; unit++) {
    const float *weights = &base->first[(size_t)unit * base->inputs];
    float sum = base->bias[unit];
    for (int col = 0; col < base->inputs; col++) {
      sum += weights[col] * input[col];
    }
    hidden[unit] = fmaxf(sum, 0);
  }
  float raw[VNT_PEG1_NET3_OUTPUTS];
  for (int out = 0; out < VNT_PEG1_NET3_OUTPUTS; out++) {
    float sum = net->out_bias3[out];
    const float *weights = &net->out3[(size_t)out * base->width];
    for (int unit = 0; unit < base->width; unit++) {
      sum += weights[unit] * hidden[unit];
    }
    raw[out] = sum;
  }
  *win = 1.0 / (1.0 + exp(-(double)raw[0]));
  *spread = (double)input[net->v2_inputs - 1] + 20.0 * (double)raw[1];
  if (utility->w_spread < 0) {
    return 10.0 * (double)raw[2];
  }
  return sim_utility_blend(*win, double_to_equity(*spread), utility->w_win,
                           utility->w_spread, utility->scale);
}

// v3 chooser over the static top-K (optionally a cascade: stage-1 network
// shortlists its top entries, stage-2 network rescoring them). Returns the
// chosen static rank, or -1 when no placement exists (the mover passes).
static int vnt_peg1_util_choose(const VntPeg1Net3 *first,
                                const VntPeg1Net3 *second, int shortlist,
                                const VntPeg1Utility *utility, const Game *root,
                                MoveList *pool, Move *candidates, int *count,
                                VntPeg1Work *work, float *inputs,
                                float *inputs2, double *values, double *wins,
                                double *spreads, VntPeg1NetTiming *timing,
                                VntPeg1NetTiming *timing2) {
  int64_t start = ctimer_monotonic_ns();
  int pass_rank;
  *count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
  timing->movegen_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (*count == 0) {
    return -1;
  }
  const int scored =
      *count < first->v2.base.top_k ? *count : first->v2.base.top_k;
  start = ctimer_monotonic_ns();
  VntPeg1Prep prep;
  vnt_peg1_prep(root, work, &prep, true);
  timing->opp_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  vnt_peg1_net3_inputs(first, root, candidates, scored, work, &prep, inputs,
                       timing);
  start = ctimer_monotonic_ns();
  for (int candidate = 0; candidate < scored; candidate++) {
    values[candidate] =
        vnt_peg1_net3_value(first, &inputs[candidate * first->v2.base.inputs],
                            utility, &wins[candidate], &spreads[candidate]);
  }
  timing->head_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (second == NULL) {
    int choice = 0;
    for (int candidate = 1; candidate < scored; candidate++) {
      if (values[candidate] > values[choice]) {
        choice = candidate;
      }
    }
    return choice;
  }
  const int listed = scored < shortlist ? scored : shortlist;
  bool taken[VNT_PEG1_POOL] = {false};
  for (int slot = 0; slot < listed; slot++) {
    int best = -1;
    for (int candidate = 0; candidate < scored; candidate++) {
      if (!taken[candidate] && (best < 0 || values[candidate] > values[best])) {
        best = candidate;
      }
    }
    taken[best] = true;
  }
  int ranks[VNT_PEG1_POOL];
  Move short_moves[VNT_PEG1_POOL];
  int filled = 0;
  for (int candidate = 0; candidate < scored; candidate++) {
    if (taken[candidate]) {
      ranks[filled] = candidate;
      move_copy(&short_moves[filled++], &candidates[candidate]);
    }
  }
  vnt_peg1_net3_inputs(second, root, short_moves, filled, work, &prep, inputs2,
                       timing2);
  start = ctimer_monotonic_ns();
  int choice = 0;
  double best_value = -INFINITY;
  for (int slot = 0; slot < filled; slot++) {
    double win, spread;
    const double value =
        vnt_peg1_net3_value(second, &inputs2[slot * second->v2.base.inputs],
                            utility, &win, &spread);
    if (value > best_value) {
      best_value = value;
      choice = slot;
    }
  }
  timing2->head_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  return ranks[choice];
}

// Evaluates a v3 chooser on stored or fresh roots: decisions.csv (with the
// chosen move's estimated win probability and spread), choices.csv for
// peg1play and inputs.f32 for parity. Fields: first model, second model or
// "-", shortlist, w_spread (negative = margin mode), scale, positions
// prefix, output prefix, check.
static void vnt_peg1_util_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 9) {
    log_fatal("peg1util needs first model, second model|-, shortlist, "
              "w_spread, scale, positions prefix, output prefix, check");
  }
  VntPeg1Net3 *first = vnt_peg1_net3_load(string_splitter_get_item(fields, 1));
  const char *second_path = string_splitter_get_item(fields, 2);
  VntPeg1Net3 *second =
      strings_equal(second_path, "-") ? NULL : vnt_peg1_net3_load(second_path);
  const int shortlist = atoi(string_splitter_get_item(fields, 3));
  const VntPeg1Utility utility = {
      .w_win = 1.0,
      .w_spread = strtod(string_splitter_get_item(fields, 4), NULL),
      .scale = strtod(string_splitter_get_item(fields, 5), NULL)};
  const char *data = string_splitter_get_item(fields, 6);
  const char *prefix = string_splitter_get_item(fields, 7);
  const bool check = atoi(string_splitter_get_item(fields, 8)) != 0;
  if (utility.scale <= 0 || (second != NULL && shortlist < 1)) {
    log_fatal("peg1util invalid scale or shortlist");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *swapped = game_duplicate(root);
  VntPeg1Work work = vnt_peg1_work_create(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  const char *suffixes[] = {"decisions.csv", "choices.csv", "inputs.f32"};
  FILE *output[3];
  for (int idx = 0; idx < 3; idx++) {
    path = get_formatted_string("%s.%s", prefix, suffixes[idx]);
    output[idx] = vnt_fopen_new_or_die(path, "wb");
    free(path);
  }
  fprintf(output[0], "root,candidates,scored,choice,win,spread,movegen_ms,"
                     "world_ms,opp_ms,mover_ms,head_ms,stage2_ms,total_ms,"
                     "privacy_checked\n");
  const int width1 = first->v2.base.inputs;
  const int width2 = second == NULL ? 1 : second->v2.base.inputs;
  float *inputs = malloc_or_die(sizeof(float) * VNT_PEG1_POOL * width1);
  float *inputs2 = malloc_or_die(sizeof(float) * VNT_PEG1_POOL * width2);
  float *private_inputs = malloc_or_die(sizeof(float) * VNT_PEG1_POOL * width1);
  double values[VNT_PEG1_POOL], wins[VNT_PEG1_POOL], spreads[VNT_PEG1_POOL];
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first_row, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first_row, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1util invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1util invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1util CGP load failed");
    }
    Move candidates[VNT_PEG1_POOL];
    int count;
    VntPeg1NetTiming timing = {0}, timing2 = {0};
    const int64_t start = ctimer_monotonic_ns();
    const int choice = vnt_peg1_util_choose(
        first, second, shortlist, &utility, root, pool, candidates, &count,
        &work, inputs, inputs2, values, wins, spreads, &timing, &timing2);
    const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    if (count != expected) {
      log_fatal("peg1util candidate replay mismatch");
    }
    const int scored =
        count < first->v2.base.top_k ? count : first->v2.base.top_k;
    if (check) {
      int unseen[VALUE_NET_TILE_TYPES];
      vnt_peg1_public_unseen(root, unseen);
      int actual = -1, other = -1;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
        if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
          actual = ml;
        }
      }
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
        if (unseen[ml] > 0 && ml != actual) {
          other = ml;
        }
      }
      vnt_peg1_world(root, unseen, other < 0 ? actual : other, swapped);
      Move private_candidates[VNT_PEG1_POOL];
      int private_count;
      VntPeg1NetTiming ignored = {0}, ignored2 = {0};
      double private_values[VNT_PEG1_POOL], private_wins[VNT_PEG1_POOL],
          private_spreads[VNT_PEG1_POOL];
      const int private_choice = vnt_peg1_util_choose(
          first, second, shortlist, &utility, swapped, pool, private_candidates,
          &private_count, &work, private_inputs, inputs2, private_values,
          private_wins, private_spreads, &ignored, &ignored2);
      if (private_count != count || private_choice != choice ||
          memcmp(inputs, private_inputs, sizeof(float) * scored * width1) !=
              0) {
        log_fatal("1PEG utility chooser depends on the hidden last tile");
      }
    }
    double stage2_ms = timing2.world_ms + timing2.opp_ms + timing2.mover_ms +
                       timing2.scalar_ms + timing2.head_ms;
    fprintf(output[0],
            "%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d\n",
            root_id, count, scored, choice, choice >= 0 ? wins[choice] : 0,
            choice >= 0 ? spreads[choice] : 0, timing.movegen_ms,
            timing.world_ms, timing.opp_ms, timing.mover_ms, timing.head_ms,
            stage2_ms, total_ms, check);
    fprintf(output[1], "%d,0\n", root_id);
    if (choice > 0) {
      fprintf(output[1], "%d,%d\n", root_id, choice);
    }
    if (!fwrite_le_floats(inputs, (size_t)scored * width1, output[2])) {
      log_fatal("peg1util write failed");
    }
    for (int idx = 0; idx < 3; idx++) {
      fflush(output[idx]);
    }
  }
  free(line);
  free(inputs);
  free(inputs2);
  free(private_inputs);
  fclose(positions);
  for (int idx = 0; idx < 3; idx++) {
    fclose(output[idx]);
  }
  error_stack_destroy(errors);
  move_list_destroy(pool);
  vnt_peg1_work_destroy(&work);
  game_destroy(swapped);
  config_destroy(config);
  vnt_peg1_net3_destroy(first);
  if (second != NULL) {
    vnt_peg1_net3_destroy(second);
  }
}

// Full-game pairs. Each game is static self-play from the opening (the
// peg1data source procedure, without rejection). If a 1PEG arises, the
// mover's choice is made twice -- static top-1 and the v2 network -- and
// each branch is continued to game over with the common continuation
// (0 = static, 2 = fixed two-ply endgame); both final spreads come from the
// 1PEG mover's view. Games without a 1PEG are recorded as identical pairs.
static void vnt_peg1_games3(const StringSplitter *fields) {
  // Utility-mode (v3) full games: first model, second model or "-",
  // shortlist, w_spread (negative = margin mode), scale, then count, offset,
  // seed, depth and prefix as peg1games.
  if (string_splitter_get_number_of_items(fields) != 11) {
    log_fatal("peg1games3 needs first, second|-, shortlist, w_spread, scale, "
              "count, offset, seed, depth, prefix");
  }
  VntPeg1Net3 *first = vnt_peg1_net3_load(string_splitter_get_item(fields, 1));
  const char *second_path = string_splitter_get_item(fields, 2);
  VntPeg1Net3 *second =
      strings_equal(second_path, "-") ? NULL : vnt_peg1_net3_load(second_path);
  const int shortlist = atoi(string_splitter_get_item(fields, 3));
  const VntPeg1Utility utility = {
      .w_win = 1.0,
      .w_spread = strtod(string_splitter_get_item(fields, 4), NULL),
      .scale = strtod(string_splitter_get_item(fields, 5), NULL)};
  const int games = atoi(string_splitter_get_item(fields, 6));
  const int offset = atoi(string_splitter_get_item(fields, 7));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 8), NULL, 10);
  const int depth = atoi(string_splitter_get_item(fields, 9));
  const char *prefix = string_splitter_get_item(fields, 10);
  if (games < 1 || offset < 0 || (depth != 0 && depth != 2)) {
    log_fatal("peg1games invalid arguments");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true "
      "-wit true -rit true -ritmmap true -threads 1 "
      "-s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  Game *branch = game_duplicate(game);
  VntPeg1Work work = vnt_peg1_work_create(game);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  char *path = get_formatted_string("%s.fullgames.csv", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "w");
  free(path);
  fprintf(output, "game,source_seed,starter,peg1,mover,net_choice,candidates,"
                  "static_final,net_final,static_win,net_win,decision_ms,"
                  "continuation_depth\n");
  float *inputs =
      malloc_or_die(sizeof(float) * VNT_PEG1_POOL * first->v2.base.inputs);
  float *inputs2 = malloc_or_die(sizeof(float) * VNT_PEG1_POOL *
                                 (second == NULL ? 1 : second->v2.base.inputs));
  double values[VNT_PEG1_POOL], wins[VNT_PEG1_POOL], spreads[VNT_PEG1_POOL];
  for (int local = 0; local < games; local++) {
    const int game_id = offset + local;
    const uint64_t source_seed =
        vnt_mix(seed ^ vnt_mix((uint64_t)game_id) ^ vnt_mix(UINT64_C(918273)));
    game_reset(game);
    game_seed(game, source_seed);
    game_set_starting_player_index(game, game_id % 2);
    draw_starting_racks(game);
    int turns = 0;
    while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 1 &&
           turns < VNT_MAX_TURNS) {
      Move played;
      move_copy(&played, vnt_static_move(game, static_list));
      play_move(&played, game, NULL);
      turns++;
    }
    const bool peg1 =
        !game_over(game) && bag_get_letters(game_get_bag(game)) == 1;
    double finals[2] = {0, 0};
    int count = 0, choice = 0, mover = -1;
    double decision_ms = 0;
    if (peg1) {
      mover = game_get_player_on_turn_index(game);
      Move candidates[VNT_PEG1_POOL];
      VntPeg1NetTiming timing = {0}, timing2 = {0};
      const int64_t start = ctimer_monotonic_ns();
      choice = vnt_peg1_util_choose(
          first, second, shortlist, &utility, game, pool, candidates, &count,
          &work, inputs, inputs2, values, wins, spreads, &timing, &timing2);
      decision_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
      for (int arm = 0; arm < 2; arm++) {
        game_copy(branch, game);
        Move first;
        if (choice < 0) {
          // No placement: both policies must pass.
          move_set_as_pass(&first);
        } else {
          move_copy(&first, &candidates[arm == 0 ? 0 : choice]);
        }
        play_move(&first, branch, NULL);
        int steps = 0;
        while (!game_over(branch) && steps < VNT_MAX_TURNS) {
          Move next;
          if (depth == 2 && bag_get_letters(game_get_bag(branch)) == 0) {
            vnt_fixed_endgame(branch, &next, 2);
          } else {
            move_copy(&next, vnt_static_move(branch, static_list));
          }
          play_move(&next, branch, NULL);
          steps++;
        }
        if (!game_over(branch)) {
          log_fatal("peg1games continuation did not end");
        }
        finals[arm] = vnt_peg1_margin(branch, mover);
      }
    } else {
      while (!game_over(game) && turns < VNT_MAX_TURNS) {
        Move played;
        move_copy(&played, vnt_static_move(game, static_list));
        play_move(&played, game, NULL);
        turns++;
      }
      finals[0] = finals[1] = vnt_peg1_margin(game, 0);
    }
    fprintf(output, "%d,%llu,%d,%d,%d,%d,%d,%.9g,%.9g,%.1f,%.1f,%.9g,%d\n",
            game_id, (unsigned long long)source_seed, game_id % 2, peg1, mover,
            choice, count, finals[0], finals[1],
            finals[0] > 0   ? 1.0
            : finals[0] < 0 ? 0.0
                            : .5,
            finals[1] > 0   ? 1.0
            : finals[1] < 0 ? 0.0
                            : .5,
            decision_ms, depth);
    fflush(output);
  }
  free(inputs);
  free(inputs2);
  fclose(output);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  vnt_peg1_work_destroy(&work);
  game_destroy(branch);
  config_destroy(config);
  vnt_peg1_net3_destroy(first);
  if (second != NULL) {
    vnt_peg1_net3_destroy(second);
  }
}

// MAGPIE's own PEG greedy seed (peg_solve stage 0) as a 1PEG chooser, over
// every legal move or only the static top-64 placements. One thread, full
// scenario enumeration, rational opponent, no nested lookahead. The chosen
// move is played with the stored game's actual last tile and finished with
// both common continuations (static, fixed two-ply endgame).
static double vnt_peg1_finish(const Game *root, const Move *move, int seat,
                              int depth, Game *game, MoveList *static_list) {
  game_copy(game, root);
  play_move(move, game, NULL);
  int turns = 1;
  while (!game_over(game) && turns < VNT_MAX_TURNS) {
    Move next;
    if (depth == 2 && bag_get_letters(game_get_bag(game)) == 0) {
      vnt_fixed_endgame(game, &next, 2);
    } else {
      move_copy(&next, vnt_static_move(game, static_list));
    }
    play_move(&next, game, NULL);
    turns++;
  }
  if (!game_over(game)) {
    log_fatal("peg1greedy continuation did not end");
  }
  return vnt_peg1_margin(game, seat);
}

static void vnt_peg1_greedy_solve(const Game *root, const Move *candidates,
                                  int count, bool top64, Move *best,
                                  double *solve_ms) {
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  ErrorStack *errors = error_stack_create();
  const Move *only[VNT_PEG1_POOL];
  for (int idx = 0; idx < count; idx++) {
    only[idx] = &candidates[idx];
  }
  const PegArgs args = {.game = root,
                        .thread_control = control,
                        .num_threads = 1,
                        .time_budget_seconds = 0,
                        .greedy_seed_only = true,
                        .opp_model = PEG_OPP_RATIONAL,
                        .scenario_stride = 1,
                        .nested_enabled = false,
                        .only_moves = top64 ? only : NULL,
                        .n_only_moves = top64 ? count : 0};
  PegResult result;
  const int64_t start = ctimer_monotonic_ns();
  peg_solve(&args, &result, errors);
  *solve_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (!error_stack_is_empty(errors) || result.n_top_cands < 1) {
    error_stack_print_and_reset(errors);
    log_fatal("peg1greedy peg_solve failed");
  }
  move_copy(best, &result.best_move);
  peg_result_destroy(&result);
  error_stack_destroy(errors);
  thread_control_destroy(control);
}

static void vnt_peg1_greedy_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 4) {
    log_fatal("peg1greedy needs positions prefix, output prefix, check");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  const bool check = atoi(string_splitter_get_item(fields, 3)) != 0;
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *swapped = game_duplicate(root), *game = game_duplicate(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  path = get_formatted_string("%s.greedy.csv", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "w");
  free(path);
  fprintf(output, "root,variant,choice,pass,solve_ms,final_d0,win_d0,"
                  "final_d2,win_d2,privacy_checked,move\n");
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1greedy invalid position");
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1greedy invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1greedy CGP load failed");
    }
    Move candidates[VNT_PEG1_POOL];
    int current_pass;
    const int count = vnt_peg1_pool(root, pool, candidates, &current_pass);
    if (count != expected) {
      log_fatal("peg1greedy candidate replay mismatch");
    }
    for (int variant = 0; variant < 2; variant++) {
      const bool top64 = variant == 1;
      Move best;
      double solve_ms;
      vnt_peg1_greedy_solve(root, candidates, count, top64, &best, &solve_ms);
      if (check) {
        int unseen[VALUE_NET_TILE_TYPES];
        vnt_peg1_public_unseen(root, unseen);
        int actual = -1, other = -1;
        for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
          if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
            actual = ml;
          }
        }
        for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
          if (unseen[ml] > 0 && ml != actual) {
            other = ml;
          }
        }
        vnt_peg1_world(root, unseen, other < 0 ? actual : other, swapped);
        Move private_candidates[VNT_PEG1_POOL];
        int private_pass;
        const int private_count =
            vnt_peg1_pool(swapped, pool, private_candidates, &private_pass);
        Move private_best;
        double ignored;
        vnt_peg1_greedy_solve(swapped, private_candidates, private_count, top64,
                              &private_best, &ignored);
        if (compare_moves(&best, &private_best, true) != -1) {
          log_fatal("peg1greedy choice depends on the hidden last tile");
        }
      }
      int choice = -1;
      for (int idx = 0; idx < count && choice < 0; idx++) {
        if (compare_moves(&best, &candidates[idx], true) == -1) {
          choice = idx;
        }
      }
      const double final0 =
          vnt_peg1_finish(root, &best, seat, 0, game, static_list);
      const double final2 =
          vnt_peg1_finish(root, &best, seat, 2, game, static_list);
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &best,
                              game_get_ld(root), false);
      fprintf(output, "%d,%s,%d,%d,%.9g,%.9g,%.1f,%.9g,%.1f,%d,\"%s\"\n",
              root_id, top64 ? "top64" : "all", choice,
              move_get_type(&best) == GAME_EVENT_PASS, solve_ms, final0,
              final0 > 0   ? 1.0
              : final0 < 0 ? 0.0
                           : .5,
              final2,
              final2 > 0   ? 1.0
              : final2 < 0 ? 0.0
                           : .5,
              check, string_builder_peek(name));
    }
    fflush(output);
  }
  free(line);
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  game_destroy(swapped);
  game_destroy(game);
  config_destroy(config);
}

// ---- Exact continuation ----------------------------------------------------
// After a 1PEG pick is played with the stored game's actual last tile, one
// full-depth endgame_solve from the post-move position gives the final spread
// under optimal play by both sides. A pass leaves the tile in the bag; the
// player then on turn plays MAGPIE's default PEG pick (one thread, built-in
// schedule) until the bag is empty, and the endgame is then solved exactly.
// Experiment toggle for PR #819's best-reply shortcuts (-ebestreply): set
// VNT_EBESTREPLY=0 to disable them in the harness's PEG and exact solves.
static bool vnt_best_reply_disabled(void) {
  const char *value = getenv("VNT_EBESTREPLY");
  return value != NULL && strings_equal(value, "0");
}

// VNT_EXACT_CAP_S > 0 caps each solve's wall-clock time (the solver's
// external deadline, checked inside the search): a capped solve returns its
// last completed depth's value, is not proven, and records the depth it was
// searching (capped, depth).
typedef struct VntPeg1Exact {
  EndgameCtx *ctx;
  EndgameResults *results;
  TranspositionTable *tt;
  int plies;
  double cap_seconds;
  bool capped;
  int depth;
} VntPeg1Exact;

static VntPeg1Exact vnt_peg1_exact_create(int plies, double tt_fraction) {
  const char *cap = getenv("VNT_EXACT_CAP_S");
  VntPeg1Exact exact = {.ctx = NULL,
                        .results = endgame_results_create(),
                        .tt = transposition_table_create(tt_fraction),
                        .plies = plies,
                        .cap_seconds = cap != NULL ? strtod(cap, NULL) : 0};
  return exact;
}

static void vnt_peg1_exact_destroy(VntPeg1Exact *exact) {
  endgame_ctx_destroy(exact->ctx);
  endgame_results_destroy(exact->results);
  transposition_table_destroy(exact->tt);
}

// Value of the rest of the game for the player on turn; *proven when the
// solve stopped below the ply cap (root value proven).
static int vnt_peg1_exact_value(VntPeg1Exact *exact, const Game *game,
                                bool *proven) {
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  EndgameArgs args;
  endgame_args_fill(
      control, game, /*tt_fraction_of_mem=*/0.0, exact->plies,
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, /*num_threads=*/1,
      /*use_heuristics=*/true, /*num_top_moves=*/1,
      /*per_ply_callback=*/NULL, /*per_ply_callback_data=*/NULL,
      /*before_search_callback=*/NULL, /*before_search_callback_data=*/NULL,
      /*per_root_move_callback=*/NULL, /*per_root_move_callback_data=*/NULL,
      DUAL_LEXICON_MODE_IGNORANT, /*forced_pass_bypass=*/false,
      /*incremental_movegen=*/true, vnt_best_reply_disabled(),
      /*enable_pv_display=*/false,
      /*soft_time_limit=*/0.0, /*hard_time_limit=*/0.0, /*seed=*/1,
      /*skip_word_pruning=*/false, exact->tt, /*max_workers=*/0,
      /*first_win=*/false, /*first_win_fallback_moves=*/0,
      /*use_initial_window=*/false, /*initial_alpha=*/0, /*initial_beta=*/0,
      /*external_deadline_ns=*/0, /*actual_move=*/NULL, &args);
  const int64_t start = ctimer_monotonic_ns();
  if (exact->cap_seconds > 0) {
    args.external_deadline_ns = start + (int64_t)(exact->cap_seconds * 1e9);
  }
  ErrorStack *errors = error_stack_create();
  endgame_solve(&exact->ctx, &args, exact->results, errors);
  const double elapsed = (double)(ctimer_monotonic_ns() - start) / 1e9;
  if (!error_stack_is_empty(errors)) {
    error_stack_print_and_reset(errors);
    log_fatal("peg1exact endgame_solve failed");
  }
  error_stack_destroy(errors);
  thread_control_destroy(control);
  int depth = 0, root_done = 0, root_total = 0, ply2_done = 0, ply2_total = 0;
  endgame_ctx_get_progress(exact->ctx, &depth, &root_done, &root_total,
                           &ply2_done, &ply2_total);
  exact->capped =
      exact->cap_seconds > 0 && elapsed >= 0.98 * exact->cap_seconds;
  exact->depth = depth;
  *proven = depth < exact->plies && !exact->capped;
  return endgame_results_get_value(exact->results, ENDGAME_RESULT_BEST);
}

// MAGPIE's default PEG pick for the player on turn (config_fill_peg_args
// defaults, no time budget), optionally with a 1PEG chooser in place of the
// nested inner peg at one-tile nested levels.
// only/n_only: when n_only > 0, PEG solves only those root moves
// (PegArgs.only_moves); NULL/0 = the full root move list.
// VNT_PEG_TRACE=1: each PEG stage's start (stage, candidates, endgame plies,
// seconds since the solve began) goes to stderr.
static void vnt_peg_trace_stage(int stage_idx, int k_cands, int inner_d,
                                int emptier_plies, void *user_data) {
  (void)inner_d;
  const int64_t *start = user_data;
  fprintf(stderr, "peg stage %d cands %d plies %d at %.2f s\n", stage_idx,
          k_cands, emptier_plies,
          (double)(ctimer_monotonic_ns() - *start) / 1e9);
}

// chooser2/nest_depth: the 2PEG drop-in at two-tile nested nodes (NULL = the
// staged inner peg) and PegArgs.nested_max_depth.
static void vnt_peg_pick_nets(const Game *game, int threads,
                              PegPeg1Chooser chooser, void *chooser_ctx,
                              PegPeg2Chooser chooser2, void *chooser2_ctx,
                              int nest_depth, const Move *const *only,
                              int n_only, Move *best, double *ms) {
  static const int caps[] = {8, 4, 2};
  // VNT_PEG2_MAX_STAGE > 0 stops 2PEG picks (two or more tiles in the bag)
  // after that halving stage; 1PEG picks always run every stage.
  const char *stage_env = getenv("VNT_PEG2_MAX_STAGE");
  const int max_stage =
      stage_env != NULL && bag_get_letters(game_get_bag(game)) >= 2
          ? atoi(stage_env)
          : 0;
  // VNT_PEG2_BUDGET_S > 0 is a wall-clock backstop for those 2PEG picks: PEG
  // returns its last completed stage's ranking when the budget runs out.
  const char *budget_env = getenv("VNT_PEG2_BUDGET_S");
  const double budget =
      budget_env != NULL && bag_get_letters(game_get_bag(game)) >= 2
          ? strtod(budget_env, NULL)
          : 0;
  const char *trace_env = getenv("VNT_PEG_TRACE");
  const bool trace = trace_env != NULL && atoi(trace_env) != 0;
  int64_t start = 0;
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  PegArgs args;
  peg_args_fill(
      game, control, threads, budget, max_stage, /*greedy_seed_only=*/false,
      /*stage_top_k=*/NULL, /*num_stages=*/0, /*inner_top_k=*/0,
      PEG_OPP_RATIONAL, /*scenario_stride=*/0,
      /*nested_enabled=*/true, /*nested_cand_cap=*/0, caps,
      (int)(sizeof(caps) / sizeof(caps[0])), /*nested_stride=*/0,
      /*nested_strides_by_bag=*/NULL,
      /*nested_n_strides_by_bag=*/0,
      /*nested_emptier_ply_cap=*/0, nest_depth, vnt_best_reply_disabled(),
      chooser, chooser_ctx, chooser2, chooser2_ctx,
      /*eval_bag_order=*/NULL, /*eval_bag_order_len=*/0, only, n_only,
      /*n_ranked_only_moves=*/0, /*rank_by_utility=*/false,
      /*protect_moves=*/NULL, /*n_protect_moves=*/0,
      /*include_per_scenario=*/false, trace ? vnt_peg_trace_stage : NULL,
      /*on_cand_done=*/NULL, /*on_scenario_done=*/NULL, trace ? &start : NULL,
      /*poll=*/NULL,
      /*shared_endgame_tt=*/NULL, /*checkpoint=*/NULL,
      /*resume=*/false, &args);
  PegResult result;
  ErrorStack *errors = error_stack_create();
  start = ctimer_monotonic_ns();
  peg_solve(&args, &result, errors);
  *ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
  if (!error_stack_is_empty(errors) || result.n_top_cands < 1) {
    error_stack_print_and_reset(errors);
    log_fatal("peg1exact default peg_solve failed");
  }
  move_copy(best, &result.best_move);
  peg_result_destroy(&result);
  error_stack_destroy(errors);
  thread_control_destroy(control);
}

static void vnt_peg_pick_only(const Game *game, int threads,
                              PegPeg1Chooser chooser, void *chooser_ctx,
                              const Move *const *only, int n_only, Move *best,
                              double *ms) {
  vnt_peg_pick_nets(game, threads, chooser, chooser_ctx, NULL, NULL,
                    PEG_NESTED_DEFAULT_DEPTH, only, n_only, best, ms);
}

static void vnt_peg_pick(const Game *game, int threads, PegPeg1Chooser chooser,
                         void *chooser_ctx, Move *best, double *ms) {
  vnt_peg_pick_only(game, threads, chooser, chooser_ctx, NULL, 0, best, ms);
}

// Mover's final spread after `move` with the exact continuation. *peg_turns
// counts default-PEG turns taken while the bag still held the tile.
static double vnt_peg1_exact_finish(VntPeg1Exact *exact, const Game *root,
                                    const Move *move, int seat, Game *game,
                                    bool *proven, double *solve_ms,
                                    double *peg_ms, int *peg_turns) {
  game_copy(game, root);
  play_move(move, game, NULL);
  *peg_turns = 0;
  *peg_ms = 0;
  while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 0 &&
         *peg_turns < VNT_MAX_TURNS) {
    Move next;
    double ms;
    vnt_peg_pick(game, 1, NULL, NULL, &next, &ms);
    *peg_ms += ms;
    play_move(&next, game, NULL);
    (*peg_turns)++;
  }
  *proven = true;
  *solve_ms = 0;
  double spread = vnt_peg1_margin(game, seat);
  if (!game_over(game)) {
    if (bag_get_letters(game_get_bag(game)) != 0) {
      log_fatal("peg1exact bag not empty before the exact solve");
    }
    const int on_turn = game_get_player_on_turn_index(game);
    const int64_t start = ctimer_monotonic_ns();
    const int value = vnt_peg1_exact_value(exact, game, proven);
    *solve_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    spread += on_turn == seat ? value : -value;
  }
  return spread;
}

enum {
  VNT_PEG1_PICK_GREEDY_ALL = -1,
  VNT_PEG1_PICK_DEFAULT_PEG = -2,
  VNT_PEG1_PICK_DEFAULT_PEG_CHECKED = -3,
};

// Scores per-board picks with the exact continuation. Choices CSV rows:
// root,code where code >= 0 is a static rank in the top-64 placement pool,
// -1 the PEG greedy seed's pick over every move and -2 MAGPIE's default PEG
// pick (both recomputed here from public information). Output rows are
// appended and fsynced per board; a rerun skips (root, code) pairs already
// in the output.
static void vnt_peg1_exact_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 6) {
    log_fatal("peg1exact needs positions prefix, choices CSV, output prefix, "
              "plies, tt fraction");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const int plies = atoi(string_splitter_get_item(fields, 4));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 5), NULL);
  if (plies < 1 || plies > 25 || tt_fraction <= 0 || tt_fraction > 0.1) {
    log_fatal("peg1exact invalid plies or tt fraction");
  }
  // Load the picks and any finished output.
  enum { VNT_PEG1_MAX_PICKS = 1 << 18 };
  int *pick_root = malloc_or_die(sizeof(int) * VNT_PEG1_MAX_PICKS);
  int *pick_code = malloc_or_die(sizeof(int) * VNT_PEG1_MAX_PICKS);
  bool *pick_done = calloc_or_die(VNT_PEG1_MAX_PICKS, sizeof(bool));
  int num_picks = 0;
  char *line = NULL;
  size_t capacity = 0;
  FILE *choices = fopen_or_die(choices_path, "r");
  while (getline_ignore_carriage_return(&line, &capacity, choices) >= 0) {
    int root_id, code;
    if (sscanf(line, "%d,%d", &root_id, &code) == 2) {
      if (num_picks == VNT_PEG1_MAX_PICKS) {
        log_fatal("peg1exact too many picks");
      }
      pick_root[num_picks] = root_id;
      pick_code[num_picks++] = code;
    }
  }
  fclose(choices);
  char *out_path = get_formatted_string("%s.exact.csv", prefix);
  FILE *previous = fopen(out_path, "r");
  bool fresh = previous == NULL;
  if (previous != NULL) {
    // A run killed before its first flush leaves an empty file: write the
    // header again.
    fresh = fgetc(previous) == EOF;
    rewind(previous);
  }
  if (previous != NULL) {
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id, code;
      if (sscanf(line, "%d,%d", &root_id, &code) == 2) {
        for (int idx = 0; idx < num_picks; idx++) {
          if (pick_root[idx] == root_id && pick_code[idx] == code) {
            pick_done[idx] = true;
          }
        }
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  if (fresh) {
    fprintf(output, "root,code,choice,pass,final,win,proven,solve_ms,peg_ms,"
                    "peg_turns,pick_ms,move\n");
  }
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit true -ritmmap true -threads 1 -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *root = config_get_game(config);
  Game *game = game_duplicate(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  VntPeg1Exact exact = vnt_peg1_exact_create(plies, tt_fraction);
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (getline_ignore_carriage_return(&line, &capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, expected, pass_rank;
    unsigned long long source_seed;
    if (sscanf(line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &source_seed,
               &attempt, &turn, &seat, &bag, &first, &expected,
               &pass_rank) != 9 ||
        bag != 1) {
      log_fatal("peg1exact invalid position");
    }
    bool pending = false;
    for (int idx = 0; idx < num_picks; idx++) {
      pending |= pick_root[idx] == root_id && !pick_done[idx];
    }
    if (!pending) {
      continue;
    }
    char *cgp = strchr(line, '"'), *end = strrchr(line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg1exact invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != 1) {
      log_fatal("peg1exact CGP load failed");
    }
    Move candidates[VNT_PEG1_POOL];
    int current_pass;
    const int count = vnt_peg1_pool(root, pool, candidates, &current_pass);
    if (count != expected) {
      log_fatal("peg1exact candidate replay mismatch");
    }
    for (int idx = 0; idx < num_picks; idx++) {
      if (pick_root[idx] != root_id || pick_done[idx]) {
        continue;
      }
      const int code = pick_code[idx];
      Move pick;
      double pick_ms = 0;
      if (code >= 0) {
        if (code >= count) {
          log_fatal("peg1exact invalid candidate");
        }
        move_copy(&pick, &candidates[code]);
      } else if (code == VNT_PEG1_PICK_GREEDY_ALL) {
        vnt_peg1_greedy_solve(root, candidates, count, false, &pick, &pick_ms);
      } else if (code == VNT_PEG1_PICK_DEFAULT_PEG) {
        vnt_peg_pick(root, 1, NULL, NULL, &pick, &pick_ms);
      } else if (code == VNT_PEG1_PICK_DEFAULT_PEG_CHECKED) {
        // Information rule: the pick must not change when the actual last
        // tile is swapped with a tile from the opponent's rack.
        vnt_peg_pick(root, 1, NULL, NULL, &pick, &pick_ms);
        int unseen[VALUE_NET_TILE_TYPES];
        vnt_peg1_public_unseen(root, unseen);
        int actual = -1, other = -1;
        for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
          if (bag_get_letter(game_get_bag(root), (MachineLetter)ml) > 0) {
            actual = ml;
          }
        }
        for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
          if (unseen[ml] > 0 && ml != actual) {
            other = ml;
          }
        }
        vnt_peg1_world(root, unseen, other < 0 ? actual : other, game);
        Move private_pick;
        double ignored;
        vnt_peg_pick(game, 1, NULL, NULL, &private_pick, &ignored);
        if (compare_moves(&pick, &private_pick, true) != -1) {
          log_fatal("default PEG pick depends on the hidden last tile");
        }
      } else {
        log_fatal("peg1exact invalid pick code");
      }
      int choice = -1;
      for (int cand = 0; cand < count && choice < 0; cand++) {
        if (compare_moves(&pick, &candidates[cand], true) == -1) {
          choice = cand;
        }
      }
      bool proven;
      double solve_ms, peg_ms;
      int peg_turns;
      const double final =
          vnt_peg1_exact_finish(&exact, root, &pick, seat, game, &proven,
                                &solve_ms, &peg_ms, &peg_turns);
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      fprintf(output, "%d,%d,%d,%d,%.9g,%.1f,%d,%.9g,%.9g,%d,%.9g,\"%s\"\n",
              root_id, code, choice, move_get_type(&pick) == GAME_EVENT_PASS,
              final,
              final > 0   ? 1.0
              : final < 0 ? 0.0
                          : .5,
              proven, solve_ms, peg_ms, peg_turns, pick_ms,
              string_builder_peek(name));
      pick_done[idx] = true;
    }
    fflush(output);
    fsync(fileno(output));
  }
  free(line);
  fclose(positions);
  fclose(output);
  vnt_peg1_exact_destroy(&exact);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  game_destroy(game);
  config_destroy(config);
  free(pick_root);
  free(pick_code);
  free(pick_done);
}

// ---- 2PEG data and the 1PEG-network drop-in for nested PEG -----------------
// The 1PEG network (cascade-u by default) picks the move of whichever player
// faces a one-tile bag, always from that player's own information: its rack
// and the public state. It serves three places:
//  - as PegArgs.nested_peg1_chooser, replacing MAGPIE's staged inner peg when
//    the opponent of a 2PEG candidate (or of a 1PEG pass) faces a 1PEG;
//  - as the opponent's reply in 2PEG training labels (peg2data);
//  - optionally as the reply policy when 2PEG picks are scored (peg2eval).
enum {
  VNT_PEG2_UNSEEN = RACK_SIZE + 2,
  VNT_PEG2_PICK_DEFAULT_PEG = -2,
  VNT_PEG2_PICK_NET_PEG = -4,
  VNT_PEG2_PICK_NET_PEG_CHECKED = -5,
  VNT_PEG2_PICK_NET_PEG_UNCACHED = -6,
  // Most candidates a VNT_PEG2_ONLY line may list for one root.
  VNT_PEG2_ONLY_MAX = 64,
  // Pick cache key: board letters, the on-turn rack, both scores, the on-turn
  // index and the scoreless-turn count -- everything the network reads.
  VNT_PEG_KEY_BYTES = BOARD_DIM * BOARD_DIM + VALUE_NET_TILE_TYPES + 2 * 4 + 2,
  VNT_PEG_CACHE_SLOTS = 4096,
};

typedef enum {
  VNT_PEG2_REPLY_NET,
  VNT_PEG2_REPLY_PEG,
} VntPeg2Reply;

typedef struct VntPegNetSlot {
  VntPeg1Work work;
  MoveList *pool;
  float *inputs;
  float *inputs2;
  Game *swap;
  // Open-addressing pick cache, cleared per root decision (vnt_peg_net_reset).
  uint8_t *keys;
  Move *picks;
  bool *used;
  int cached;
  int64_t calls;
  int64_t hits;
  int64_t checks;
  double ms;
} VntPegNetSlot;

typedef struct VntPegNet {
  VntPeg1Net3 *first;
  VntPeg1Net3 *second;
  int shortlist;
  VntPeg1Utility utility;
  bool check;
  bool cache;
  int slots;
  VntPegNetSlot *slot;
} VntPegNet;

static VntPegNet *vnt_peg_net_create(const char *first, const char *second,
                                     int shortlist, double w_spread,
                                     double scale, const Game *game,
                                     int slots) {
  VntPegNet *net = calloc_or_die(1, sizeof(VntPegNet));
  net->first = vnt_peg1_net3_load(first);
  net->second = strings_equal(second, "-") ? NULL : vnt_peg1_net3_load(second);
  net->shortlist = shortlist;
  net->utility =
      (VntPeg1Utility){.w_win = 1.0, .w_spread = w_spread, .scale = scale};
  net->slots = slots;
  net->slot = calloc_or_die((size_t)slots, sizeof(VntPegNetSlot));
  for (int slot_idx = 0; slot_idx < slots; slot_idx++) {
    VntPegNetSlot *slot = &net->slot[slot_idx];
    slot->work = vnt_peg1_work_create(game);
    slot->pool = move_list_create(VNT_PEG1_POOL + 1);
    slot->inputs = malloc_or_die(sizeof(float) * VNT_PEG1_POOL *
                                 net->first->v2.base.inputs);
    slot->inputs2 =
        malloc_or_die(sizeof(float) * VNT_PEG1_POOL *
                      (net->second == NULL ? 1 : net->second->v2.base.inputs));
    slot->swap = game_duplicate(game);
    slot->keys = malloc_or_die((size_t)VNT_PEG_CACHE_SLOTS * VNT_PEG_KEY_BYTES);
    slot->picks = malloc_or_die(sizeof(Move) * VNT_PEG_CACHE_SLOTS);
    slot->used = calloc_or_die(VNT_PEG_CACHE_SLOTS, sizeof(bool));
  }
  return net;
}

static void vnt_peg_net_reset(VntPegNet *net) {
  for (int slot_idx = 0; slot_idx < net->slots; slot_idx++) {
    memset(net->slot[slot_idx].used, 0, VNT_PEG_CACHE_SLOTS * sizeof(bool));
    net->slot[slot_idx].cached = 0;
  }
}

static uint64_t vnt_peg_net_key(const Game *game, uint8_t *key) {
  const int on_turn = game_get_player_on_turn_index(game);
  const Board *board = game_get_board(game);
  int pos = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      key[pos++] = board_get_letter(board, row, col);
    }
  }
  const Rack *rack = player_get_rack(game_get_player(game, on_turn));
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    key[pos++] = (uint8_t)rack_get_letter(rack, (MachineLetter)ml);
  }
  for (int player = 0; player < 2; player++) {
    const int32_t score = player_get_score(game_get_player(game, player));
    memcpy(&key[pos], &score, sizeof(score));
    pos += 4;
  }
  key[pos++] = (uint8_t)on_turn;
  key[pos++] = (uint8_t)game_get_consecutive_scoreless_turns(game);
  uint64_t hash = UINT64_C(1469598103934665603);
  for (int idx = 0; idx < VNT_PEG_KEY_BYTES; idx++) {
    hash = (hash ^ key[idx]) * UINT64_C(1099511628211);
  }
  return hash;
}

static void vnt_peg_net_destroy(VntPegNet *net) {
  for (int slot_idx = 0; slot_idx < net->slots; slot_idx++) {
    VntPegNetSlot *slot = &net->slot[slot_idx];
    vnt_peg1_work_destroy(&slot->work);
    move_list_destroy(slot->pool);
    free(slot->inputs);
    free(slot->inputs2);
    game_destroy(slot->swap);
    free(slot->keys);
    free(slot->picks);
    free(slot->used);
  }
  free(net->slot);
  vnt_peg1_net3_destroy(net->first);
  if (net->second != NULL) {
    vnt_peg1_net3_destroy(net->second);
  }
  free(net);
}

static int vnt_peg_net_choose(VntPegNet *net, VntPegNetSlot *slot,
                              const Game *game, Move *move) {
  Move candidates[VNT_PEG1_POOL];
  double values[VNT_PEG1_POOL], wins[VNT_PEG1_POOL], spreads[VNT_PEG1_POOL];
  VntPeg1NetTiming timing = {0}, timing2 = {0};
  int count;
  const int choice = vnt_peg1_util_choose(
      net->first, net->second, net->shortlist, &net->utility, game, slot->pool,
      candidates, &count, &slot->work, slot->inputs, slot->inputs2, values,
      wins, spreads, &timing, &timing2);
  if (choice < 0) {
    move_set_as_pass(move);
  } else {
    move_copy(move, &candidates[choice]);
  }
  return choice;
}

// Network pick for the player on turn when the bag holds one tile and eight
// tiles are unseen to that player; false otherwise. With net->check, the pick
// is recomputed with the bag tile swapped for another hidden tile (from the
// other player's rack) and must not change.
static bool vnt_peg_net_pick(VntPegNet *net, int slot_idx, const Game *game,
                             Move *move) {
  int unseen[VALUE_NET_TILE_TYPES];
  if (game_over(game) || bag_get_letters(game_get_bag(game)) != 1 ||
      vnt_peg1_public_unseen(game, unseen) != RACK_SIZE + 1) {
    return false;
  }
  VntPegNetSlot *slot = &net->slot[slot_idx];
  const int64_t start = ctimer_monotonic_ns();
  uint8_t key[VNT_PEG_KEY_BYTES];
  int entry = -1;
  bool hit = false;
  if (net->cache) {
    entry = (int)(vnt_peg_net_key(game, key) % VNT_PEG_CACHE_SLOTS);
    while (slot->used[entry] &&
           memcmp(&slot->keys[(size_t)entry * VNT_PEG_KEY_BYTES], key,
                  VNT_PEG_KEY_BYTES) != 0) {
      entry = (entry + 1) % VNT_PEG_CACHE_SLOTS;
    }
    hit = slot->used[entry];
  }
  if (hit) {
    move_copy(move, &slot->picks[entry]);
    slot->hits++;
  } else {
    vnt_peg_net_choose(net, slot, game, move);
    // Keep the table at most half full so probes stay short.
    if (net->cache && slot->cached < VNT_PEG_CACHE_SLOTS / 2) {
      memcpy(&slot->keys[(size_t)entry * VNT_PEG_KEY_BYTES], key,
             VNT_PEG_KEY_BYTES);
      move_copy(&slot->picks[entry], move);
      slot->used[entry] = true;
      slot->cached++;
    }
  }
  slot->ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  slot->calls++;
  if (net->check && hit) {
    Move fresh;
    vnt_peg_net_choose(net, slot, game, &fresh);
    if (compare_moves(move, &fresh, true) != -1) {
      log_fatal("1PEG network pick cache returned a different move");
    }
  }
  if (net->check) {
    int actual = -1, other = -1;
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      if (bag_get_letter(game_get_bag(game), (MachineLetter)ml) > 0) {
        actual = ml;
      }
    }
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
      if (unseen[ml] > 0 && ml != actual) {
        other = ml;
      }
    }
    if (other >= 0) {
      vnt_peg1_world(game, unseen, other, slot->swap);
      Move swapped;
      vnt_peg_net_choose(net, slot, slot->swap, &swapped);
      if (compare_moves(move, &swapped, true) != -1) {
        log_fatal("1PEG network pick depends on the hidden tiles");
      }
      slot->checks++;
    }
  }
  return true;
}

static bool vnt_peg_net_chooser(const Game *game, int worker_idx, void *ctx,
                                Move *move) {
  VntPegNet *net = ctx;
  if (worker_idx < 0 || worker_idx >= net->slots) {
    log_fatal("1PEG network chooser has no slot for PEG worker %d", worker_idx);
  }
  return vnt_peg_net_pick(net, worker_idx, game, move);
}

static void vnt_peg_net_totals(const VntPegNet *net, int64_t *calls,
                               int64_t *checks, double *ms) {
  *calls = 0;
  *checks = 0;
  *ms = 0;
  for (int slot_idx = 0; slot_idx < net->slots; slot_idx++) {
    *calls += net->slot[slot_idx].calls;
    *checks += net->slot[slot_idx].checks;
    *ms += net->slot[slot_idx].ms;
  }
}

static int64_t vnt_peg_net_hits(const VntPegNet *net) {
  int64_t hits = 0;
  for (int slot_idx = 0; slot_idx < net->slots; slot_idx++) {
    hits += net->slot[slot_idx].hits;
  }
  return hits;
}

// Post-candidate 2PEG world: the bag held bag2[0..1]; the opponent's rack is
// the mover's unseen pool minus those two; the mover draws the first n_drawn
// of them (the candidate's tiles played, capped at two) and the bag keeps the
// rest. The opponent is then on turn.
static void vnt_peg2_world(const Game *root, const int *unseen,
                           const Move *cand, const MachineLetter *bag2,
                           int n_drawn, Game *world) {
  const int mover = game_get_player_on_turn_index(root);
  game_copy(world, root);
  int rest[VALUE_NET_TILE_TYPES];
  memcpy(rest, unseen, sizeof(rest));
  rest[bag2[0]]--;
  rest[bag2[1]]--;
  Rack *opponent = player_get_rack(game_get_player(world, 1 - mover));
  rack_reset(opponent);
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    if (rest[ml] < 0) {
      log_fatal("peg2 world draws an unavailable tile");
    }
    for (int copy = 0; copy < rest[ml]; copy++) {
      rack_add_letter(opponent, (MachineLetter)ml);
    }
  }
  play_move_without_drawing_tiles(cand, world);
  Rack *own = player_get_rack(game_get_player(world, mover));
  for (int idx = 0; idx < n_drawn; idx++) {
    rack_add_letter(own, bag2[idx]);
  }
  bag_set_to_tiles(game_get_bag(world), bag2 + n_drawn, 2 - n_drawn);
  if (!rack_is_empty(own)) {
    game_set_game_end_reason(world, GAME_END_REASON_NONE);
  }
}

// Plays the turns that still draw: a one-tile bag by the reply policy, a
// two-tile bag (only after a pass) by default PEG or static top 1.
static int vnt_peg2_drain(Game *game, VntPegNet *net, VntPeg2Reply reply,
                          MoveList *static_list, double *reply_ms) {
  int turns = 0;
  while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 0 &&
         turns < VNT_MAX_TURNS) {
    Move next;
    double ms = 0;
    const int64_t start = ctimer_monotonic_ns();
    if (reply == VNT_PEG2_REPLY_NET && vnt_peg_net_pick(net, 0, game, &next)) {
      ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    } else if (reply == VNT_PEG2_REPLY_PEG) {
      vnt_peg_pick(game, 1, NULL, NULL, &next, &ms);
    } else {
      move_copy(&next, vnt_static_move(game, static_list));
    }
    *reply_ms += ms;
    play_move(&next, game, NULL);
    turns++;
  }
  return turns;
}

// Mover's final spread after a fixed-depth endgame from a bag-empty position
// (either player on turn).
static double vnt_peg2_depth_label(Game *game, int mover, int plies,
                                   EndgameCtx **ctx, EndgameResults *results,
                                   ThreadControl *control) {
  const double margin = vnt_peg1_margin(game, mover);
  if (game_over(game)) {
    return margin;
  }
  if (bag_get_letters(game_get_bag(game)) != 0) {
    log_fatal("peg2 label before the bag is empty");
  }
  const EndgameArgs args = {.game = game,
                            .thread_control = control,
                            .plies = plies,
                            .tt_fraction_of_mem = 0,
                            .initial_small_move_arena_size =
                                DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                            .num_threads = 1,
                            .use_heuristics = true,
                            .num_top_moves = 1,
                            .incremental_movegen = true,
                            .enable_pv_display = true,
                            .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT};
  endgame_solve_inline(ctx, &args, results);
  if (endgame_results_get_status(results) != ENDGAME_RESULT_STATUS_FINISHED) {
    log_fatal("peg2 incomplete endgame label");
  }
  const double value = endgame_results_get_value(results, ENDGAME_RESULT_BEST);
  return game_get_player_on_turn_index(game) == mover ? margin + value
                                                      : margin - value;
}

// The rack info table is off: 2PEG searches sort by score, so it only adds
// cold page faults on its 1.9 GB map (leave values are unchanged without it).
static Config *vnt_peg2_config(void) {
  Config *config = config_create_or_die(
      "set -path ./collection-runtime-data:./data -lex NWL23 -wmp true -wit "
      "true -rit false -threads 1 -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  return config;
}

// Fresh PEG boards from static self-play: the first turn with bag_size tiles
// in the bag (both racks full). Fields: count, offset, seed, output prefix.
static void vnt_pegn_positions(const StringSplitter *fields, int bag_size) {
  if (string_splitter_get_number_of_items(fields) != 5) {
    log_fatal("peg%dpos needs count, offset, seed and prefix", bag_size);
  }
  const int roots = atoi(string_splitter_get_item(fields, 1));
  const int offset = atoi(string_splitter_get_item(fields, 2));
  const uint64_t seed = strtoull(string_splitter_get_item(fields, 3), NULL, 10);
  const char *prefix = string_splitter_get_item(fields, 4);
  if (roots < 1 || offset < 0) {
    log_fatal("peg2pos invalid count or offset");
  }
  Config *config = vnt_peg2_config();
  Game *game = config_get_game(config);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  char *path = get_formatted_string("%s.positions.csv", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "w");
  free(path);
  fprintf(output, "root,source_seed,source_attempt,turn,seat,bag,first_row,"
                  "rows,pass_rank,movegen_ms,cgp\n");
  for (int local = 0; local < roots; local++) {
    const int root_id = offset + local;
    uint64_t source_seed = 0;
    int attempt = 0, turns = 0, count = 0, pass_rank = -1;
    double movegen_ms = 0;
    bool ready = false;
    while (!ready) {
      if (++attempt > 1000) {
        log_fatal("peg2 source generation failed");
      }
      source_seed = vnt_mix(seed ^ vnt_mix((uint64_t)root_id) ^
                            vnt_mix((uint64_t)attempt + UINT64_C(748239)));
      game_reset(game);
      game_seed(game, source_seed);
      game_set_starting_player_index(game, root_id % 2);
      draw_starting_racks(game);
      turns = 0;
      while (!game_over(game) &&
             bag_get_letters(game_get_bag(game)) > bag_size &&
             turns < VNT_MAX_TURNS) {
        Move move;
        move_copy(&move, vnt_static_move(game, static_list));
        play_move(&move, game, NULL);
        turns++;
      }
      if (!game_over(game) && bag_get_letters(game_get_bag(game)) == bag_size) {
        Move candidates[VNT_PEG1_POOL];
        const int64_t start = ctimer_monotonic_ns();
        count = vnt_peg1_pool(game, pool, candidates, &pass_rank);
        movegen_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        int unseen[VALUE_NET_TILE_TYPES];
        ready = count > 0 &&
                vnt_peg1_public_unseen(game, unseen) == RACK_SIZE + bag_size;
      }
    }
    char *cgp = game_get_cgp(game, false);
    fprintf(output, "%d,%llu,%d,%d,%d,%d,0,%d,%d,%.9g,\"%s\"\n", root_id,
            (unsigned long long)source_seed, attempt, turns,
            game_get_player_on_turn_index(game), bag_size, count, pass_rank,
            movegen_ms, cgp);
    free(cgp);
    fflush(output);
  }
  fclose(output);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  config_destroy(config);
}

static void vnt_peg2_positions(const StringSplitter *fields) {
  vnt_pegn_positions(fields, 2);
}

// Loads one positions row (peg1/peg2 layout) into root; returns the root id or
// -1 at end of file.
static int vnt_peg2_next_root(FILE *positions, char **line, size_t *capacity,
                              Game *root, ErrorStack *errors, int bag_size,
                              int *expected, uint64_t *source_seed) {
  while (getline_ignore_carriage_return(line, capacity, positions) >= 0) {
    int root_id, attempt, turn, seat, bag, first, pass_rank;
    unsigned long long seed;
    if (sscanf(*line, "%d,%llu,%d,%d,%d,%d,%d,%d,%d,", &root_id, &seed,
               &attempt, &turn, &seat, &bag, &first, expected,
               &pass_rank) != 9 ||
        bag != bag_size) {
      log_fatal("peg2 invalid position row");
    }
    char *cgp = strchr(*line, '"'), *end = strrchr(*line, '"');
    if (cgp == NULL || end <= cgp) {
      log_fatal("peg2 invalid CGP");
    }
    *end = '\0';
    game_load_cgp(root, cgp + 1, errors);
    game_set_player_on_turn_index(root, seat);
    if (!error_stack_is_empty(errors) ||
        bag_get_letters(game_get_bag(root)) != bag_size) {
      log_fatal("peg2 CGP load failed");
    }
    *source_seed = seed;
    return root_id;
  }
  return -1;
}

// The v3 networks' head outputs for every scored candidate of stored 1PEG
// roots: estimated win probability and spread of each static top-K
// placement, from each network on its own (no cascade). Fields: fast model,
// strong model, positions prefix, output prefix, max rank. Writes
// PREFIX.pred.csv: root, rank, candidates, tiles, each network's win and
// spread (empty past its top K), quoted move.
static void vnt_peg1_predict(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 6) {
    log_fatal("peg1pred needs fast model, strong model, positions prefix, "
              "output prefix, max rank");
  }
  VntPeg1Net3 *nets[2] = {
      vnt_peg1_net3_load(string_splitter_get_item(fields, 1)),
      vnt_peg1_net3_load(string_splitter_get_item(fields, 2))};
  const char *data = string_splitter_get_item(fields, 3);
  const char *prefix = string_splitter_get_item(fields, 4);
  const int max_rank = atoi(string_splitter_get_item(fields, 5));
  if (max_rank < 1 || max_rank > VNT_PEG1_POOL) {
    log_fatal("peg1pred invalid max rank");
  }
  const VntPeg1Utility utility = {.w_win = 1.0, .w_spread = 0.5, .scale = 100};
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  VntPeg1Work work = vnt_peg1_work_create(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  path = get_formatted_string("%s.pred.csv", prefix);
  FILE *output = vnt_fopen_new_or_die(path, "wb");
  free(path);
  fprintf(output, "root,rank,candidates,tiles,fast_win,fast_spread,strong_win,"
                  "strong_spread,move\n");
  int max_inputs = 1;
  for (int net_idx = 0; net_idx < 2; net_idx++) {
    if (nets[net_idx]->v2.base.inputs > max_inputs) {
      max_inputs = nets[net_idx]->v2.base.inputs;
    }
  }
  float *inputs = malloc_or_die(sizeof(float) * VNT_PEG1_POOL * max_inputs);
  double values[VNT_PEG1_POOL], wins[2][VNT_PEG1_POOL],
      spreads[2][VNT_PEG1_POOL];
  int scored[2];
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = vnt_peg2_next_root(positions, &line, &capacity, root,
                                           errors, 1, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    Move candidates[VNT_PEG1_POOL];
    int count = 0;
    for (int net_idx = 0; net_idx < 2; net_idx++) {
      VntPeg1NetTiming timing = {0}, timing2 = {0};
      (void)vnt_peg1_util_choose(nets[net_idx], NULL, 0, &utility, root, pool,
                                 candidates, &count, &work, inputs, NULL,
                                 values, wins[net_idx], spreads[net_idx],
                                 &timing, &timing2);
      scored[net_idx] = count < nets[net_idx]->v2.base.top_k
                            ? count
                            : nets[net_idx]->v2.base.top_k;
    }
    if (count != expected) {
      log_fatal("peg1pred candidate replay mismatch");
    }
    for (int rank = 0; rank < count && rank < max_rank; rank++) {
      fprintf(output, "%d,%d,%d,%d", root_id, rank, count,
              move_get_tiles_played(&candidates[rank]));
      for (int net_idx = 0; net_idx < 2; net_idx++) {
        if (rank < scored[net_idx]) {
          fprintf(output, ",%.9g,%.9g", wins[net_idx][rank],
                  spreads[net_idx][rank]);
        } else {
          fprintf(output, ",,");
        }
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &candidates[rank],
                              game_get_ld(root), false);
      fprintf(output, ",\"%s\"\n", string_builder_peek(name));
    }
    fflush(output);
  }
  free(line);
  free(inputs);
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  vnt_peg1_work_destroy(&work);
  config_destroy(config);
  for (int net_idx = 0; net_idx < 2; net_idx++) {
    vnt_peg1_net3_destroy(nets[net_idx]);
  }
}

// ---- Wide 2PEG candidates ---------------------------------------------------
// A wide rank is a placement's index in the root's full move list sorted by
// equity, with the pass left out. compare_moves is a total order, so the
// first VNT_PEG1_POOL wide ranks are exactly vnt_peg1_pool's candidates and
// keep their ids. A list longer than VNT_PEG2_WIDE_CAP keeps the best by
// equity.
enum { VNT_PEG2_WIDE_CAP = 16384 };

static int vnt_peg2_wide_pool(const Game *game, MoveList *wide, Move *moves) {
  move_list_reset(wide);
  const MoveGenArgs args = {.game = game,
                            .move_list = wide,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  move_list_sort_moves(wide);
  int count = 0;
  for (int rank = 0; rank < move_list_get_count(wide); rank++) {
    const Move *move = move_list_get_move(wide, rank);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      // Exchanges are never candidates (legal only with a full rack's worth
      // of tiles in the bag).
      if (bag_get_letters(game_get_bag(game)) < RACK_SIZE) {
        log_fatal("peg2 unexpected exchange");
      }
      continue;
    }
    move_copy(&moves[count++], move);
  }
  return count;
}

// Optional candidate lists (env VNT_PEG2_RANKS: a CSV of root,wide rank
// rows). peg2data and peg2feat then score exactly the listed ranks of each
// board, in file order, instead of the static top K.
typedef struct VntPeg2Ranks {
  int count;
  int *root;
  int *rank;
} VntPeg2Ranks;

static VntPeg2Ranks vnt_peg2_ranks_load(void) {
  VntPeg2Ranks ranks = {.count = -1, .root = NULL, .rank = NULL};
  const char *path = getenv("VNT_PEG2_RANKS");
  if (path == NULL || path[0] == '\0') {
    return ranks;
  }
  FILE *file = fopen_or_die(path, "r");
  int capacity = 1024;
  ranks.count = 0;
  ranks.root = malloc_or_die(sizeof(int) * capacity);
  ranks.rank = malloc_or_die(sizeof(int) * capacity);
  char *line = NULL;
  size_t line_capacity = 0;
  while (getline_ignore_carriage_return(&line, &line_capacity, file) >= 0) {
    int root_id, rank;
    if (sscanf(line, "%d,%d", &root_id, &rank) != 2) {
      continue;
    }
    if (rank < 0) {
      log_fatal("peg2 rank list has a negative rank");
    }
    if (ranks.count == capacity) {
      capacity *= 2;
      ranks.root = realloc_or_die(ranks.root, sizeof(int) * capacity);
      ranks.rank = realloc_or_die(ranks.rank, sizeof(int) * capacity);
    }
    ranks.root[ranks.count] = root_id;
    ranks.rank[ranks.count++] = rank;
  }
  free(line);
  fclose(file);
  return ranks;
}

static void vnt_peg2_ranks_destroy(VntPeg2Ranks *ranks) {
  free(ranks->root);
  free(ranks->rank);
}

// The candidates one board scores: the static top K, or (with a rank list)
// the listed wide ranks. Returns how many; chosen[slot] is each one's move
// and ids[slot] its candidate id. *wide_count is the length of the list the
// ids index (the static pool, or the wide list).
static int vnt_peg2_board_candidates(const Game *root, int root_id,
                                     const VntPeg2Ranks *ranks,
                                     const Move *pool_moves, int count,
                                     int top_k, MoveList *wide,
                                     Move *wide_moves, int *wide_count,
                                     const Move **chosen, int *ids) {
  if (ranks->count < 0) {
    const int scored = count < top_k ? count : top_k;
    for (int slot = 0; slot < scored; slot++) {
      chosen[slot] = &pool_moves[slot];
      ids[slot] = slot;
    }
    *wide_count = count;
    return scored;
  }
  *wide_count = vnt_peg2_wide_pool(root, wide, wide_moves);
  for (int rank = 0; rank < count; rank++) {
    if (compare_moves(&wide_moves[rank], &pool_moves[rank], true) != -1) {
      log_fatal("peg2 wide ranks disagree with the static pool");
    }
  }
  int scored = 0;
  for (int idx = 0; idx < ranks->count; idx++) {
    if (ranks->root[idx] != root_id) {
      continue;
    }
    const int rank = ranks->rank[idx];
    if (rank >= *wide_count || scored == VNT_PEG2_WIDE_CAP) {
      log_fatal("peg2 rank past the move list");
    }
    chosen[scored] = &wide_moves[rank];
    ids[scored++] = rank;
  }
  return scored;
}

// 2PEG labels. For each board, each of the static top-K placements is played
// in every hidden world: which two of the nine unseen tiles are in the bag
// and, for a one-tile play, which of them the mover draws (weighted by
// multiplicity). A play of two or more tiles empties the bag. After a one-tile
// play the opponent faces a 1PEG and the network picks its move from the
// opponent's information; any further one-tile-bag turn is picked the same
// way. The bag-empty endgame is then searched to a fixed depth. Fields:
// positions prefix, output prefix, top K, endgame plies, first model, second
// model|-, shortlist, w_spread, scale, check boards (network information-rule
// check on the first N boards).
static void vnt_peg2_data(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 11) {
    log_fatal("peg2data needs positions prefix, output prefix, top K, plies, "
              "first, second|-, shortlist, w_spread, scale, check boards");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  const int top_k = atoi(string_splitter_get_item(fields, 3));
  const int plies = atoi(string_splitter_get_item(fields, 4));
  const int check_boards = atoi(string_splitter_get_item(fields, 10));
  if (top_k < 1 || top_k > VNT_PEG1_POOL || plies < 1 || plies > 25) {
    log_fatal("peg2data invalid top K or plies");
  }
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  Game *world = game_duplicate(root);
  VntPegNet *net = vnt_peg_net_create(
      string_splitter_get_item(fields, 5), string_splitter_get_item(fields, 6),
      atoi(string_splitter_get_item(fields, 7)),
      strtod(string_splitter_get_item(fields, 8), NULL),
      strtod(string_splitter_get_item(fields, 9), NULL), root, 1);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  VntPeg2Ranks ranks = vnt_peg2_ranks_load();
  MoveList *wide = move_list_create(VNT_PEG2_WIDE_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  const Move **chosen = malloc_or_die(sizeof(Move *) * VNT_PEG2_WIDE_CAP);
  int *ids = malloc_or_die(sizeof(int) * VNT_PEG2_WIDE_CAP);
  EndgameCtx *ctx = NULL;
  EndgameResults *results = endgame_results_create();
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  // Resume: boards already in the labels file are skipped.
  char *labels_path = get_formatted_string("%s.labels.csv", prefix);
  char *scenarios_path = get_formatted_string("%s.scenarios.csv", prefix);
  char *line = NULL;
  size_t capacity = 0;
  int done_max = -1;
  bool fresh = true;
  FILE *previous = fopen(labels_path, "r");
  if (previous != NULL) {
    fresh = fgetc(previous) == EOF;
    rewind(previous);
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id;
      if (sscanf(line, "%d,", &root_id) == 1 && root_id > done_max) {
        done_max = root_id;
      }
    }
    fclose(previous);
  }
  FILE *labels = fopen_or_die(labels_path, "a");
  FILE *scenarios = fopen_or_die(scenarios_path, "a");
  free(labels_path);
  free(scenarios_path);
  if (fresh) {
    fprintf(labels, "root,candidate,candidates,tiles,after,static_equity,win,"
                    "margin,worlds,net_calls,net_ms,solve_ms,move\n");
    fprintf(scenarios, "root,candidate,bag_a,bag_b,drawn,multiplicity,"
                       "reply_pass,reply_tiles,reply_score,turns,win,"
                       "margin\n");
  }
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  int boards = 0;
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = vnt_peg2_next_root(positions, &line, &capacity, root,
                                           errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    net->check = boards++ < check_boards;
    // Rows are written in board order, so a resumed run continues after the
    // last board in the labels file (that board's rows were flushed whole).
    if (root_id <= done_max) {
      continue;
    }
    const int mover = game_get_player_on_turn_index(root);
    int unseen[VALUE_NET_TILE_TYPES];
    if (vnt_peg1_public_unseen(root, unseen) != VNT_PEG2_UNSEEN) {
      log_fatal("peg2 root does not have nine unseen tiles");
    }
    Move candidates[VNT_PEG1_POOL];
    int pass_rank;
    const int count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("peg2 candidate replay mismatch");
    }
    int wide_count;
    const int scored = vnt_peg2_board_candidates(
        root, root_id, &ranks, candidates, count, top_k, wide, wide_moves,
        &wide_count, chosen, ids);
    for (int slot = 0; slot < scored; slot++) {
      const int candidate = ids[slot];
      const Move *cand = chosen[slot];
      const int tiles = move_get_tiles_played(cand);
      const int n_drawn = tiles < 2 ? tiles : 2;
      int64_t calls_before, checks;
      double net_ms_before;
      vnt_peg_net_totals(net, &calls_before, &checks, &net_ms_before);
      double win_sum = 0, margin_sum = 0, weight_sum = 0, solve_ms = 0;
      int worlds = 0;
      for (int tile_a = 0; tile_a < VALUE_NET_TILE_TYPES; tile_a++) {
        for (int tile_b = tile_a; tile_b < VALUE_NET_TILE_TYPES; tile_b++) {
          const int pairs = tile_a == tile_b
                                ? unseen[tile_a] * (unseen[tile_a] - 1) / 2
                                : unseen[tile_a] * unseen[tile_b];
          if (pairs == 0) {
            continue;
          }
          // The opponent's reply depends only on its rack, i.e. on the
          // unordered pair, so one network pick serves both draw orders.
          bool have_reply = false;
          Move reply;
          const int orders = n_drawn == 1 && tile_a != tile_b ? 2 : 1;
          for (int order = 0; order < orders; order++) {
            const MachineLetter bag2[2] = {
                (MachineLetter)(order == 0 ? tile_a : tile_b),
                (MachineLetter)(order == 0 ? tile_b : tile_a)};
            // Weight in ordered draws of the two bag tiles.
            const double weight = n_drawn == 1
                                      ? (tile_a == tile_b ? 2.0 * pairs : pairs)
                                      : 2.0 * pairs;
            vnt_peg2_world(root, unseen, cand, bag2, n_drawn, world);
            int reply_pass = -1, reply_tiles = -1, turns = 0;
            double reply_score = 0, ignored_ms = 0;
            if (n_drawn == 1) {
              if (!have_reply) {
                if (!vnt_peg_net_pick(net, 0, world, &reply)) {
                  log_fatal("peg2 opponent reply is not a 1PEG");
                }
                have_reply = true;
              }
              reply_pass = move_get_type(&reply) == GAME_EVENT_PASS;
              reply_tiles = move_get_tiles_played(&reply);
              reply_score = equity_to_double(move_get_score(&reply));
              play_move(&reply, world, NULL);
              turns = 1 + vnt_peg2_drain(world, net, VNT_PEG2_REPLY_NET,
                                         static_list, &ignored_ms);
            }
            const int64_t start = ctimer_monotonic_ns();
            const double margin = vnt_peg2_depth_label(world, mover, plies,
                                                       &ctx, results, control);
            solve_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
            const double win = margin > 0 ? 1 : margin < 0 ? 0 : .5;
            win_sum += weight * win;
            margin_sum += weight * margin;
            weight_sum += weight;
            worlds++;
            fprintf(scenarios, "%d,%d,%d,%d,%d,%.0f,%d,%d,%.9g,%d,%.1f,%.9g\n",
                    root_id, candidate, bag2[0], bag2[1], n_drawn, weight,
                    reply_pass, reply_tiles, reply_score, turns, win, margin);
          }
        }
      }
      if (weight_sum != VNT_PEG2_UNSEEN * (VNT_PEG2_UNSEEN - 1)) {
        log_fatal("peg2 world weights do not cover the unseen pool");
      }
      int64_t calls_after;
      double net_ms_after;
      vnt_peg_net_totals(net, &calls_after, &checks, &net_ms_after);
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), cand,
                              game_get_ld(root), false);
      fprintf(
          labels, "%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%d,%lld,%.9g,%.9g,\"%s\"\n",
          root_id, candidate, wide_count, tiles,
          value_net_spread_after_move(root, cand),
          equity_to_double(move_get_equity(cand)), win_sum / weight_sum,
          margin_sum / weight_sum, worlds,
          (long long)(calls_after - calls_before), net_ms_after - net_ms_before,
          solve_ms, string_builder_peek(name));
    }
    fflush(scenarios);
    fsync(fileno(scenarios));
    fflush(labels);
    fsync(fileno(labels));
    int64_t calls, checks;
    double net_ms;
    vnt_peg_net_totals(net, &calls, &checks, &net_ms);
    printf("peg2 root %d candidates %d net calls %lld checks %lld\n", root_id,
           scored, (long long)calls, (long long)checks);
    fflush(stdout);
  }
  free(line);
  fclose(positions);
  fclose(labels);
  fclose(scenarios);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  endgame_ctx_destroy(ctx);
  endgame_results_destroy(results);
  thread_control_destroy(control);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  vnt_peg2_ranks_destroy(&ranks);
  move_list_destroy(wide);
  free(wide_moves);
  free(chosen);
  free(ids);
  vnt_peg_net_destroy(net);
  game_destroy(world);
  config_destroy(config);
}

// 2PEG picks scored in the actual game: the stored board's bag tiles, drawn in
// a fixed per-board order, then the reply policy for every one-tile-bag turn
// and an exact endgame solve. Codes: >= 0 static rank; -2 MAGPIE's default
// PEG; -4 default PEG with the network replacing the nested 1PEG; -5 the same
// with information-rule checks (network POV at every nested call, and the
// whole pick under a hidden-tile swap). Fields: positions prefix, choices CSV,
// output prefix, reply (net|peg), plies, TT fraction, first, second|-,
// shortlist, w_spread, scale.
static void vnt_peg2_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 12) {
    log_fatal("peg2eval needs positions prefix, choices, output prefix, "
              "reply, plies, tt fraction, first, second|-, shortlist, "
              "w_spread, scale");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const char *reply_name = string_splitter_get_item(fields, 4);
  const int plies = atoi(string_splitter_get_item(fields, 5));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 6), NULL);
  VntPeg2Reply reply;
  if (strings_equal(reply_name, "net")) {
    reply = VNT_PEG2_REPLY_NET;
  } else if (strings_equal(reply_name, "peg")) {
    reply = VNT_PEG2_REPLY_PEG;
  } else {
    log_fatal("peg2eval reply must be net or peg");
  }
  if (plies < 1 || plies > 25 || tt_fraction <= 0 || tt_fraction > 0.1) {
    log_fatal("peg2eval invalid plies or tt fraction");
  }
  int num_picks = 0, pick_capacity = 64;
  int *pick_root = malloc_or_die(sizeof(int) * pick_capacity);
  int *pick_code = malloc_or_die(sizeof(int) * pick_capacity);
  bool *pick_done = NULL;
  char *line = NULL;
  size_t capacity = 0;
  FILE *choices = fopen_or_die(choices_path, "r");
  while (getline_ignore_carriage_return(&line, &capacity, choices) >= 0) {
    int root_id, code;
    if (sscanf(line, "%d,%d", &root_id, &code) != 2) {
      continue;
    }
    if (num_picks == pick_capacity) {
      pick_capacity *= 2;
      pick_root = realloc_or_die(pick_root, sizeof(int) * pick_capacity);
      pick_code = realloc_or_die(pick_code, sizeof(int) * pick_capacity);
    }
    pick_root[num_picks] = root_id;
    pick_code[num_picks++] = code;
  }
  fclose(choices);
  // VNT_PEG2_ONLY: a CSV of "root,rank,rank,..." lines (ranks in the root's
  // static candidate pool, e.g. a network's top K). When set, the PEG pick
  // codes solve only the listed candidates of their root.
  const char *only_path = getenv("VNT_PEG2_ONLY");
  int only_roots = 0;
  int *only_root = NULL, *only_count = NULL, *only_rank = NULL;
  if (only_path != NULL && only_path[0] != '\0') {
    int only_capacity = 64;
    only_root = malloc_or_die(sizeof(int) * only_capacity);
    only_count = malloc_or_die(sizeof(int) * only_capacity);
    only_rank = malloc_or_die(sizeof(int) * only_capacity * VNT_PEG2_ONLY_MAX);
    FILE *only_file = fopen_or_die(only_path, "r");
    while (getline_ignore_carriage_return(&line, &capacity, only_file) >= 0) {
      char *cursor = line;
      char *end;
      const long root_id = strtol(cursor, &end, 10);
      if (end == cursor) {
        continue;
      }
      if (only_roots == only_capacity) {
        only_capacity *= 2;
        only_root = realloc_or_die(only_root, sizeof(int) * only_capacity);
        only_count = realloc_or_die(only_count, sizeof(int) * only_capacity);
        only_rank = realloc_or_die(only_rank, sizeof(int) * only_capacity *
                                                  VNT_PEG2_ONLY_MAX);
      }
      int listed = 0;
      cursor = end;
      while (*cursor == ',') {
        const long rank = strtol(cursor + 1, &end, 10);
        if (end == cursor + 1 || listed == VNT_PEG2_ONLY_MAX || rank < 0) {
          log_fatal("peg2eval invalid VNT_PEG2_ONLY line");
        }
        only_rank[only_roots * VNT_PEG2_ONLY_MAX + listed++] = (int)rank;
        cursor = end;
      }
      if (listed < 1) {
        log_fatal("peg2eval VNT_PEG2_ONLY line lists no candidates");
      }
      only_root[only_roots] = (int)root_id;
      only_count[only_roots++] = listed;
    }
    fclose(only_file);
  }
  pick_done =
      calloc_or_die((size_t)(num_picks > 0 ? num_picks : 1), sizeof(bool));
  char *out_path = get_formatted_string("%s.peg2.csv", prefix);
  FILE *previous = fopen(out_path, "r");
  bool fresh = previous == NULL;
  if (previous != NULL) {
    fresh = fgetc(previous) == EOF;
    rewind(previous);
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id, code;
      if (sscanf(line, "%d,%d", &root_id, &code) == 2) {
        for (int idx = 0; idx < num_picks; idx++) {
          if (pick_root[idx] == root_id && pick_code[idx] == code) {
            pick_done[idx] = true;
          }
        }
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  if (fresh) {
    fprintf(output,
            "root,code,choice,tiles,pass,drawn,final,win,proven,solve_ms,"
            "reply_ms,reply_turns,pick_ms,net_calls,net_ms,net_checks,"
            "net_hits,move\n");
  }
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  Game *game = game_duplicate(root);
  Game *swap = game_duplicate(root);
  VntPegNet *net = vnt_peg_net_create(
      string_splitter_get_item(fields, 7), string_splitter_get_item(fields, 8),
      atoi(string_splitter_get_item(fields, 9)),
      strtod(string_splitter_get_item(fields, 10), NULL),
      strtod(string_splitter_get_item(fields, 11), NULL), root, 2);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  MoveList *wide = move_list_create(VNT_PEG2_WIDE_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  VntPeg1Exact exact = vnt_peg1_exact_create(plies, tt_fraction);
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = vnt_peg2_next_root(positions, &line, &capacity, root,
                                           errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    bool pending = false;
    for (int idx = 0; idx < num_picks; idx++) {
      pending |= pick_root[idx] == root_id && !pick_done[idx];
    }
    if (!pending) {
      continue;
    }
    const int seat = game_get_player_on_turn_index(root);
    int unseen[VALUE_NET_TILE_TYPES];
    if (vnt_peg1_public_unseen(root, unseen) != VNT_PEG2_UNSEEN) {
      log_fatal("peg2eval root does not have nine unseen tiles");
    }
    Move candidates[VNT_PEG1_POOL];
    int pass_rank;
    const int count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("peg2eval candidate replay mismatch");
    }
    const Move *only_moves[VNT_PEG2_ONLY_MAX];
    int n_only = 0;
    if (only_root != NULL) {
      int entry = -1;
      for (int idx = 0; idx < only_roots && entry < 0; idx++) {
        if (only_root[idx] == root_id) {
          entry = idx;
        }
      }
      if (entry < 0) {
        log_fatal("peg2eval root missing from VNT_PEG2_ONLY");
      }
      for (int idx = 0; idx < only_count[entry]; idx++) {
        const int rank = only_rank[entry * VNT_PEG2_ONLY_MAX + idx];
        if (rank >= count) {
          log_fatal("peg2eval VNT_PEG2_ONLY rank outside the pool");
        }
        only_moves[n_only++] = &candidates[rank];
      }
    }
    // The actual bag tiles, in a fixed per-board draw order.
    MachineLetter bag2[2];
    int found = 0;
    for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
      for (int copy = 0;
           copy < bag_get_letter(game_get_bag(root), (MachineLetter)ml);
           copy++) {
        bag2[found++] = (MachineLetter)ml;
      }
    }
    if (found != 2) {
      log_fatal("peg2eval bag does not hold two tiles");
    }
    if (vnt_mix((uint64_t)root_id) & 1) {
      const MachineLetter first = bag2[0];
      bag2[0] = bag2[1];
      bag2[1] = first;
    }
    for (int idx = 0; idx < num_picks; idx++) {
      if (pick_root[idx] != root_id || pick_done[idx]) {
        continue;
      }
      const int code = pick_code[idx];
      int64_t calls0, checks0;
      double net_ms0;
      vnt_peg_net_totals(net, &calls0, &checks0, &net_ms0);
      const int64_t hits0 = vnt_peg_net_hits(net);
      Move pick;
      double pick_ms = 0;
      if (code >= 0) {
        // Codes past the static pool are wide ranks.
        if (code < count) {
          move_copy(&pick, &candidates[code]);
        } else {
          const int wide_count = vnt_peg2_wide_pool(root, wide, wide_moves);
          if (code >= wide_count) {
            log_fatal("peg2eval invalid candidate");
          }
          move_copy(&pick, &wide_moves[code]);
        }
      } else if (code == VNT_PEG2_PICK_DEFAULT_PEG) {
        vnt_peg_pick_only(root, 1, NULL, NULL, only_moves, n_only, &pick,
                          &pick_ms);
      } else if (code == VNT_PEG2_PICK_NET_PEG ||
                 code == VNT_PEG2_PICK_NET_PEG_CHECKED ||
                 code == VNT_PEG2_PICK_NET_PEG_UNCACHED) {
        net->check = code == VNT_PEG2_PICK_NET_PEG_CHECKED;
        net->cache = code != VNT_PEG2_PICK_NET_PEG_UNCACHED;
        vnt_peg_net_reset(net);
        vnt_peg_pick_only(root, 1, vnt_peg_net_chooser, net, only_moves, n_only,
                          &pick, &pick_ms);
        if (net->check) {
          // Swap one bag tile with a tile from the opponent's rack: the
          // mover's information is unchanged, so the pick must be too.
          const Rack *opp_rack =
              player_get_rack(game_get_player(root, 1 - seat));
          int other = -1;
          for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
            if (ml != bag2[0] && rack_get_letter(opp_rack, ml) > 0) {
              other = ml;
            }
          }
          if (other >= 0) {
            game_copy(swap, root);
            Rack *swap_rack = player_get_rack(game_get_player(swap, 1 - seat));
            rack_take_letter(swap_rack, (MachineLetter)other);
            rack_add_letter(swap_rack, bag2[0]);
            const MachineLetter swapped_bag[2] = {(MachineLetter)other,
                                                  bag2[1]};
            bag_set_to_tiles(game_get_bag(swap), swapped_bag, 2);
            Move private_pick;
            double ignored;
            vnt_peg_net_reset(net);
            // The swapped game replays the same pool (the mover's rack and
            // board are unchanged), so the restricted moves still apply.
            vnt_peg_pick_only(swap, 1, vnt_peg_net_chooser, net, only_moves,
                              n_only, &private_pick, &ignored);
            if (compare_moves(&pick, &private_pick, true) != -1) {
              log_fatal("network PEG pick depends on the hidden tiles");
            }
          }
          net->check = false;
        }
        // The continuation's reply policy does not use the cache.
        net->cache = false;
      } else {
        log_fatal("peg2eval invalid pick code");
      }
      int choice = -1;
      for (int cand = 0; cand < count && choice < 0; cand++) {
        if (compare_moves(&pick, &candidates[cand], true) == -1) {
          choice = cand;
        }
      }
      int64_t calls1, checks1;
      double net_ms1;
      vnt_peg_net_totals(net, &calls1, &checks1, &net_ms1);
      const int64_t hits1 = vnt_peg_net_hits(net);
      const int tiles = move_get_tiles_played(&pick);
      const int n_drawn = tiles < 2 ? tiles : 2;
      vnt_peg2_world(root, unseen, &pick, bag2, n_drawn, game);
      double reply_ms = 0;
      const int reply_turns =
          vnt_peg2_drain(game, net, reply, static_list, &reply_ms);
      bool proven = true;
      double solve_ms = 0;
      double final = vnt_peg1_margin(game, seat);
      if (!game_over(game)) {
        if (bag_get_letters(game_get_bag(game)) != 0) {
          log_fatal("peg2eval bag not empty before the exact solve");
        }
        const int on_turn = game_get_player_on_turn_index(game);
        const int64_t start = ctimer_monotonic_ns();
        const int value = vnt_peg1_exact_value(&exact, game, &proven);
        solve_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        final += on_turn == seat ? value : -value;
        if (exact.capped) {
          char *capped_path = get_formatted_string("%s.capped.csv", prefix);
          FILE *capped = fopen_or_die(capped_path, "a");
          free(capped_path);
          fprintf(capped, "%d,%d,%d,%d,%.9g\n", root_id, code, exact.depth,
                  value, solve_ms);
          fclose(capped);
        }
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      fprintf(output,
              "%d,%d,%d,%d,%d,%d,%.9g,%.1f,%d,%.9g,%.9g,%d,%.9g,%lld,%.9g,%"
              "lld,%lld,\"%s\"\n",
              root_id, code, choice, tiles,
              move_get_type(&pick) == GAME_EVENT_PASS,
              n_drawn == 1 ? bag2[0] : -1, final,
              final > 0   ? 1.0
              : final < 0 ? 0.0
                          : .5,
              proven, solve_ms, reply_ms, reply_turns, pick_ms,
              (long long)(calls1 - calls0), net_ms1 - net_ms0,
              (long long)(checks1 - checks0), (long long)(hits1 - hits0),
              string_builder_peek(name));
      fflush(output);
      fsync(fileno(output));
      pick_done[idx] = true;
    }
  }
  free(line);
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  vnt_peg1_exact_destroy(&exact);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  move_list_destroy(wide);
  free(wide_moves);
  vnt_peg_net_destroy(net);
  game_destroy(game);
  game_destroy(swap);
  config_destroy(config);
  free(pick_root);
  free(pick_code);
  free(pick_done);
  free(only_root);
  free(only_count);
  free(only_rank);
}

// ---- 2PEG candidate features ------------------------------------------------
// Per-world features for a 2PEG chooser, from the mover's information only:
// the nine unseen tiles are enumerated as the possible bag pairs (the
// opponent holds the other seven), weighted by ordered-draw multiplicity; the
// actual bag is never read. Two tiers, timed separately:
//  - cheap, every candidate: the opponent's top-scoring reply (lane-
//    incremental from its three best plays on the pre-move board, as in the
//    1PEG v2 features) and the mover's greedy follow-up. After a bag-emptying
//    play the follow-up is on the post-move board; after a one-tile play it is
//    after the opponent's top-scoring reply (which draws the last tile).
//  - expensive, one-tile candidates only: the 1PEG network's pick for the
//    opponent, which then faces a 1PEG, and its predicted win probability,
//    spread and utility from the opponent's view. The opponent's information
//    is the same for both draw orders of a pair, so one call serves both.
enum {
  VNT_PEG2_FEAT_WIDTH = 16,
  VNT_PEG2_MAX_PAIRS = VALUE_NET_TILE_TYPES * (VALUE_NET_TILE_TYPES + 1) / 2,
  VNT_PEG2_ORDERED = VNT_PEG2_UNSEEN * (VNT_PEG2_UNSEEN - 1),
};

typedef struct VntPeg2Prep {
  int unseen[VALUE_NET_TILE_TYPES];
  int pairs;
  MachineLetter tile_a[VNT_PEG2_MAX_PAIRS];
  MachineLetter tile_b[VNT_PEG2_MAX_PAIRS];
  int count[VNT_PEG2_MAX_PAIRS];
  Move lane_best[VNT_PEG2_MAX_PAIRS][VNT_PEG1_ROOT_LANES];
} VntPeg2Prep;

static void vnt_peg2_prep(const Game *root, VntPeg1Work *work,
                          VntPeg2Prep *prep) {
  if (vnt_peg1_public_unseen(root, prep->unseen) != VNT_PEG2_UNSEEN) {
    log_fatal("peg2feat root does not have nine unseen tiles");
  }
  const int mover = game_get_player_on_turn_index(root);
  Move pass;
  move_set_as_pass(&pass);
  prep->pairs = 0;
  for (int tile_a = 0; tile_a < VALUE_NET_TILE_TYPES; tile_a++) {
    for (int tile_b = tile_a; tile_b < VALUE_NET_TILE_TYPES; tile_b++) {
      const int pairs =
          tile_a == tile_b
              ? prep->unseen[tile_a] * (prep->unseen[tile_a] - 1) / 2
              : prep->unseen[tile_a] * prep->unseen[tile_b];
      if (pairs == 0) {
        continue;
      }
      const int pair = prep->pairs++;
      prep->tile_a[pair] = (MachineLetter)tile_a;
      prep->tile_b[pair] = (MachineLetter)tile_b;
      prep->count[pair] = pairs;
      // The opponent's rack in this world, on the pre-move board.
      const MachineLetter bag2[2] = {(MachineLetter)tile_a,
                                     (MachineLetter)tile_b};
      vnt_peg2_world(root, prep->unseen, &pass, bag2, 0, work->opp_root);
      game_set_player_on_turn_index(work->opp_root, 1 - mover);
      uint64_t lanes = ~UINT64_C(0) >> (64 - 2 * BOARD_DIM);
      for (int idx = 0; idx < VNT_PEG1_ROOT_LANES; idx++) {
        Move *best = &prep->lane_best[pair][idx];
        vnt_peg1_best_scoring(work->opp_root, work->best_list, lanes, NULL,
                              best);
        if (move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
          lanes &= ~(UINT64_C(1) << vnt_peg1_move_lane(best));
        }
      }
    }
  }
}

// The opponent's top-scoring reply in world (opponent on turn); features 1-4.
static void vnt_peg2_cheap_reply(const VntPeg2Prep *prep, int pair,
                                 uint64_t touched, Game *world, int mover,
                                 VntPeg1Work *work, float *feature) {
  const Move *base = NULL;
  for (int idx = 0; idx < VNT_PEG1_ROOT_LANES; idx++) {
    const Move *lb = &prep->lane_best[pair][idx];
    if (move_get_type(lb) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      break;
    }
    if (!((touched >> vnt_peg1_move_lane(lb)) & 1)) {
      base = lb;
      break;
    }
  }
  vnt_peg1_best_scoring(world, work->best_list, base == NULL ? 0 : touched,
                        base, work->found);
  const bool placement =
      move_get_type(work->found) == GAME_EVENT_TILE_PLACEMENT_MOVE;
  const int tiles = placement ? move_get_tiles_played(work->found) : 0;
  const bool out = placement && bag_get_letters(game_get_bag(world)) == 0 &&
                   tiles == rack_get_total_letters(player_get_rack(
                                game_get_player(world, 1 - mover)));
  feature[1] =
      (float)(equity_to_int(move_get_score(work->found)) +
              (out ? equity_to_int(calculate_end_rack_points(
                         player_get_rack(game_get_player(world, mover)),
                         game_get_ld(world)))
                   : 0));
  feature[2] = out;
  feature[3] = (float)tiles;
  feature[4] = !placement;
}

// The 1PEG network's pick for the player on turn in a one-tile-bag world, with
// the predicted win probability, spread and utility of that pick from the
// picker's view (the cascade's second network when there is one).
static void vnt_peg2_net_value(VntPegNet *net, const Game *game, Move *pick,
                               double *win, double *spread, double *value) {
  VntPegNetSlot *slot = &net->slot[0];
  Move candidates[VNT_PEG1_POOL];
  double values[VNT_PEG1_POOL], wins[VNT_PEG1_POOL], spreads[VNT_PEG1_POOL];
  VntPeg1NetTiming timing = {0}, timing2 = {0};
  int count;
  const int choice = vnt_peg1_util_choose(
      net->first, net->second, net->shortlist, &net->utility, game, slot->pool,
      candidates, &count, &slot->work, slot->inputs, slot->inputs2, values,
      wins, spreads, &timing, &timing2);
  if (choice < 0) {
    move_set_as_pass(pick);
    *win = NAN;
    *spread = NAN;
    *value = NAN;
    return;
  }
  move_copy(pick, &candidates[choice]);
  if (net->second == NULL) {
    *win = wins[choice];
    *spread = spreads[choice];
    *value = values[choice];
    return;
  }
  VntPeg1Prep prep;
  vnt_peg1_prep(game, &slot->work, &prep, true);
  vnt_peg1_net3_inputs(net->second, game, pick, 1, &slot->work, &prep,
                       slot->inputs2, &timing2);
  *value = vnt_peg1_net3_value(net->second, slot->inputs2, &net->utility, win,
                               spread);
}

// Fields: positions prefix, output prefix, top K, first model, second
// model|-, shortlist, w_spread, scale, net features (0|1), check boards.
// Writes OUT.cands.csv (one row per candidate), OUT.public.f32 (the 68 public
// scalars, static equity and spread after the move per candidate) and
// OUT.worlds.f32 (VNT_PEG2_FEAT_WIDTH floats per candidate world, in
// cands.csv order; see the column list in the CSV header comment below).
static void vnt_peg2_features(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 11) {
    log_fatal("peg2feat needs positions prefix, output prefix, top K, first, "
              "second|-, shortlist, w_spread, scale, net features, check "
              "boards");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  const int top_k = atoi(string_splitter_get_item(fields, 3));
  const bool net_features = atoi(string_splitter_get_item(fields, 9)) != 0;
  const int check_boards = atoi(string_splitter_get_item(fields, 10));
  if (top_k < 1 || top_k > VNT_PEG1_POOL) {
    log_fatal("peg2feat invalid top K");
  }
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  Game *world = game_duplicate(root);
  Game *swapped_root = game_duplicate(root);
  VntPegNet *net = vnt_peg_net_create(
      string_splitter_get_item(fields, 4), string_splitter_get_item(fields, 5),
      atoi(string_splitter_get_item(fields, 6)),
      strtod(string_splitter_get_item(fields, 7), NULL),
      strtod(string_splitter_get_item(fields, 8), NULL), root, 1);
  VntPeg1Work work = vnt_peg1_work_create(root);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  VntPeg2Ranks ranks = vnt_peg2_ranks_load();
  MoveList *wide = move_list_create(VNT_PEG2_WIDE_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  const Move **chosen = malloc_or_die(sizeof(Move *) * VNT_PEG2_WIDE_CAP);
  int *ids = malloc_or_die(sizeof(int) * VNT_PEG2_WIDE_CAP);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.cands.csv", prefix);
  FILE *cands = vnt_fopen_new_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.public.f32", prefix);
  FILE *public_out = vnt_fopen_new_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.worlds.f32", prefix);
  FILE *worlds_out = vnt_fopen_new_or_die(path, "w");
  free(path);
  // worlds.f32 columns: 0 weight (ordered-draw probability), 1 opponent
  // top-scoring reply (+ mover rack x2 when it goes out), 2 out, 3 tiles,
  // 4 pass, 5 mover greedy follow-up (+ opponent rack x2 when out), 6 its out
  // flag, 7 network features present, 8 network win, 9 network spread,
  // 10 network utility, 11 network pick score, 12 its tiles, 13 its pass
  // flag, 14 tile drawn by the mover (one-tile plays; -1 otherwise), 15 tile
  // left in the bag (one-tile plays; -1 otherwise).
  fprintf(cands, "root,candidate,candidates,tiles,after,static_equity,"
                 "world_rows,prep_ms,cheap_ms,net_ms,net_calls\n");
  path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  ValueNetHistory history;
  value_net_history_reset(&history);
  int boards = 0;
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = vnt_peg2_next_root(positions, &line, &capacity, root,
                                           errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    const bool check = boards++ < check_boards;
    const int mover = game_get_player_on_turn_index(root);
    Move candidates[VNT_PEG1_POOL];
    int pass_rank;
    const int count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("peg2feat candidate replay mismatch");
    }
    int64_t start = ctimer_monotonic_ns();
    VntPeg2Prep prep;
    vnt_peg2_prep(root, &work, &prep);
    const double prep_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    int wide_count;
    const int scored = vnt_peg2_board_candidates(
        root, root_id, &ranks, candidates, count, top_k, wide, wide_moves,
        &wide_count, chosen, ids);
    float *rows =
        malloc_or_die(sizeof(float) * VNT_PEG2_ORDERED * VNT_PEG2_FEAT_WIDTH);
    for (int slot = 0; slot < scored; slot++) {
      const int candidate = ids[slot];
      const Move *cand = chosen[slot];
      const int tiles = move_get_tiles_played(cand);
      const int n_drawn = tiles < 2 ? tiles : 2;
      float scalars[VALUE_NET_SCALARS];
      value_net_scalars_for_move(root, cand, &history, scalars);
      float public_row[VALUE_NET_SCALARS - 4 + 2];
      int col = 0;
      for (int idx = 0; idx < VALUE_NET_SCALARS; idx++) {
        if (idx != 54 && idx != 55 && idx != 56 && idx != 67) {
          public_row[col++] = scalars[idx];
        }
      }
      const double after = value_net_spread_after_move(root, cand);
      public_row[col++] = (float)equity_to_double(move_get_equity(cand));
      public_row[col++] = (float)after;
      fwrite(public_row, sizeof(float), (size_t)col, public_out);
      const uint64_t touched =
          vnt_peg1_lane_influence(game_get_board(root), cand);
      double cheap_ms = 0, net_ms = 0, weight_sum = 0;
      int net_calls = 0, filled = 0;
      for (int pair = 0; pair < prep.pairs; pair++) {
        const MachineLetter tile_a = prep.tile_a[pair];
        const MachineLetter tile_b = prep.tile_b[pair];
        const int orders = n_drawn == 1 && tile_a != tile_b ? 2 : 1;
        bool have_net = false;
        float net_cols[6] = {0};
        for (int order = 0; order < orders; order++) {
          const MachineLetter bag2[2] = {order == 0 ? tile_a : tile_b,
                                         order == 0 ? tile_b : tile_a};
          float *feature = &rows[(size_t)filled * VNT_PEG2_FEAT_WIDTH];
          memset(feature, 0, sizeof(float) * VNT_PEG2_FEAT_WIDTH);
          const double weight = n_drawn == 1
                                    ? (tile_a == tile_b ? 2.0 * prep.count[pair]
                                                        : prep.count[pair])
                                    : 2.0 * prep.count[pair];
          feature[0] = (float)(weight / VNT_PEG2_ORDERED);
          weight_sum += weight;
          start = ctimer_monotonic_ns();
          vnt_peg2_world(root, prep.unseen, cand, bag2, n_drawn, world);
          if (game_over(world)) {
            // The mover went out with an empty bag: nothing follows.
            feature[14] = n_drawn == 1 ? bag2[0] : -1;
            feature[15] = n_drawn == 1 ? bag2[1] : -1;
            cheap_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
            filled++;
            continue;
          }
          vnt_peg2_cheap_reply(&prep, pair, touched, world, mover, &work,
                               feature);
          bool out = false, pass;
          int greedy_tiles;
          if (n_drawn == 1) {
            // The mover's greedy reply after the opponent's top-scoring
            // reply, which draws the last tile.
            if (move_get_type(work.found) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
              play_move(work.found, world, NULL);
            } else {
              Move pass_move;
              move_set_as_pass(&pass_move);
              play_move(&pass_move, world, NULL);
            }
            if (!game_over(world)) {
              feature[5] = (float)vnt_peg1_greedy(world, work.small, work.move,
                                                  &out, &greedy_tiles, &pass);
            }
          } else {
            game_set_player_on_turn_index(world, mover);
            feature[5] = (float)vnt_peg1_greedy(world, work.small, work.move,
                                                &out, &greedy_tiles, &pass);
          }
          feature[6] = out;
          feature[14] = n_drawn == 1 ? bag2[0] : -1;
          feature[15] = n_drawn == 1 ? bag2[1] : -1;
          cheap_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
          if (n_drawn == 1 && net_features) {
            if (!have_net) {
              start = ctimer_monotonic_ns();
              vnt_peg2_world(root, prep.unseen, cand, bag2, n_drawn, world);
              Move pick;
              double win, spread, value;
              vnt_peg2_net_value(net, world, &pick, &win, &spread, &value);
              net_ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
              net_calls++;
              const bool placed =
                  move_get_type(&pick) == GAME_EVENT_TILE_PLACEMENT_MOVE;
              net_cols[0] = 1;
              net_cols[1] = placed ? (float)win : 0;
              net_cols[2] = placed ? (float)spread : 0;
              net_cols[3] = placed ? (float)value : 0;
              net_cols[4] = (float)equity_to_int(move_get_score(&pick));
              net_cols[5] = placed ? (float)move_get_tiles_played(&pick) : 0;
              have_net = true;
              if (check) {
                // Information rule: the opponent's pick and values must not
                // change when the bag tile and the mover's drawn tile swap
                // (the opponent cannot tell them apart).
                const MachineLetter other[2] = {bag2[1], bag2[0]};
                vnt_peg2_world(root, prep.unseen, cand, other, n_drawn,
                               swapped_root);
                Move pick2;
                double win2, spread2, value2;
                vnt_peg2_net_value(net, swapped_root, &pick2, &win2, &spread2,
                                   &value2);
                if (compare_moves(&pick, &pick2, true) != -1 ||
                    (placed && (win != win2 || spread != spread2))) {
                  log_fatal("peg2feat network features depend on hidden "
                            "tiles");
                }
              }
            }
            feature[7] = net_cols[0];
            feature[8] = net_cols[1];
            feature[9] = net_cols[2];
            feature[10] = net_cols[3];
            feature[11] = net_cols[4];
            feature[12] = net_cols[5];
            feature[13] = net_cols[5] == 0;
          }
          filled++;
        }
      }
      if (weight_sum != VNT_PEG2_ORDERED) {
        log_fatal("peg2feat world weights do not cover the unseen pool");
      }
      fwrite(rows, sizeof(float), (size_t)filled * VNT_PEG2_FEAT_WIDTH,
             worlds_out);
      fprintf(cands, "%d,%d,%d,%d,%.9g,%.9g,%d,%.6g,%.6g,%.6g,%d\n", root_id,
              candidate, wide_count, tiles, after,
              equity_to_double(move_get_equity(cand)), filled,
              slot == 0 ? prep_ms : 0, cheap_ms, net_ms, net_calls);
    }
    free(rows);
    if (check) {
      // Information rule for the whole board: with the actual bag swapped for
      // two other unseen tiles (taken from the opponent's rack), the prep must
      // be identical; the features are built from the prep and public state.
      int actual[2] = {-1, -1}, found = 0;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
        for (int copy = 0;
             copy < bag_get_letter(game_get_bag(root), (MachineLetter)ml) &&
             found < 2;
             copy++) {
          actual[found++] = ml;
        }
      }
      MachineLetter swap2[2];
      int picked = 0;
      int rest[VALUE_NET_TILE_TYPES];
      memcpy(rest, prep.unseen, sizeof(rest));
      rest[actual[0]]--;
      rest[actual[1]]--;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES && picked < 2; ml++) {
        while (rest[ml] > 0 && picked < 2 &&
               (ml != actual[0] && ml != actual[1])) {
          swap2[picked++] = (MachineLetter)ml;
          rest[ml]--;
        }
      }
      if (picked == 2) {
        Move pass;
        move_set_as_pass(&pass);
        vnt_peg2_world(root, prep.unseen, &pass, swap2, 0, swapped_root);
        // swapped_root: same board and mover rack, the opponent holding the
        // old bag tiles, the bag holding swap2.
        bag_set_to_tiles(game_get_bag(swapped_root), swap2, 2);
        game_set_player_on_turn_index(swapped_root, mover);
        VntPeg2Prep prep2;
        vnt_peg2_prep(swapped_root, &work, &prep2);
        if (prep2.pairs != prep.pairs ||
            memcmp(prep2.unseen, prep.unseen, sizeof(prep.unseen)) != 0) {
          log_fatal("peg2feat prep depends on hidden tiles");
        }
        for (int pair = 0; pair < prep.pairs; pair++) {
          for (int idx = 0; idx < VNT_PEG1_ROOT_LANES; idx++) {
            if (compare_moves(&prep.lane_best[pair][idx],
                              &prep2.lane_best[pair][idx], true) != -1 &&
                move_get_type(&prep.lane_best[pair][idx]) ==
                    GAME_EVENT_TILE_PLACEMENT_MOVE) {
              log_fatal("peg2feat prep depends on hidden tiles");
            }
          }
        }
      }
    }
    fflush(cands);
    fflush(public_out);
    fflush(worlds_out);
    printf("peg2feat root %d candidates %d\n", root_id, scored);
    fflush(stdout);
  }
  free(line);
  fclose(positions);
  fclose(cands);
  fclose(public_out);
  fclose(worlds_out);
  error_stack_destroy(errors);
  move_list_destroy(pool);
  vnt_peg2_ranks_destroy(&ranks);
  move_list_destroy(wide);
  free(wide_moves);
  free(chosen);
  free(ids);
  vnt_peg1_work_destroy(&work);
  vnt_peg_net_destroy(net);
  game_destroy(world);
  game_destroy(swapped_root);
  config_destroy(config);
}

// ---- 2PEG cheap-tier chooser ------------------------------------------------
// The cheap-tier 2PEG chooser in native code. For each of the static top K
// placements: the per-world cheap features of peg2feat (the opponent's
// top-scoring reply and the mover's greedy follow-up, over the mover's hidden
// worlds), aggregated as in scripts/peg2_cv.py, then a one-hidden-layer
// network (normalization folded into the first layer). The best score is
// played. Only the public state and the mover's rack are read: worlds are
// enumerated from the public unseen pool, and the bag is never consulted.
// Optional staging: a first model on the opponent-reply columns only (no
// greedy follow-up, the costly half) scores all K, and the full model scores
// the first model's best N.
enum {
  VNT_PEG2_PICK_MAGIC = 0x50473253,
  VNT_PEG2_PUBLIC = VALUE_NET_SCALARS - 4 + 2,
  VNT_PEG2_THRESHOLDS = 9,
  VNT_PEG2_CHEAP = 4 * 6 + 2 * (VNT_PEG2_THRESHOLDS + 4),
  VNT_PEG2_PICK_INPUT = VNT_PEG2_PUBLIC + 2 + VNT_PEG2_CHEAP,
  // Opponent-reply columns: mean, sd, min and max of world columns 1-4, and
  // the one-ply estimate block.
  VNT_PEG2_OPP_INPUT = VNT_PEG2_PUBLIC + 2 + 4 * 4 + VNT_PEG2_THRESHOLDS + 4,
};

static const double vnt_peg2_thresholds[VNT_PEG2_THRESHOLDS] = {
    -40, -20, -10, -5, 0, 5, 10, 20, 40};

typedef struct VntPeg2Model {
  int inputs;
  int width;
  int top_k;
  float *blob;
  const float *first;
  const float *bias;
  const float *out;
  float out_bias;
} VntPeg2Model;

static VntPeg2Model *vnt_peg2_model_load(const char *path) {
  FILE *file = fopen_or_die(path, "rb");
  uint32_t shape[4];
  if (!fread_le_uint32s(shape, 4, file) || shape[0] != VNT_PEG2_PICK_MAGIC ||
      (shape[1] != VNT_PEG2_PICK_INPUT && shape[1] != VNT_PEG2_OPP_INPUT) ||
      shape[2] < 1 || shape[2] > 1024 || shape[3] < 1 ||
      shape[3] > VNT_PEG1_POOL) {
    log_fatal("invalid 2PEG chooser model");
  }
  VntPeg2Model *model = calloc_or_die(1, sizeof(VntPeg2Model));
  model->inputs = (int)shape[1];
  model->width = (int)shape[2];
  model->top_k = (int)shape[3];
  const size_t count = (size_t)model->width * (model->inputs + 2) + 1;
  uint32_t *bits = malloc_or_die(count * sizeof(uint32_t));
  model->blob = malloc_or_die(count * sizeof(float));
  if (!fread_le_uint32s(bits, count, file) || fgetc(file) != EOF) {
    log_fatal("invalid 2PEG chooser weights");
  }
  memcpy(model->blob, bits, count * sizeof(float));
  free(bits);
  fclose(file);
  model->first = model->blob;
  model->bias = model->first + (size_t)model->width * model->inputs;
  model->out = model->bias + model->width;
  model->out_bias = model->out[model->width];
  return model;
}

static void vnt_peg2_model_destroy(VntPeg2Model *model) {
  free(model->blob);
  free(model);
}

static float vnt_peg2_model_score(const VntPeg2Model *model,
                                  const float *input) {
  float total = model->out_bias;
  for (int unit = 0; unit < model->width; unit++) {
    const float *weights = &model->first[(size_t)unit * model->inputs];
    float sum = model->bias[unit];
    for (int col = 0; col < model->inputs; col++) {
      sum += weights[col] * input[col];
    }
    total += model->out[unit] * (sum > 0 ? sum : 0);
  }
  return total;
}

typedef struct VntPeg2PickTiming {
  int64_t pool_ns;
  int64_t prep_ns;
  int64_t opp_ns;
  int64_t greedy_ns;
  int64_t model_ns;
} VntPeg2PickTiming;

// Resets base, a post-candidate world of vnt_peg2_world, to another bag
// pair: only the racks, the bag, the end reason and the side on turn differ
// between a candidate's worlds. own_after is the mover's rack after the
// candidate, before drawing.
static void vnt_peg2_world_set_pair(const int *unseen, const Rack *own_after,
                                    const MachineLetter *bag2, int n_drawn,
                                    int mover, int on_turn, Game *base) {
  int rest[VALUE_NET_TILE_TYPES];
  memcpy(rest, unseen, sizeof(rest));
  rest[bag2[0]]--;
  rest[bag2[1]]--;
  Rack *opponent = player_get_rack(game_get_player(base, 1 - mover));
  rack_reset(opponent);
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    for (int copy = 0; copy < rest[ml]; copy++) {
      rack_add_letter(opponent, (MachineLetter)ml);
    }
  }
  Rack *own = player_get_rack(game_get_player(base, mover));
  rack_copy(own, own_after);
  for (int idx = 0; idx < n_drawn; idx++) {
    rack_add_letter(own, bag2[idx]);
  }
  bag_set_to_tiles(game_get_bag(base), bag2 + n_drawn, 2 - n_drawn);
  game_set_game_end_reason(base, GAME_END_REASON_NONE);
  game_set_player_on_turn_index(base, on_turn);
}

// VNT_PEG2_NO_CACHE=1: rebuild every world from the root (the original
// path, kept for parity checks and timing).
static bool vnt_peg2_world_cache(void) {
  static int cache = -1;
  if (cache < 0) {
    const char *value = getenv("VNT_PEG2_NO_CACHE");
    cache = value == NULL || atoi(value) == 0;
  }
  return cache == 1;
}

// The cheap-tier world rows of one candidate, in peg2feat's layout (network
// columns zero; without greedy, columns 5 and 6 zero too). Returns the number
// of rows. The candidate's post-move world is built once (base) and reset per
// bag pair; after a one-tile play both draw orders leave the opponent the
// same rack on the same board (and the bag not empty), so the second order
// reuses the first's reply.
static int vnt_peg2_cheap_rows(const Game *root, const VntPeg2Prep *prep,
                               const Move *cand, bool greedy, VntPeg1Work *work,
                               Game *world, Game *base, float *rows,
                               VntPeg2PickTiming *timing) {
  const int mover = game_get_player_on_turn_index(root);
  const int tiles = move_get_tiles_played(cand);
  const int n_drawn = tiles < 2 ? tiles : 2;
  const uint64_t touched = vnt_peg1_lane_influence(game_get_board(root), cand);
  const bool cache = vnt_peg2_world_cache() && n_drawn > 0;
  bool base_ready = false;
  Rack own_after;
  int on_turn = 0;
  double weight_sum = 0;
  int filled = 0;
  for (int pair = 0; pair < prep->pairs; pair++) {
    const MachineLetter tile_a = prep->tile_a[pair];
    const MachineLetter tile_b = prep->tile_b[pair];
    const int orders = n_drawn == 1 && tile_a != tile_b ? 2 : 1;
    for (int order = 0; order < orders; order++) {
      const MachineLetter bag2[2] = {order == 0 ? tile_a : tile_b,
                                     order == 0 ? tile_b : tile_a};
      float *feature = &rows[(size_t)filled * VNT_PEG2_FEAT_WIDTH];
      memset(feature, 0, sizeof(float) * VNT_PEG2_FEAT_WIDTH);
      const double weight =
          n_drawn == 1
              ? (tile_a == tile_b ? 2.0 * prep->count[pair] : prep->count[pair])
              : 2.0 * prep->count[pair];
      feature[0] = (float)(weight / VNT_PEG2_ORDERED);
      weight_sum += weight;
      feature[14] = n_drawn == 1 ? bag2[0] : -1;
      feature[15] = n_drawn == 1 ? bag2[1] : -1;
      filled++;
      int64_t start = ctimer_monotonic_ns();
      Game *target = world;
      if (!cache) {
        vnt_peg2_world(root, prep->unseen, cand, bag2, n_drawn, world);
      } else {
        if (!base_ready) {
          vnt_peg2_world(root, prep->unseen, cand, bag2, n_drawn, base);
          rack_copy(&own_after, player_get_rack(game_get_player(base, mover)));
          for (int idx = 0; idx < n_drawn; idx++) {
            rack_take_letter(&own_after, bag2[idx]);
          }
          on_turn = game_get_player_on_turn_index(base);
          base_ready = true;
        } else {
          vnt_peg2_world_set_pair(prep->unseen, &own_after, bag2, n_drawn,
                                  mover, on_turn, base);
        }
        target = base;
        // Greedy plays the reply after a one-tile play: work on a copy.
        if (greedy && n_drawn == 1) {
          game_copy(world, base);
          target = world;
        }
      }
      if (game_over(target)) {
        timing->opp_ns += ctimer_monotonic_ns() - start;
        continue;
      }
      if (cache && order == 1) {
        memcpy(&feature[1], &feature[1 - VNT_PEG2_FEAT_WIDTH],
               sizeof(float) * 4);
      } else {
        vnt_peg2_cheap_reply(prep, pair, touched, target, mover, work, feature);
      }
      if (!greedy) {
        timing->opp_ns += ctimer_monotonic_ns() - start;
        continue;
      }
      if (n_drawn == 1) {
        if (move_get_type(work->found) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
          play_move(work->found, target, NULL);
        } else {
          Move pass_move;
          move_set_as_pass(&pass_move);
          play_move(&pass_move, target, NULL);
        }
      } else {
        game_set_player_on_turn_index(target, mover);
      }
      const int64_t mid = ctimer_monotonic_ns();
      timing->opp_ns += mid - start;
      bool out = false, pass;
      int greedy_tiles;
      if (!game_over(target)) {
        feature[5] = (float)vnt_peg1_greedy(target, work->small, work->move,
                                            &out, &greedy_tiles, &pass);
      }
      feature[6] = out;
      timing->greedy_ns += ctimer_monotonic_ns() - mid;
    }
  }
  if (weight_sum != VNT_PEG2_ORDERED) {
    log_fatal("peg2pick world weights do not cover the unseen pool");
  }
  return filled;
}

// Weighted mean, standard deviation, min and max of values[row * stride].
static void vnt_peg2_wstats(const double *values, int stride,
                            const double *prob, int rows, double *stats,
                            int block) {
  double mean = 0, square = 0, low = INFINITY, high = -INFINITY;
  for (int row = 0; row < rows; row++) {
    const double value = values[(size_t)row * stride];
    mean += value * prob[row];
    square += value * value * prob[row];
    low = fmin(low, value);
    high = fmax(high, value);
  }
  stats[0] = mean;
  stats[block] = sqrt(fmax(0, square - mean * mean));
  stats[2 * block] = low;
  stats[3 * block] = high;
}

// The model input of one candidate (scripts/peg2_cv.py cand_features, public
// and cheap groups).
static void vnt_peg2_pick_input(const float *public_row, int tiles,
                                const float *rows, int count, double after,
                                double *columns, double *prob, float *input) {
  for (int idx = 0; idx < VNT_PEG2_PUBLIC; idx++) {
    input[idx] = public_row[idx];
  }
  const bool one = tiles == 1;
  input[VNT_PEG2_PUBLIC] = (float)tiles;
  input[VNT_PEG2_PUBLIC + 1] = one;
  for (int row = 0; row < count; row++) {
    const float *feature = &rows[(size_t)row * VNT_PEG2_FEAT_WIDTH];
    prob[row] = feature[0];
    for (int col = 0; col < 6; col++) {
      columns[(size_t)row * 8 + col] = feature[col + 1];
    }
    const double est1 = after - (double)feature[1];
    columns[(size_t)row * 8 + 6] = est1;
    columns[(size_t)row * 8 + 7] =
        est1 + (one ? (double)feature[5]
                    : (1 - (double)feature[2]) * (double)feature[5]);
  }
  double cheap[VNT_PEG2_CHEAP];
  for (int col = 0; col < 6; col++) {
    vnt_peg2_wstats(&columns[col], 8, prob, count, &cheap[col], 6);
  }
  for (int est = 0; est < 2; est++) {
    double *block = &cheap[24 + est * (VNT_PEG2_THRESHOLDS + 4)];
    for (int idx = 0; idx < VNT_PEG2_THRESHOLDS; idx++) {
      const double threshold = vnt_peg2_thresholds[idx];
      double mass = 0;
      for (int row = 0; row < count; row++) {
        const double value = columns[(size_t)row * 8 + 6 + est];
        if (threshold == 0) {
          mass += ((value > 0) + .5 * (value == 0)) * prob[row];
        } else {
          mass += (value > threshold) * prob[row];
        }
      }
      block[idx] = mass;
    }
    vnt_peg2_wstats(&columns[6 + est], 8, prob, count,
                    &block[VNT_PEG2_THRESHOLDS], 1);
  }
  for (int idx = 0; idx < VNT_PEG2_CHEAP; idx++) {
    input[VNT_PEG2_PUBLIC + 2 + idx] = (float)cheap[idx];
  }
}

// The first-stage input: public columns and the opponent-reply columns of a
// full input.
static void vnt_peg2_opp_input(const float *input, float *opp) {
  int col = 0;
  for (int idx = 0; idx < VNT_PEG2_PUBLIC + 2; idx++) {
    opp[col++] = input[idx];
  }
  const float *cheap = &input[VNT_PEG2_PUBLIC + 2];
  for (int block = 0; block < 4; block++) {
    for (int idx = 0; idx < 4; idx++) {
      opp[col++] = cheap[block * 6 + idx];
    }
  }
  for (int idx = 24; idx < 24 + VNT_PEG2_THRESHOLDS + 4; idx++) {
    opp[col++] = cheap[idx];
  }
}

typedef struct VntPeg2PickWork {
  VntPeg1Work peg1;
  Game *world;
  Game *base;
  MoveList *pool;
  float *rows;
  double *columns;
  double *prob;
} VntPeg2PickWork;

static VntPeg2PickWork vnt_peg2_pick_work_create(const Game *game) {
  VntPeg2PickWork work = {
      .peg1 = vnt_peg1_work_create(game),
      .world = game_duplicate(game),
      .base = game_duplicate(game),
      .pool = move_list_create(VNT_PEG1_POOL + 1),
      .rows =
          malloc_or_die(sizeof(float) * VNT_PEG2_ORDERED * VNT_PEG2_FEAT_WIDTH),
      .columns = malloc_or_die(sizeof(double) * VNT_PEG2_ORDERED * 8),
      .prob = malloc_or_die(sizeof(double) * VNT_PEG2_ORDERED)};
  return work;
}

static void vnt_peg2_pick_work_destroy(VntPeg2PickWork *work) {
  vnt_peg1_work_destroy(&work->peg1);
  game_destroy(work->world);
  game_destroy(work->base);
  move_list_destroy(work->pool);
  free(work->rows);
  free(work->columns);
  free(work->prob);
}

// The model input of rank's candidate from its world rows.
static void vnt_peg2_candidate_input(const Game *root, const Move *cand,
                                     VntPeg2PickWork *work, int filled,
                                     float *input) {
  ValueNetHistory history;
  value_net_history_reset(&history);
  float scalars[VALUE_NET_SCALARS];
  value_net_scalars_for_move(root, cand, &history, scalars);
  float public_row[VNT_PEG2_PUBLIC];
  int col = 0;
  for (int idx = 0; idx < VALUE_NET_SCALARS; idx++) {
    if (idx != 54 && idx != 55 && idx != 56 && idx != 67) {
      public_row[col++] = scalars[idx];
    }
  }
  const double after = value_net_spread_after_move(root, cand);
  public_row[col++] = (float)equity_to_double(move_get_equity(cand));
  public_row[col++] = (float)after;
  vnt_peg2_pick_input(public_row, move_get_tiles_played(cand), work->rows,
                      filled, after, work->columns, work->prob, input);
}

// Scores the static top K of root (mover on turn, two tiles in the bag) and
// returns the static rank of the pick. Without a first model, scores[rank]
// is the full model's score of every rank and first_scores is unused. With
// one, first_scores[rank] is its score of every rank, and scores[rank] the
// full model's score of its best N (NAN elsewhere). With dump, writes each
// candidate's world rows (peg2feat layout; first-stage rows when staged).
static int vnt_peg2_cheap_pick(const Game *root, const VntPeg2Model *model,
                               const VntPeg2Model *first, int stage_n,
                               int top_k, VntPeg2PickWork *work,
                               Move *candidates, int *count, int *scored,
                               float *scores, float *first_scores,
                               VntPeg2PickTiming *timing, FILE *dump) {
  int64_t start = ctimer_monotonic_ns();
  int pass_rank;
  *count = vnt_peg1_pool(root, work->pool, candidates, &pass_rank);
  timing->pool_ns += ctimer_monotonic_ns() - start;
  *scored = *count < top_k ? *count : top_k;
  if (*scored == 0) {
    return -1;
  }
  start = ctimer_monotonic_ns();
  VntPeg2Prep prep;
  vnt_peg2_prep(root, &work->peg1, &prep);
  timing->prep_ns += ctimer_monotonic_ns() - start;
  float input[VNT_PEG2_PICK_INPUT];
  bool full[VNT_PEG1_POOL];
  for (int rank = 0; rank < *scored; rank++) {
    full[rank] = first == NULL;
    scores[rank] = NAN;
  }
  if (first != NULL) {
    for (int rank = 0; rank < *scored; rank++) {
      const int filled = vnt_peg2_cheap_rows(root, &prep, &candidates[rank],
                                             false, &work->peg1, work->world,
                                             work->base, work->rows, timing);
      if (dump != NULL) {
        fwrite(work->rows, sizeof(float), (size_t)filled * VNT_PEG2_FEAT_WIDTH,
               dump);
      }
      start = ctimer_monotonic_ns();
      vnt_peg2_candidate_input(root, &candidates[rank], work, filled, input);
      float opp[VNT_PEG2_OPP_INPUT];
      vnt_peg2_opp_input(input, opp);
      first_scores[rank] = vnt_peg2_model_score(first, opp);
      timing->model_ns += ctimer_monotonic_ns() - start;
    }
    // The first model's best N (ties to the better static rank).
    for (int pick = 0; pick < stage_n && pick < *scored; pick++) {
      int best = -1;
      for (int rank = 0; rank < *scored; rank++) {
        if (!full[rank] &&
            (best < 0 || first_scores[rank] > first_scores[best])) {
          best = rank;
        }
      }
      full[best] = true;
    }
  }
  int best = -1;
  for (int rank = 0; rank < *scored; rank++) {
    if (!full[rank]) {
      continue;
    }
    const int filled =
        vnt_peg2_cheap_rows(root, &prep, &candidates[rank], true, &work->peg1,
                            work->world, work->base, work->rows, timing);
    if (dump != NULL && first == NULL) {
      fwrite(work->rows, sizeof(float), (size_t)filled * VNT_PEG2_FEAT_WIDTH,
             dump);
    }
    start = ctimer_monotonic_ns();
    vnt_peg2_candidate_input(root, &candidates[rank], work, filled, input);
    scores[rank] = vnt_peg2_model_score(model, input);
    if (best < 0 || scores[rank] > scores[best]) {
      best = rank;
    }
    timing->model_ns += ctimer_monotonic_ns() - start;
  }
  return best;
}

// Fields: positions prefix, output prefix, model, first model|-, stage N,
// top K (0 = the model's), check boards, dump world rows (0|1). Writes
// OUT.picks.csv (pick and timing per board), OUT.scores.csv and, with dump,
// OUT.worlds.f32. On the first check boards the pick is recomputed with the
// bag swapped for two other unseen tiles (from the opponent's rack) and must
// be identical, scores included.
static void vnt_peg2_pick_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 9) {
    log_fatal("peg2pick needs positions prefix, output prefix, model, first "
              "model|-, stage N, top K, check boards, dump");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  VntPeg2Model *model =
      vnt_peg2_model_load(string_splitter_get_item(fields, 3));
  if (model->inputs != VNT_PEG2_PICK_INPUT) {
    log_fatal("peg2pick model must use the full cheap input");
  }
  const char *first_path = string_splitter_get_item(fields, 4);
  VntPeg2Model *first =
      strings_equal(first_path, "-") ? NULL : vnt_peg2_model_load(first_path);
  if (first != NULL && first->inputs != VNT_PEG2_OPP_INPUT) {
    log_fatal("peg2pick first model must use the opponent-reply input");
  }
  const int stage_n = atoi(string_splitter_get_item(fields, 5));
  int top_k = atoi(string_splitter_get_item(fields, 6));
  if (top_k <= 0) {
    top_k = model->top_k;
  }
  if (top_k > VNT_PEG1_POOL || (first != NULL && stage_n < 1)) {
    log_fatal("peg2pick invalid top K or stage N");
  }
  const int check_boards = atoi(string_splitter_get_item(fields, 7));
  const bool dump_rows = atoi(string_splitter_get_item(fields, 8)) != 0;
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  Game *swapped_root = game_duplicate(root);
  VntPeg2PickWork work = vnt_peg2_pick_work_create(root);
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.picks.csv", prefix);
  FILE *picks = vnt_fopen_new_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.scores.csv", prefix);
  FILE *scores_out = vnt_fopen_new_or_die(path, "w");
  free(path);
  FILE *dump = NULL;
  if (dump_rows) {
    path = get_formatted_string("%s.worlds.f32", prefix);
    dump = vnt_fopen_new_or_die(path, "w");
    free(path);
  }
  fprintf(picks, "root,candidates,scored,pick,tiles,pool_ms,prep_ms,opp_ms,"
                 "greedy_ms,model_ms,total_ms,checked\n");
  fprintf(scores_out, "root,candidate,first_score,score\n");
  path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  Move candidates[VNT_PEG1_POOL], candidates2[VNT_PEG1_POOL];
  float scores[VNT_PEG1_POOL], scores2[VNT_PEG1_POOL];
  float first_scores[VNT_PEG1_POOL], first_scores2[VNT_PEG1_POOL];
  int boards = 0;
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = vnt_peg2_next_root(positions, &line, &capacity, root,
                                           errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    const int mover = game_get_player_on_turn_index(root);
    VntPeg2PickTiming timing = {0};
    int count, scored;
    const int64_t start = ctimer_monotonic_ns();
    const int pick = vnt_peg2_cheap_pick(root, model, first, stage_n, top_k,
                                         &work, candidates, &count, &scored,
                                         scores, first_scores, &timing, dump);
    const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    if (count != expected) {
      log_fatal("peg2pick candidate replay mismatch");
    }
    bool checked = false;
    if (boards++ < check_boards) {
      int actual[2] = {-1, -1}, found = 0;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
        for (int copy = 0;
             copy < bag_get_letter(game_get_bag(root), (MachineLetter)ml) &&
             found < 2;
             copy++) {
          actual[found++] = ml;
        }
      }
      int full_unseen[VALUE_NET_TILE_TYPES], rest[VALUE_NET_TILE_TYPES];
      vnt_peg1_public_unseen(root, full_unseen);
      memcpy(rest, full_unseen, sizeof(rest));
      rest[actual[0]]--;
      rest[actual[1]]--;
      MachineLetter swap2[2];
      int picked = 0;
      for (int ml = 0; ml < VALUE_NET_TILE_TYPES && picked < 2; ml++) {
        while (rest[ml] > 0 && picked < 2 && ml != actual[0] &&
               ml != actual[1]) {
          swap2[picked++] = (MachineLetter)ml;
          rest[ml]--;
        }
      }
      if (picked == 2) {
        Move pass;
        move_set_as_pass(&pass);
        vnt_peg2_world(root, full_unseen, &pass, swap2, 0, swapped_root);
        bag_set_to_tiles(game_get_bag(swapped_root), swap2, 2);
        game_set_player_on_turn_index(swapped_root, mover);
        VntPeg2PickTiming ignored = {0};
        int count2, scored2;
        const int pick2 = vnt_peg2_cheap_pick(
            swapped_root, model, first, stage_n, top_k, &work, candidates2,
            &count2, &scored2, scores2, first_scores2, &ignored, NULL);
        if (pick2 != pick || scored2 != scored ||
            memcmp(scores, scores2, sizeof(float) * (size_t)scored) != 0 ||
            (first != NULL && memcmp(first_scores, first_scores2,
                                     sizeof(float) * (size_t)scored) != 0)) {
          log_fatal("peg2pick depends on hidden tiles");
        }
        checked = true;
      }
    }
    fprintf(picks, "%d,%d,%d,%d,%d,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%d\n", root_id,
            count, scored, pick,
            pick >= 0 ? move_get_tiles_played(&candidates[pick]) : 0,
            (double)timing.pool_ns / 1e6, (double)timing.prep_ns / 1e6,
            (double)timing.opp_ns / 1e6, (double)timing.greedy_ns / 1e6,
            (double)timing.model_ns / 1e6, total_ms, checked);
    for (int rank = 0; rank < scored; rank++) {
      fprintf(scores_out, "%d,%d,%.9g,%.9g\n", root_id, rank,
              first != NULL ? first_scores[rank] : NAN, scores[rank]);
    }
    fflush(picks);
    fflush(scores_out);
  }
  free(line);
  fclose(positions);
  fclose(picks);
  fclose(scores_out);
  if (dump != NULL) {
    fclose(dump);
  }
  error_stack_destroy(errors);
  vnt_peg2_pick_work_destroy(&work);
  vnt_peg2_model_destroy(model);
  if (first != NULL) {
    vnt_peg2_model_destroy(first);
  }
  game_destroy(swapped_root);
  config_destroy(config);
}

// ---- Wide 2PEG chooser
// -------------------------------------------------------- The first
// (opponent-reply) model scores every placement of the root (wide ranks), not
// only the static top K. With a full cheap model, that model scores the first
// model's best N and its best score is played; without one, the first model's
// best is played. Ties go to the better wide rank. As in peg2pick, only the
// public state and the mover's rack are read.
static int vnt_peg2_wide_pick(const Game *root, const VntPeg2Model *first,
                              const VntPeg2Model *model, int stage_n,
                              VntPeg2PickWork *work, MoveList *wide,
                              Move *moves, bool *taken, int *count,
                              float *first_scores, float *scores,
                              VntPeg2PickTiming *timing) {
  int64_t start = ctimer_monotonic_ns();
  *count = vnt_peg2_wide_pool(root, wide, moves);
  timing->pool_ns += ctimer_monotonic_ns() - start;
  if (*count == 0) {
    return -1;
  }
  start = ctimer_monotonic_ns();
  VntPeg2Prep prep;
  vnt_peg2_prep(root, &work->peg1, &prep);
  timing->prep_ns += ctimer_monotonic_ns() - start;
  float input[VNT_PEG2_PICK_INPUT];
  float opp[VNT_PEG2_OPP_INPUT];
  int best = -1;
  for (int rank = 0; rank < *count; rank++) {
    const int filled =
        vnt_peg2_cheap_rows(root, &prep, &moves[rank], false, &work->peg1,
                            work->world, work->base, work->rows, timing);
    start = ctimer_monotonic_ns();
    vnt_peg2_candidate_input(root, &moves[rank], work, filled, input);
    vnt_peg2_opp_input(input, opp);
    first_scores[rank] = vnt_peg2_model_score(first, opp);
    scores[rank] = NAN;
    taken[rank] = false;
    if (best < 0 || first_scores[rank] > first_scores[best]) {
      best = rank;
    }
    timing->model_ns += ctimer_monotonic_ns() - start;
  }
  if (model == NULL) {
    return best;
  }
  for (int pick = 0; pick < stage_n && pick < *count; pick++) {
    int top = -1;
    for (int rank = 0; rank < *count; rank++) {
      if (!taken[rank] && (top < 0 || first_scores[rank] > first_scores[top])) {
        top = rank;
      }
    }
    taken[top] = true;
  }
  best = -1;
  for (int rank = 0; rank < *count; rank++) {
    if (!taken[rank]) {
      continue;
    }
    const int filled =
        vnt_peg2_cheap_rows(root, &prep, &moves[rank], true, &work->peg1,
                            work->world, work->base, work->rows, timing);
    start = ctimer_monotonic_ns();
    vnt_peg2_candidate_input(root, &moves[rank], work, filled, input);
    scores[rank] = vnt_peg2_model_score(model, input);
    if (best < 0 || scores[rank] > scores[best]) {
      best = rank;
    }
    timing->model_ns += ctimer_monotonic_ns() - start;
  }
  return best;
}

// root with its two bag tiles swapped for two other unseen tiles (taken from
// the opponent's rack): the mover's information is unchanged. False when the
// unseen pool has no two such tiles.
static bool vnt_peg2_swapped_root(const Game *root, Game *swapped) {
  const int mover = game_get_player_on_turn_index(root);
  int actual[2] = {-1, -1}, found = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    for (int copy = 0;
         copy < bag_get_letter(game_get_bag(root), (MachineLetter)ml) &&
         found < 2;
         copy++) {
      actual[found++] = ml;
    }
  }
  int full_unseen[VALUE_NET_TILE_TYPES], rest[VALUE_NET_TILE_TYPES];
  vnt_peg1_public_unseen(root, full_unseen);
  memcpy(rest, full_unseen, sizeof(rest));
  rest[actual[0]]--;
  rest[actual[1]]--;
  MachineLetter swap2[2];
  int picked = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES && picked < 2; ml++) {
    while (rest[ml] > 0 && picked < 2 && ml != actual[0] && ml != actual[1]) {
      swap2[picked++] = (MachineLetter)ml;
      rest[ml]--;
    }
  }
  if (picked < 2) {
    return false;
  }
  Move pass;
  move_set_as_pass(&pass);
  vnt_peg2_world(root, full_unseen, &pass, swap2, 0, swapped);
  bag_set_to_tiles(game_get_bag(swapped), swap2, 2);
  game_set_player_on_turn_index(swapped, mover);
  return true;
}

// Fields: positions prefix, output prefix, first model, full model|-, stage
// N, check boards, dump (0 = scored rows only, 1 = every placement). Writes
// OUT.picks.csv (the pick as a wide rank, and timing) and OUT.ranks.csv
// (root, wide rank, tiles, static equity, first score, full score or NAN,
// move). On the first check boards the whole pick is recomputed with the bag
// swapped for two other unseen tiles and must be identical, scores included.
static void vnt_peg2_wide_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 8) {
    log_fatal("peg2wide needs positions prefix, output prefix, first model, "
              "full model|-, stage N, check boards, dump");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  VntPeg2Model *first =
      vnt_peg2_model_load(string_splitter_get_item(fields, 3));
  if (first->inputs != VNT_PEG2_OPP_INPUT) {
    log_fatal("peg2wide first model must use the opponent-reply input");
  }
  const char *model_path = string_splitter_get_item(fields, 4);
  VntPeg2Model *model =
      strings_equal(model_path, "-") ? NULL : vnt_peg2_model_load(model_path);
  if (model != NULL && model->inputs != VNT_PEG2_PICK_INPUT) {
    log_fatal("peg2wide full model must use the full cheap input");
  }
  const int stage_n = atoi(string_splitter_get_item(fields, 5));
  if (model != NULL && stage_n < 1) {
    log_fatal("peg2wide invalid stage N");
  }
  const int check_boards = atoi(string_splitter_get_item(fields, 6));
  const bool dump_all = atoi(string_splitter_get_item(fields, 7)) != 0;
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  Game *swapped_root = game_duplicate(root);
  VntPeg2PickWork work = vnt_peg2_pick_work_create(root);
  MoveList *wide = move_list_create(VNT_PEG2_WIDE_CAP);
  Move *moves = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  Move *moves2 = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  bool *taken = malloc_or_die(sizeof(bool) * VNT_PEG2_WIDE_CAP);
  float *first_scores = malloc_or_die(sizeof(float) * VNT_PEG2_WIDE_CAP);
  float *scores = malloc_or_die(sizeof(float) * VNT_PEG2_WIDE_CAP);
  float *first_scores2 = malloc_or_die(sizeof(float) * VNT_PEG2_WIDE_CAP);
  float *scores2 = malloc_or_die(sizeof(float) * VNT_PEG2_WIDE_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.picks.csv", prefix);
  FILE *picks = vnt_fopen_new_or_die(path, "w");
  free(path);
  path = get_formatted_string("%s.ranks.csv", prefix);
  FILE *ranks_out = vnt_fopen_new_or_die(path, "w");
  free(path);
  fprintf(picks, "root,candidates,pick,tiles,pool_ms,prep_ms,opp_ms,"
                 "greedy_ms,model_ms,total_ms,checked\n");
  fprintf(ranks_out,
          "root,candidate,tiles,static_equity,first_score,score,move\n");
  path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  char *line = NULL;
  size_t capacity = 0;
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  int boards = 0;
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = vnt_peg2_next_root(positions, &line, &capacity, root,
                                           errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    VntPeg2PickTiming timing = {0};
    int count;
    const int64_t start = ctimer_monotonic_ns();
    const int pick =
        vnt_peg2_wide_pick(root, first, model, stage_n, &work, wide, moves,
                           taken, &count, first_scores, scores, &timing);
    const double total_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    if (count < expected) {
      log_fatal("peg2wide move list shorter than the static pool");
    }
    bool checked = false;
    if (boards++ < check_boards && vnt_peg2_swapped_root(root, swapped_root)) {
      VntPeg2PickTiming ignored = {0};
      int count2;
      const int pick2 = vnt_peg2_wide_pick(swapped_root, first, model, stage_n,
                                           &work, wide, moves2, taken, &count2,
                                           first_scores2, scores2, &ignored);
      if (pick2 != pick || count2 != count ||
          memcmp(first_scores, first_scores2, sizeof(float) * (size_t)count) !=
              0 ||
          memcmp(scores, scores2, sizeof(float) * (size_t)count) != 0) {
        log_fatal("peg2wide depends on hidden tiles");
      }
      checked = true;
    }
    fprintf(picks, "%d,%d,%d,%d,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%d\n", root_id,
            count, pick, pick >= 0 ? move_get_tiles_played(&moves[pick]) : 0,
            (double)timing.pool_ns / 1e6, (double)timing.prep_ns / 1e6,
            (double)timing.opp_ns / 1e6, (double)timing.greedy_ns / 1e6,
            (double)timing.model_ns / 1e6, total_ms, checked);
    for (int rank = 0; rank < count; rank++) {
      if (!dump_all && isnan(scores[rank]) && rank != pick) {
        continue;
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &moves[rank],
                              game_get_ld(root), false);
      fprintf(ranks_out, "%d,%d,%d,%.9g,%.9g,%.9g,\"%s\"\n", root_id, rank,
              move_get_tiles_played(&moves[rank]),
              equity_to_double(move_get_equity(&moves[rank])),
              first_scores[rank], scores[rank], string_builder_peek(name));
    }
    fflush(picks);
    fflush(ranks_out);
  }
  free(line);
  fclose(positions);
  fclose(picks);
  fclose(ranks_out);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  vnt_peg2_pick_work_destroy(&work);
  move_list_destroy(wide);
  free(moves);
  free(moves2);
  free(taken);
  free(first_scores);
  free(scores);
  free(first_scores2);
  free(scores2);
  vnt_peg2_model_destroy(first);
  if (model != NULL) {
    vnt_peg2_model_destroy(model);
  }
  game_destroy(swapped_root);
  config_destroy(config);
}

// ---- 3PEG -------------------------------------------------------------------
// Three tiles in the bag, mover on turn, ten tiles unseen to the mover. The
// native 2PEG chooser (peg2pick: an opponent-reply model ranks the static top
// K, the full model scores its best N) serves as
//  - PegArgs.nested_peg2_chooser, replacing MAGPIE's staged inner peg when the
//    opponent of a 3PEG candidate faces a 2PEG;
//  - the reply policy on two-tile-bag turns when 3PEG picks are scored
//    (peg3eval), as the 1PEG network is on one-tile-bag turns.
// Like the 1PEG chooser it reads only the on-turn player's rack and the public
// state; with check on, every pick is recomputed with the bag swapped for two
// other hidden tiles and must not change.
enum {
  VNT_PEG3_UNSEEN = RACK_SIZE + 3,
  // PEG with the 1PEG network and the 2PEG chooser at nested nodes.
  VNT_PEG3_PICK_NETS_PEG = -7,
  // The same with the hidden-tile checks (both choosers and the root pick).
  VNT_PEG3_PICK_NETS_PEG_CHECKED = -8,
  // Default PEG over every root placement, without the pass.
  VNT_PEG3_PICK_DEFAULT_PEG_NO_PASS = -3,
};

typedef struct VntPeg2ChooserSlot {
  VntPeg2PickWork work;
  Move candidates[VNT_PEG1_POOL];
  Move candidates2[VNT_PEG1_POOL];
  Game *swap;
  // Open-addressing pick cache (the vnt_peg_net_key key), cleared per root
  // decision (vnt_peg2_chooser_reset).
  uint8_t *keys;
  Move *picks;
  bool *used;
  int cached;
  int64_t calls;
  int64_t hits;
  int64_t checks;
  double ms;
} VntPeg2ChooserSlot;

typedef struct VntPeg2Chooser {
  VntPeg2Model *model;
  VntPeg2Model *first;
  int stage_n;
  int top_k;
  bool check;
  bool cache;
  int slots;
  VntPeg2ChooserSlot *slot;
} VntPeg2Chooser;

static VntPeg2Chooser *vnt_peg2_chooser_create(const char *model_path,
                                               const char *first_path,
                                               int stage_n, int top_k,
                                               const Game *game, int slots) {
  VntPeg2Chooser *chooser = calloc_or_die(1, sizeof(VntPeg2Chooser));
  chooser->model = vnt_peg2_model_load(model_path);
  if (chooser->model->inputs != VNT_PEG2_PICK_INPUT) {
    log_fatal("2PEG chooser model must use the full cheap input");
  }
  chooser->first =
      strings_equal(first_path, "-") ? NULL : vnt_peg2_model_load(first_path);
  if (chooser->first != NULL && chooser->first->inputs != VNT_PEG2_OPP_INPUT) {
    log_fatal("2PEG chooser first model must use the opponent-reply input");
  }
  chooser->stage_n = stage_n;
  chooser->top_k = top_k > 0 ? top_k : chooser->model->top_k;
  if (chooser->top_k > VNT_PEG1_POOL ||
      (chooser->first != NULL && stage_n < 1)) {
    log_fatal("2PEG chooser invalid top K or stage N");
  }
  chooser->slots = slots;
  chooser->slot = calloc_or_die((size_t)slots, sizeof(VntPeg2ChooserSlot));
  for (int slot_idx = 0; slot_idx < slots; slot_idx++) {
    chooser->slot[slot_idx].work = vnt_peg2_pick_work_create(game);
    chooser->slot[slot_idx].swap = game_duplicate(game);
    chooser->slot[slot_idx].keys =
        malloc_or_die((size_t)VNT_PEG_CACHE_SLOTS * VNT_PEG_KEY_BYTES);
    chooser->slot[slot_idx].picks =
        malloc_or_die(sizeof(Move) * VNT_PEG_CACHE_SLOTS);
    chooser->slot[slot_idx].used =
        calloc_or_die(VNT_PEG_CACHE_SLOTS, sizeof(bool));
  }
  return chooser;
}

static void vnt_peg2_chooser_reset(VntPeg2Chooser *chooser) {
  for (int slot_idx = 0; slot_idx < chooser->slots; slot_idx++) {
    memset(chooser->slot[slot_idx].used, 0, sizeof(bool) * VNT_PEG_CACHE_SLOTS);
    chooser->slot[slot_idx].cached = 0;
  }
}

static void vnt_peg2_chooser_destroy(VntPeg2Chooser *chooser) {
  for (int slot_idx = 0; slot_idx < chooser->slots; slot_idx++) {
    vnt_peg2_pick_work_destroy(&chooser->slot[slot_idx].work);
    game_destroy(chooser->slot[slot_idx].swap);
    free(chooser->slot[slot_idx].keys);
    free(chooser->slot[slot_idx].picks);
    free(chooser->slot[slot_idx].used);
  }
  free(chooser->slot);
  vnt_peg2_model_destroy(chooser->model);
  if (chooser->first != NULL) {
    vnt_peg2_model_destroy(chooser->first);
  }
  free(chooser);
}

// The chooser's full pick for game (the static rank, -1 = pass) into move.
static int vnt_peg2_chooser_choose(VntPeg2Chooser *chooser,
                                   VntPeg2ChooserSlot *slot, const Game *game,
                                   Move *candidates, Move *move) {
  VntPeg2PickTiming timing = {0};
  int count, scored;
  float scores[VNT_PEG1_POOL], first_scores[VNT_PEG1_POOL];
  const int pick = vnt_peg2_cheap_pick(game, chooser->model, chooser->first,
                                       chooser->stage_n, chooser->top_k,
                                       &slot->work, candidates, &count, &scored,
                                       scores, first_scores, &timing, NULL);
  if (pick < 0) {
    move_set_as_pass(move);
  } else {
    move_copy(move, &candidates[pick]);
  }
  return pick;
}

// The chooser's pick for the player on turn when the bag holds two tiles and
// nine tiles are unseen to that player; false otherwise. With chooser->check,
// a cached pick is recomputed, and the pick is recomputed with the bag swapped
// for two other hidden tiles; both must match.
static bool vnt_peg2_chooser_pick(VntPeg2Chooser *chooser, int slot_idx,
                                  const Game *game, Move *move) {
  int unseen[VALUE_NET_TILE_TYPES];
  if (game_over(game) || bag_get_letters(game_get_bag(game)) != 2 ||
      vnt_peg1_public_unseen(game, unseen) != VNT_PEG2_UNSEEN) {
    return false;
  }
  VntPeg2ChooserSlot *slot = &chooser->slot[slot_idx];
  const int64_t start = ctimer_monotonic_ns();
  uint8_t key[VNT_PEG_KEY_BYTES];
  int entry = -1;
  bool hit = false;
  if (chooser->cache) {
    entry = (int)(vnt_peg_net_key(game, key) % VNT_PEG_CACHE_SLOTS);
    while (slot->used[entry] &&
           memcmp(&slot->keys[(size_t)entry * VNT_PEG_KEY_BYTES], key,
                  VNT_PEG_KEY_BYTES) != 0) {
      entry = (entry + 1) % VNT_PEG_CACHE_SLOTS;
    }
    hit = slot->used[entry];
  }
  if (hit) {
    move_copy(move, &slot->picks[entry]);
    slot->hits++;
  } else {
    vnt_peg2_chooser_choose(chooser, slot, game, slot->candidates, move);
    // Keep the table at most half full so probes stay short.
    if (chooser->cache && slot->cached < VNT_PEG_CACHE_SLOTS / 2) {
      memcpy(&slot->keys[(size_t)entry * VNT_PEG_KEY_BYTES], key,
             VNT_PEG_KEY_BYTES);
      move_copy(&slot->picks[entry], move);
      slot->used[entry] = true;
      slot->cached++;
    }
  }
  slot->ms += (double)(ctimer_monotonic_ns() - start) / 1e6;
  slot->calls++;
  if (chooser->check) {
    Move fresh;
    if (hit) {
      vnt_peg2_chooser_choose(chooser, slot, game, slot->candidates, &fresh);
      if (compare_moves(move, &fresh, true) != -1) {
        log_fatal("2PEG chooser pick cache returned a different move");
      }
    }
    if (vnt_peg2_swapped_root(game, slot->swap)) {
      vnt_peg2_chooser_choose(chooser, slot, slot->swap, slot->candidates2,
                              &fresh);
      if (compare_moves(move, &fresh, true) != -1) {
        log_fatal("2PEG chooser pick depends on the hidden tiles");
      }
      slot->checks++;
    }
  }
  return true;
}

static bool vnt_peg2_chooser_cb(const Game *game, int worker_idx, void *ctx,
                                Move *move) {
  VntPeg2Chooser *chooser = ctx;
  if (worker_idx < 0 || worker_idx >= chooser->slots) {
    log_fatal("2PEG chooser has no slot for PEG worker %d", worker_idx);
  }
  return vnt_peg2_chooser_pick(chooser, worker_idx, game, move);
}

static void vnt_peg2_chooser_totals(const VntPeg2Chooser *chooser,
                                    int64_t *calls, int64_t *checks, double *ms,
                                    int64_t *hits) {
  *calls = 0;
  *checks = 0;
  *ms = 0;
  *hits = 0;
  for (int slot_idx = 0; slot_idx < chooser->slots; slot_idx++) {
    *calls += chooser->slot[slot_idx].calls;
    *checks += chooser->slot[slot_idx].checks;
    *ms += chooser->slot[slot_idx].ms;
    *hits += chooser->slot[slot_idx].hits;
  }
}

static void vnt_peg3_positions(const StringSplitter *fields) {
  vnt_pegn_positions(fields, 3);
}

// Plays the turns that still draw: a one-tile bag by the 1PEG network, a
// two-tile bag by the 2PEG chooser, anything else (a three-tile bag after a
// root pass, or a declined pick) by static top 1; with reply peg, every
// drawing turn by default PEG.
static int vnt_peg3_drain(Game *game, VntPegNet *net, VntPeg2Chooser *chooser,
                          int slot_idx, VntPeg2Reply reply,
                          MoveList *static_list, double *reply_ms,
                          int *static_turns) {
  int turns = 0;
  while (!game_over(game) && bag_get_letters(game_get_bag(game)) > 0 &&
         turns < VNT_MAX_TURNS) {
    Move next;
    double ms = 0;
    const int64_t start = ctimer_monotonic_ns();
    if (reply == VNT_PEG2_REPLY_PEG) {
      vnt_peg_pick(game, 1, NULL, NULL, &next, &ms);
    } else if (vnt_peg_net_pick(net, slot_idx, game, &next) ||
               vnt_peg2_chooser_pick(chooser, slot_idx, game, &next)) {
      ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
    } else {
      move_copy(&next, vnt_static_move(game, static_list));
      (*static_turns)++;
    }
    *reply_ms += ms;
    play_move(&next, game, NULL);
    turns++;
  }
  return turns;
}

// Exact continuation of 3PEG picks in the actual world: the pick is played
// from the root (draws come from the root's bag in its stored order, the same
// for every code), the drawing turns are played by vnt_peg3_drain and the
// endgame is solved exactly (as in peg2eval). Fields: positions prefix,
// choices (root,code rows), output prefix, reply (net|peg), plies, tt
// fraction, 1PEG first, second|-, shortlist, w_spread, scale, 2PEG model,
// 2PEG first model|-, stage N, top K (0 = the model's). Codes: >= 0 a static
// rank; -2 default PEG; -3 default PEG over every placement but not the pass;
// -4 PEG with the 1PEG network at one-tile nested nodes; -7 PEG with both
// choosers; -8 = -7 with every hidden-tile check, including the root pick
// recomputed with one bag tile swapped for a tile of the opponent's rack.
// Codes -3, -4, -7 and -8 never pass at the root (PegArgs.only_moves = every
// placement): a root pass leaves the opponent a 3PEG, which no chooser covers,
// so PEG solves it with the full staged inner peg. VNT_PEG_NEST_DEPTH sets
// PegArgs.nested_max_depth for -4, -7 and -8 (default
// PEG_NESTED_DEFAULT_DEPTH); VNT_PEG2_MAX_STAGE and VNT_PEG2_BUDGET_S apply to
// every PEG pick with two or more tiles in the bag, VNT_PEG_THREADS to the
// root picks.
static void vnt_peg3_eval(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 16) {
    log_fatal("peg3eval needs positions prefix, choices, output prefix, "
              "reply, plies, tt fraction, first, second|-, shortlist, "
              "w_spread, scale, 2PEG model, 2PEG first|-, stage N, top K");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *choices_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const char *reply_name = string_splitter_get_item(fields, 4);
  const int plies = atoi(string_splitter_get_item(fields, 5));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 6), NULL);
  VntPeg2Reply reply;
  if (strings_equal(reply_name, "net")) {
    reply = VNT_PEG2_REPLY_NET;
  } else if (strings_equal(reply_name, "peg")) {
    reply = VNT_PEG2_REPLY_PEG;
  } else {
    log_fatal("peg3eval reply must be net or peg");
  }
  if (plies < 1 || plies > 25 || tt_fraction <= 0 || tt_fraction > 0.1) {
    log_fatal("peg3eval invalid plies or tt fraction");
  }
  const char *depth_env = getenv("VNT_PEG_NEST_DEPTH");
  const int nest_depth = depth_env != NULL && atoi(depth_env) > 0
                             ? atoi(depth_env)
                             : PEG_NESTED_DEFAULT_DEPTH;
  // VNT_PEG_THREADS > 1 runs each root PEG pick on that many threads. PEG
  // hands the choosers worker indices 0..threads (the main thread helps the
  // pool), so they get threads + 1 slots.
  const char *threads_env = getenv("VNT_PEG_THREADS");
  const int pick_threads =
      threads_env != NULL && atoi(threads_env) > 1 ? atoi(threads_env) : 1;
  int num_picks = 0, pick_capacity = 64;
  int *pick_root = malloc_or_die(sizeof(int) * pick_capacity);
  int *pick_code = malloc_or_die(sizeof(int) * pick_capacity);
  char *line = NULL;
  size_t capacity = 0;
  FILE *choices = fopen_or_die(choices_path, "r");
  while (getline_ignore_carriage_return(&line, &capacity, choices) >= 0) {
    int root_id, code;
    if (sscanf(line, "%d,%d", &root_id, &code) != 2) {
      continue;
    }
    if (num_picks == pick_capacity) {
      pick_capacity *= 2;
      pick_root = realloc_or_die(pick_root, sizeof(int) * pick_capacity);
      pick_code = realloc_or_die(pick_code, sizeof(int) * pick_capacity);
    }
    pick_root[num_picks] = root_id;
    pick_code[num_picks++] = code;
  }
  fclose(choices);
  bool *pick_done =
      calloc_or_die((size_t)(num_picks > 0 ? num_picks : 1), sizeof(bool));
  char *out_path = get_formatted_string("%s.peg3.csv", prefix);
  FILE *previous = fopen(out_path, "r");
  bool fresh = previous == NULL;
  if (previous != NULL) {
    fresh = fgetc(previous) == EOF;
    rewind(previous);
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id, code;
      if (sscanf(line, "%d,%d", &root_id, &code) == 2) {
        for (int idx = 0; idx < num_picks; idx++) {
          if (pick_root[idx] == root_id && pick_code[idx] == code) {
            pick_done[idx] = true;
          }
        }
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  if (fresh) {
    fprintf(output, "root,code,choice,tiles,pass,final,win,proven,solve_ms,"
                    "reply_ms,reply_turns,static_turns,pick_ms,net1_calls,"
                    "net1_hits,net1_ms,net2_calls,net2_hits,net2_ms,checks,"
                    "nest_depth,move\n");
  }
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  Game *game = game_duplicate(root);
  Game *swap = game_duplicate(root);
  VntPegNet *net = vnt_peg_net_create(
      string_splitter_get_item(fields, 7), string_splitter_get_item(fields, 8),
      atoi(string_splitter_get_item(fields, 9)),
      strtod(string_splitter_get_item(fields, 10), NULL),
      strtod(string_splitter_get_item(fields, 11), NULL), root,
      pick_threads + 1);
  VntPeg2Chooser *chooser = vnt_peg2_chooser_create(
      string_splitter_get_item(fields, 12),
      string_splitter_get_item(fields, 13),
      atoi(string_splitter_get_item(fields, 14)),
      atoi(string_splitter_get_item(fields, 15)), root, pick_threads + 1);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *static_list = move_list_create(1);
  MoveList *wide = move_list_create(VNT_PEG2_WIDE_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  const Move **placements = malloc_or_die(sizeof(Move *) * VNT_PEG2_WIDE_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  VntPeg1Exact exact = vnt_peg1_exact_create(plies, tt_fraction);
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = vnt_peg2_next_root(positions, &line, &capacity, root,
                                           errors, 3, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    bool pending = false;
    for (int idx = 0; idx < num_picks; idx++) {
      pending |= pick_root[idx] == root_id && !pick_done[idx];
    }
    if (!pending) {
      continue;
    }
    const int seat = game_get_player_on_turn_index(root);
    int unseen[VALUE_NET_TILE_TYPES];
    if (vnt_peg1_public_unseen(root, unseen) != VNT_PEG3_UNSEEN) {
      log_fatal("peg3eval root does not have ten unseen tiles");
    }
    Move candidates[VNT_PEG1_POOL];
    int pass_rank;
    const int count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("peg3eval candidate replay mismatch");
    }
    const int n_placements = vnt_peg2_wide_pool(root, wide, wide_moves);
    for (int rank = 0; rank < n_placements; rank++) {
      placements[rank] = &wide_moves[rank];
    }
    for (int idx = 0; idx < num_picks; idx++) {
      if (pick_root[idx] != root_id || pick_done[idx]) {
        continue;
      }
      const int code = pick_code[idx];
      int64_t calls1_0, checks1_0, calls2_0, checks2_0, hits2_0;
      double ms1_0, ms2_0;
      vnt_peg_net_totals(net, &calls1_0, &checks1_0, &ms1_0);
      vnt_peg2_chooser_totals(chooser, &calls2_0, &checks2_0, &ms2_0, &hits2_0);
      const int64_t hits1_0 = vnt_peg_net_hits(net);
      Move pick;
      double pick_ms = 0;
      int depth_used = PEG_NESTED_DEFAULT_DEPTH;
      int root_checks = 0;
      if (code >= 0) {
        if (code >= count) {
          log_fatal("peg3eval invalid candidate");
        }
        move_copy(&pick, &candidates[code]);
        depth_used = 0;
      } else if (code == VNT_PEG2_PICK_DEFAULT_PEG) {
        vnt_peg_pick(root, pick_threads, NULL, NULL, &pick, &pick_ms);
      } else if (code == VNT_PEG3_PICK_DEFAULT_PEG_NO_PASS) {
        vnt_peg_pick_only(root, pick_threads, NULL, NULL, placements,
                          n_placements, &pick, &pick_ms);
      } else if (code == VNT_PEG2_PICK_NET_PEG ||
                 code == VNT_PEG3_PICK_NETS_PEG ||
                 code == VNT_PEG3_PICK_NETS_PEG_CHECKED) {
        const bool both = code != VNT_PEG2_PICK_NET_PEG;
        const bool check = code == VNT_PEG3_PICK_NETS_PEG_CHECKED;
        net->check = check;
        net->cache = true;
        chooser->check = check;
        chooser->cache = true;
        depth_used = nest_depth;
        vnt_peg_net_reset(net);
        vnt_peg2_chooser_reset(chooser);
        vnt_peg_pick_nets(root, pick_threads, vnt_peg_net_chooser, net,
                          both ? vnt_peg2_chooser_cb : NULL,
                          both ? chooser : NULL, nest_depth, placements,
                          n_placements, &pick, &pick_ms);
        if (check) {
          // Swap one bag tile with a tile from the opponent's rack: the
          // mover's information is unchanged, so the pick must be too.
          MachineLetter bag3[3];
          const int held = bag_peek_tiles(game_get_bag(root), bag3);
          const Rack *opp_rack =
              player_get_rack(game_get_player(root, 1 - seat));
          int other = -1;
          for (int ml = 0; ml < VALUE_NET_TILE_TYPES && other < 0; ml++) {
            if (ml != bag3[0] && rack_get_letter(opp_rack, ml) > 0) {
              other = ml;
            }
          }
          if (held == 3 && other >= 0) {
            game_copy(swap, root);
            Rack *swap_rack = player_get_rack(game_get_player(swap, 1 - seat));
            rack_take_letter(swap_rack, (MachineLetter)other);
            rack_add_letter(swap_rack, bag3[0]);
            const MachineLetter swapped_bag[3] = {(MachineLetter)other, bag3[1],
                                                  bag3[2]};
            bag_set_to_tiles(game_get_bag(swap), swapped_bag, 3);
            Move private_pick;
            double ignored;
            vnt_peg_net_reset(net);
            vnt_peg2_chooser_reset(chooser);
            vnt_peg_pick_nets(swap, pick_threads, vnt_peg_net_chooser, net,
                              vnt_peg2_chooser_cb, chooser, nest_depth,
                              placements, n_placements, &private_pick,
                              &ignored);
            if (compare_moves(&pick, &private_pick, true) != -1) {
              log_fatal("network 3PEG pick depends on the hidden tiles");
            }
            root_checks = 1;
          }
        }
        net->check = false;
        chooser->check = false;
        // The continuation's reply policy does not use the cache.
        net->cache = false;
        chooser->cache = false;
      } else {
        log_fatal("peg3eval invalid pick code");
      }
      int64_t calls1_1, checks1_1, calls2_1, checks2_1, hits2_1;
      double ms1_1, ms2_1;
      vnt_peg_net_totals(net, &calls1_1, &checks1_1, &ms1_1);
      vnt_peg2_chooser_totals(chooser, &calls2_1, &checks2_1, &ms2_1, &hits2_1);
      const int64_t hits1_1 = vnt_peg_net_hits(net);
      int choice = -1;
      for (int cand = 0; cand < count && choice < 0; cand++) {
        if (compare_moves(&pick, &candidates[cand], true) == -1) {
          choice = cand;
        }
      }
      game_copy(game, root);
      play_move(&pick, game, NULL);
      double reply_ms = 0;
      int static_turns = 0;
      const int reply_turns = vnt_peg3_drain(
          game, net, chooser, 0, reply, static_list, &reply_ms, &static_turns);
      bool proven = true;
      double solve_ms = 0;
      double final = vnt_peg1_margin(game, seat);
      if (!game_over(game)) {
        if (bag_get_letters(game_get_bag(game)) != 0) {
          log_fatal("peg3eval bag not empty before the exact solve");
        }
        const int on_turn = game_get_player_on_turn_index(game);
        const int64_t start = ctimer_monotonic_ns();
        const int value = vnt_peg1_exact_value(&exact, game, &proven);
        solve_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
        final += on_turn == seat ? value : -value;
        if (exact.capped) {
          char *capped_path = get_formatted_string("%s.capped.csv", prefix);
          FILE *capped = fopen_or_die(capped_path, "a");
          free(capped_path);
          fprintf(capped, "%d,%d,%d,%d,%.9g\n", root_id, code, exact.depth,
                  value, solve_ms);
          fclose(capped);
        }
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      fprintf(output,
              "%d,%d,%d,%d,%d,%.9g,%.1f,%d,%.9g,%.9g,%d,%d,%.9g,%lld,%lld,%"
              ".9g,%lld,%lld,%.9g,%lld,%d,\"%s\"\n",
              root_id, code, choice, move_get_tiles_played(&pick),
              move_get_type(&pick) == GAME_EVENT_PASS, final,
              final > 0   ? 1.0
              : final < 0 ? 0.0
                          : .5,
              proven, solve_ms, reply_ms, reply_turns, static_turns, pick_ms,
              (long long)(calls1_1 - calls1_0), (long long)(hits1_1 - hits1_0),
              ms1_1 - ms1_0, (long long)(calls2_1 - calls2_0),
              (long long)(hits2_1 - hits2_0), ms2_1 - ms2_0,
              (long long)(checks1_1 - checks1_0 + checks2_1 - checks2_0 +
                          root_checks),
              depth_used, string_builder_peek(name));
      fflush(output);
      fsync(fileno(output));
      pick_done[idx] = true;
    }
  }
  free(line);
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  vnt_peg1_exact_destroy(&exact);
  move_list_destroy(pool);
  move_list_destroy(static_list);
  move_list_destroy(wide);
  free(wide_moves);
  free(placements);
  vnt_peg_net_destroy(net);
  vnt_peg2_chooser_destroy(chooser);
  game_destroy(game);
  game_destroy(swap);
  config_destroy(config);
  free(pick_root);
  free(pick_code);
  free(pick_done);
}

// ---- 3PEG oracle (tier A) ---------------------------------------------------
// Scores fixed 3PEG root picks over every hidden world consistent with the
// mover's information: each ordered three-tile bag drawn from the ten unseen
// tiles (the opponent holds the other seven), weighted by its number of
// labelled-tile assignments, so worlds are uniform over the deals. In each
// world the pick is played, the drawing turns are played by the nets
// (vnt_peg3_drain, 1PEG net at one tile, 2PEG chooser at two, static
// otherwise) and the endgame is solved exactly. A pick that plays three or
// more tiles draws the whole bag, so its worlds are the bag multisets.
// VNT_ORACLE_BAG (1, 2 or 3; default 3) scores roots with that many tiles in
// the bag the same way: ordered bags of that size from the RACK_SIZE + bag
// unseen tiles, multisets when the pick draws the whole bag. With
// VNT_ORACLE_SAMPLES = K > 0 the worlds are instead K uniformly random deals
// of the unseen tiles (bag order and the opponent's rack; the same K for every
// pick of a root, from VNT_ORACLE_SEED + root), weight 1 each, plus the root's
// actual deal at weight 0 (reported, not averaged); bags up to RACK_SIZE.
enum {
  VNT_PEG3_ORACLE_MAX_WORLDS = 720,
  VNT_PEG3_ORACLE_MAX_PICKS = 16,
  VNT_PEG3_ORACLE_MAX_THREADS = 64,
  VNT_PEG3_ORACLE_MAX_BAG = RACK_SIZE,
  VNT_PEG3_ORACLE_MAX_ENUM_BAG = 3,
};

typedef struct VntPeg3World {
  MachineLetter bag[VNT_PEG3_ORACLE_MAX_BAG];
  int64_t weight;
  bool actual;
  double final;
  bool proven;
  bool capped;
  int static_turns;
} VntPeg3World;

typedef struct VntPeg3OracleWorker {
  const Game *root;
  const Move *pick;
  int bag_size;
  VntPeg3World *worlds;
  int n_worlds;
  atomic_int *next;
  VntPegNet *net;
  VntPeg2Chooser *chooser;
  int slot_idx;
  Game *game;
  MoveList *static_list;
  VntPeg1Exact exact;
} VntPeg3OracleWorker;

// Distinct orderings of a multiset of up to VNT_PEG3_ORACLE_MAX_BAG tiles.
static int vnt_peg3_oracle_orders(const MachineLetter *tiles, int bag_size) {
  int orders = 1;
  for (int idx = 2; idx <= bag_size; idx++) {
    orders *= idx;
  }
  for (int idx = 0; idx < bag_size; idx++) {
    bool first_copy = true;
    int copies = 0;
    for (int other = 0; other < bag_size; other++) {
      first_copy &= other >= idx || tiles[other] != tiles[idx];
      copies += tiles[other] == tiles[idx];
    }
    for (int factor = 2; first_copy && factor <= copies; factor++) {
      orders /= factor;
    }
  }
  return orders;
}

// Appends the bags that extend tiles[0..depth) to worlds: ordered draws, or
// non-decreasing multisets (each weighted by its orderings) when whole_bag.
// weight is the product of the remaining counts drawn so far.
static void vnt_peg3_oracle_extend(int *count, MachineLetter *tiles, int depth,
                                   int bag_size, bool whole_bag, int64_t weight,
                                   const MachineLetter *actual,
                                   VntPeg3World *worlds, int *n_worlds) {
  if (depth == bag_size) {
    VntPeg3World *world = &worlds[(*n_worlds)++];
    memset(world, 0, sizeof(*world));
    world->actual = true;
    for (int idx = 0; idx < bag_size; idx++) {
      world->bag[idx] = tiles[idx];
      world->actual &= tiles[idx] == actual[idx];
    }
    world->weight =
        whole_bag ? weight * vnt_peg3_oracle_orders(tiles, bag_size) : weight;
    return;
  }
  const int lowest = whole_bag && depth > 0 ? tiles[depth - 1] : 0;
  for (int ml = lowest; ml < VALUE_NET_TILE_TYPES; ml++) {
    if (count[ml] == 0) {
      continue;
    }
    tiles[depth] = (MachineLetter)ml;
    const int64_t extended = weight * count[ml]--;
    vnt_peg3_oracle_extend(count, tiles, depth + 1, bag_size, whole_bag,
                           extended, actual, worlds, n_worlds);
    count[ml]++;
  }
}

// The worlds for a pick from the root's unseen counts; the root's own bag
// order is flagged as the actual world. Returns the number of worlds.
static int vnt_peg3_oracle_worlds(const Game *root, const int *unseen,
                                  int bag_size, bool whole_bag,
                                  VntPeg3World *worlds) {
  MachineLetter actual[RACK_SIZE];
  if (bag_peek_tiles(game_get_bag(root), actual) != bag_size) {
    log_fatal("peg3oracle root bag does not hold %d tiles", bag_size);
  }
  if (whole_bag) {
    for (int lo = 0; lo < bag_size - 1; lo++) {
      for (int hi = lo + 1; hi < bag_size; hi++) {
        if (actual[hi] < actual[lo]) {
          const MachineLetter tmp = actual[lo];
          actual[lo] = actual[hi];
          actual[hi] = tmp;
        }
      }
    }
  }
  int count[VALUE_NET_TILE_TYPES];
  memcpy(count, unseen, sizeof(count));
  MachineLetter tiles[VNT_PEG3_ORACLE_MAX_BAG];
  int n_worlds = 0;
  vnt_peg3_oracle_extend(count, tiles, 0, bag_size, whole_bag, 1, actual,
                         worlds, &n_worlds);
  return n_worlds;
}

// The sampled worlds: the root's actual deal (weight 0) and samples uniformly
// random deals of the unseen tiles (weight 1), the same for every pick of the
// root. Returns the number of worlds.
static int vnt_peg3_oracle_sample_worlds(const Game *root, const int *unseen,
                                         int bag_size, int samples,
                                         uint64_t seed, VntPeg3World *worlds) {
  MachineLetter tiles[2 * RACK_SIZE];
  int n_unseen = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    for (int copy = 0; copy < unseen[ml]; copy++) {
      tiles[n_unseen++] = (MachineLetter)ml;
    }
  }
  memset(&worlds[0], 0, sizeof(worlds[0]));
  if (bag_peek_tiles(game_get_bag(root), worlds[0].bag) != bag_size) {
    log_fatal("peg3oracle root bag does not hold %d tiles", bag_size);
  }
  worlds[0].actual = true;
  XoshiroPRNG *prng = prng_create(seed);
  for (int world_idx = 1; world_idx <= samples; world_idx++) {
    for (int idx = n_unseen - 1; idx > 0; idx--) {
      const int other = (int)prng_get_random_number(prng, (uint64_t)idx + 1);
      const MachineLetter tmp = tiles[idx];
      tiles[idx] = tiles[other];
      tiles[other] = tmp;
    }
    VntPeg3World *world = &worlds[world_idx];
    memset(world, 0, sizeof(*world));
    memcpy(world->bag, tiles, sizeof(MachineLetter) * (size_t)bag_size);
    world->weight = 1;
  }
  prng_destroy(prng);
  return samples + 1;
}

// root with the opponent's rack and the bag replaced by one world.
static void vnt_peg3_oracle_set_world(const Game *root, const int *unseen,
                                      const MachineLetter *bag_tiles,
                                      int bag_size, Game *world) {
  const int mover = game_get_player_on_turn_index(root);
  game_copy(world, root);
  int rest[VALUE_NET_TILE_TYPES];
  memcpy(rest, unseen, sizeof(rest));
  for (int idx = 0; idx < bag_size; idx++) {
    rest[bag_tiles[idx]]--;
  }
  Rack *opponent = player_get_rack(game_get_player(world, 1 - mover));
  rack_reset(opponent);
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    if (rest[ml] < 0) {
      log_fatal("peg3oracle world uses an unavailable tile");
    }
    for (int copy = 0; copy < rest[ml]; copy++) {
      rack_add_letter(opponent, (MachineLetter)ml);
    }
  }
  bag_set_to_tiles(game_get_bag(world), bag_tiles, bag_size);
}

static void *vnt_peg3_oracle_worker(void *arg) {
  VntPeg3OracleWorker *worker = arg;
  const int seat = game_get_player_on_turn_index(worker->root);
  int unseen[VALUE_NET_TILE_TYPES];
  vnt_peg1_public_unseen(worker->root, unseen);
  while (true) {
    const int world_idx = atomic_fetch_add(worker->next, 1);
    if (world_idx >= worker->n_worlds) {
      break;
    }
    VntPeg3World *world = &worker->worlds[world_idx];
    vnt_peg3_oracle_set_world(worker->root, unseen, world->bag,
                              worker->bag_size, worker->game);
    play_move(worker->pick, worker->game, NULL);
    double reply_ms = 0;
    world->static_turns = 0;
    (void)vnt_peg3_drain(worker->game, worker->net, worker->chooser,
                         worker->slot_idx, VNT_PEG2_REPLY_NET,
                         worker->static_list, &reply_ms, &world->static_turns);
    double final = vnt_peg1_margin(worker->game, seat);
    world->proven = true;
    world->capped = false;
    if (!game_over(worker->game)) {
      if (bag_get_letters(game_get_bag(worker->game)) != 0) {
        log_fatal("peg3oracle bag not empty before the exact solve");
      }
      const int on_turn = game_get_player_on_turn_index(worker->game);
      const int value =
          vnt_peg1_exact_value(&worker->exact, worker->game, &world->proven);
      final += on_turn == seat ? value : -value;
      world->capped = worker->exact.capped;
    }
    world->final = final;
  }
  return NULL;
}

// The root move whose string (string_builder_add_move, as peg3eval writes it)
// is text: a placement from the wide pool or one of the static top 64. An
// empty text names the static top-64 candidate of rank code (vnt_peg1_pool).
static bool vnt_peg3_oracle_find_move(const Game *root, const char *text,
                                      int code, MoveList *wide,
                                      Move *wide_moves, MoveList *pool,
                                      StringBuilder *name, Move *move) {
  Move candidates[VNT_PEG1_POOL];
  int pass_rank;
  if (text[0] == '\0') {
    const int count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
    if (code < 0 || code >= count) {
      return false;
    }
    move_copy(move, &candidates[code]);
    return true;
  }
  const int n_placements = vnt_peg2_wide_pool(root, wide, wide_moves);
  const int count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
  for (int idx = 0; idx < n_placements + count; idx++) {
    const Move *cand =
        idx < n_placements ? &wide_moves[idx] : &candidates[idx - n_placements];
    string_builder_clear(name);
    string_builder_add_move(name, game_get_board(root), cand, game_get_ld(root),
                            false);
    if (strings_equal(string_builder_peek(name), text)) {
      move_copy(move, cand);
      return true;
    }
  }
  return false;
}

// Two pick rows name the same move: equal texts, or equal ranks for rank
// picks (empty text).
static bool vnt_peg3_oracle_same_pick(const char *text, int code,
                                      const char *other_text, int other_code) {
  return strings_equal(text, other_text) &&
         (text[0] != '\0' || code == other_code);
}

// Fields: positions prefix, picks (a peg3eval .peg3.csv: root, code, ...,
// quoted move; an empty quoted move picks the static rank code), output prefix,
// plies, tt fraction per thread, 1PEG first, second|-, shortlist, w_spread,
// scale, 2PEG model, 2PEG first|-, stage N, top K. VNT_ORACLE_THREADS (default
// 1) threads share each pick's worlds; VNT_EXACT_CAP_S caps each world's exact
// solve. Writes PREFIX.oracle.csv (one row per root and distinct pick: the
// codes that chose it, weighted win% (W=1, T=.5), mean final spread, counts,
// and the actual world's final) and PREFIX.worlds.csv (every world's bag,
// weight and final). Resumes by root.
static void vnt_peg3_oracle(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 15) {
    log_fatal("peg3oracle needs positions prefix, picks, output prefix, "
              "plies, tt fraction, first, second|-, shortlist, w_spread, "
              "scale, 2PEG model, 2PEG first|-, stage N, top K");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *picks_path = string_splitter_get_item(fields, 2);
  const char *prefix = string_splitter_get_item(fields, 3);
  const int plies = atoi(string_splitter_get_item(fields, 4));
  const double tt_fraction = strtod(string_splitter_get_item(fields, 5), NULL);
  const char *threads_env = getenv("VNT_ORACLE_THREADS");
  const int threads =
      threads_env != NULL && atoi(threads_env) > 1 ? atoi(threads_env) : 1;
  const char *bag_env = getenv("VNT_ORACLE_BAG");
  const int bag_size = bag_env != NULL ? atoi(bag_env) : 3;
  const char *samples_env = getenv("VNT_ORACLE_SAMPLES");
  const int samples = samples_env != NULL ? atoi(samples_env) : 0;
  const char *seed_env = getenv("VNT_ORACLE_SEED");
  const uint64_t sample_seed =
      seed_env != NULL ? strtoull(seed_env, NULL, 10) : 0;
  if (plies < 1 || plies > 25 || tt_fraction <= 0 ||
      tt_fraction * threads > 0.4 || threads > VNT_PEG3_ORACLE_MAX_THREADS ||
      bag_size < 1 ||
      bag_size > (samples > 0 ? VNT_PEG3_ORACLE_MAX_BAG
                              : VNT_PEG3_ORACLE_MAX_ENUM_BAG) ||
      samples < 0 || samples + 1 > VNT_PEG3_ORACLE_MAX_WORLDS) {
    log_fatal("peg3oracle invalid plies, tt fraction, threads, bag or samples");
  }
  // The picks: root, code and move text per row.
  int num_picks = 0, pick_capacity = 64;
  int *pick_root = malloc_or_die(sizeof(int) * (size_t)pick_capacity);
  int *pick_code = malloc_or_die(sizeof(int) * (size_t)pick_capacity);
  char **pick_text = malloc_or_die(sizeof(char *) * (size_t)pick_capacity);
  char *line = NULL;
  size_t capacity = 0;
  FILE *picks = fopen_or_die(picks_path, "r");
  (void)getline_ignore_carriage_return(&line, &capacity, picks);
  while (getline_ignore_carriage_return(&line, &capacity, picks) > 0) {
    int root_id, code;
    char *open = strchr(line, '"'), *close = strrchr(line, '"');
    if (sscanf(line, "%d,%d,", &root_id, &code) != 2 || open == NULL ||
        close <= open) {
      log_fatal("peg3oracle invalid pick row");
    }
    *close = '\0';
    if (num_picks == pick_capacity) {
      pick_capacity *= 2;
      pick_root =
          realloc_or_die(pick_root, sizeof(int) * (size_t)pick_capacity);
      pick_code =
          realloc_or_die(pick_code, sizeof(int) * (size_t)pick_capacity);
      pick_text =
          realloc_or_die(pick_text, sizeof(char *) * (size_t)pick_capacity);
    }
    pick_root[num_picks] = root_id;
    pick_code[num_picks] = code;
    pick_text[num_picks] = string_duplicate(open + 1);
    num_picks++;
  }
  fclose(picks);
  // Roots already in the output are done.
  char *out_path = get_formatted_string("%s.oracle.csv", prefix);
  bool *root_done = calloc_or_die((size_t)num_picks, sizeof(bool));
  FILE *previous = fopen(out_path, "r");
  const bool fresh = previous == NULL;
  if (previous != NULL) {
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) > 0) {
      const int root_id = atoi(line);
      for (int idx = 0; idx < num_picks; idx++) {
        root_done[idx] |= pick_root[idx] == root_id;
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  char *worlds_path = get_formatted_string("%s.worlds.csv", prefix);
  FILE *worlds_out = fopen_or_die(worlds_path, "a");
  free(worlds_path);
  if (fresh) {
    fprintf(output, "root,pick,codes,tiles,worlds,weight,win,mean,capped,"
                    "unproven,static_worlds,actual_final,ms,move\n");
    fprintf(worlds_out,
            samples > 0 ? "root,pick,bag,weight,actual,final,proven\n"
                        : "root,pick,b0,b1,b2,weight,actual,final,proven\n");
  }
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  VntPegNet *net = vnt_peg_net_create(
      string_splitter_get_item(fields, 6), string_splitter_get_item(fields, 7),
      atoi(string_splitter_get_item(fields, 8)),
      strtod(string_splitter_get_item(fields, 9), NULL),
      strtod(string_splitter_get_item(fields, 10), NULL), root, threads);
  VntPeg2Chooser *chooser = vnt_peg2_chooser_create(
      string_splitter_get_item(fields, 11),
      string_splitter_get_item(fields, 12),
      atoi(string_splitter_get_item(fields, 13)),
      atoi(string_splitter_get_item(fields, 14)), root, threads);
  net->cache = true;
  chooser->cache = true;
  VntPeg3OracleWorker workers[VNT_PEG3_ORACLE_MAX_THREADS];
  pthread_t thread_ids[VNT_PEG3_ORACLE_MAX_THREADS];
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    workers[thread_idx] = (VntPeg3OracleWorker){
        .net = net,
        .chooser = chooser,
        .slot_idx = thread_idx,
        .game = game_duplicate(root),
        .static_list = move_list_create(1),
        .exact = vnt_peg1_exact_create(plies, tt_fraction),
    };
  }
  VntPeg3World *worlds =
      malloc_or_die(sizeof(VntPeg3World) * VNT_PEG3_ORACLE_MAX_WORLDS);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *wide = move_list_create(VNT_PEG2_WIDE_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  StringBuilder *name = string_builder_create();
  StringBuilder *codes = string_builder_create();
  ErrorStack *errors = error_stack_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id =
        vnt_peg2_next_root(positions, &line, &capacity, root, errors, bag_size,
                           &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    // The root's distinct picks, in pick-file order.
    int distinct[VNT_PEG3_ORACLE_MAX_PICKS];
    int n_distinct = 0;
    for (int idx = 0; idx < num_picks; idx++) {
      if (pick_root[idx] != root_id || root_done[idx]) {
        continue;
      }
      bool seen = false;
      for (int prev = 0; prev < n_distinct; prev++) {
        seen |= vnt_peg3_oracle_same_pick(pick_text[distinct[prev]],
                                          pick_code[distinct[prev]],
                                          pick_text[idx], pick_code[idx]);
      }
      if (!seen) {
        if (n_distinct == VNT_PEG3_ORACLE_MAX_PICKS) {
          log_fatal("peg3oracle too many distinct picks for root %d", root_id);
        }
        distinct[n_distinct++] = idx;
      }
    }
    if (n_distinct == 0) {
      continue;
    }
    int unseen[VALUE_NET_TILE_TYPES];
    if (vnt_peg1_public_unseen(root, unseen) != RACK_SIZE + bag_size) {
      log_fatal("peg3oracle root does not have %d unseen tiles",
                RACK_SIZE + bag_size);
    }
    vnt_peg_net_reset(net);
    vnt_peg2_chooser_reset(chooser);
    for (int pick_idx = 0; pick_idx < n_distinct; pick_idx++) {
      const char *text = pick_text[distinct[pick_idx]];
      const int code = pick_code[distinct[pick_idx]];
      Move pick;
      if (!vnt_peg3_oracle_find_move(root, text, code, wide, wide_moves, pool,
                                     name, &pick)) {
        log_fatal("peg3oracle root %d has no move %s (code %d)", root_id, text,
                  code);
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      const int n_worlds =
          samples > 0
              ? vnt_peg3_oracle_sample_worlds(root, unseen, bag_size, samples,
                                              sample_seed + (uint64_t)root_id,
                                              worlds)
              : vnt_peg3_oracle_worlds(root, unseen, bag_size,
                                       move_get_tiles_played(&pick) >= bag_size,
                                       worlds);
      atomic_int next;
      atomic_init(&next, 0);
      const int64_t start = ctimer_monotonic_ns();
      for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
        workers[thread_idx].root = root;
        workers[thread_idx].pick = &pick;
        workers[thread_idx].bag_size = bag_size;
        workers[thread_idx].worlds = worlds;
        workers[thread_idx].n_worlds = n_worlds;
        workers[thread_idx].next = &next;
        cpthread_create(&thread_ids[thread_idx], vnt_peg3_oracle_worker,
                        &workers[thread_idx]);
      }
      for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
        cpthread_join(thread_ids[thread_idx]);
      }
      const double ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
      int64_t weight_sum = 0;
      double win_sum = 0, spread_sum = 0, actual_final = NAN;
      int capped = 0, unproven = 0, static_worlds = 0;
      for (int world_idx = 0; world_idx < n_worlds; world_idx++) {
        const VntPeg3World *world = &worlds[world_idx];
        weight_sum += world->weight;
        win_sum += (double)world->weight * (world->final > 0   ? 1.0
                                            : world->final < 0 ? 0.0
                                                               : 0.5);
        spread_sum += (double)world->weight * world->final;
        capped += world->capped;
        unproven += !world->proven;
        static_worlds += world->static_turns > 0;
        if (world->actual) {
          actual_final = world->final;
        }
        if (samples > 0) {
          fprintf(worlds_out, "%d,%d,", root_id, pick_idx);
          for (int idx = 0; idx < bag_size; idx++) {
            fprintf(worlds_out, "%s%d", idx > 0 ? "." : "", world->bag[idx]);
          }
          fprintf(worlds_out, ",%lld,%d,%.9g,%d\n", (long long)world->weight,
                  world->actual, world->final, world->proven);
        } else {
          fprintf(worlds_out, "%d,%d,%d,%d,%d,%lld,%d,%.9g,%d\n", root_id,
                  pick_idx, world->bag[0], bag_size > 1 ? world->bag[1] : -1,
                  bag_size > 2 ? world->bag[2] : -1, (long long)world->weight,
                  world->actual, world->final, world->proven);
        }
      }
      string_builder_clear(codes);
      for (int idx = 0; idx < num_picks; idx++) {
        if (pick_root[idx] == root_id &&
            vnt_peg3_oracle_same_pick(pick_text[idx], pick_code[idx], text,
                                      code)) {
          string_builder_add_formatted_string(
              codes, "%s%d", string_builder_peek(codes)[0] ? ";" : "",
              pick_code[idx]);
        }
      }
      fprintf(output,
              "%d,%d,%s,%d,%d,%lld,%.9g,%.9g,%d,%d,%d,%.9g,%.9g,\"%s\"\n",
              root_id, pick_idx, string_builder_peek(codes),
              move_get_tiles_played(&pick), n_worlds, (long long)weight_sum,
              100.0 * win_sum / (double)weight_sum,
              spread_sum / (double)weight_sum, capped, unproven, static_worlds,
              actual_final, ms, string_builder_peek(name));
    }
    fflush(worlds_out);
    fflush(output);
    fsync(fileno(worlds_out));
    fsync(fileno(output));
  }
  free(line);
  fclose(positions);
  fclose(output);
  fclose(worlds_out);
  for (int thread_idx = 0; thread_idx < threads; thread_idx++) {
    game_destroy(workers[thread_idx].game);
    move_list_destroy(workers[thread_idx].static_list);
    vnt_peg1_exact_destroy(&workers[thread_idx].exact);
  }
  for (int idx = 0; idx < num_picks; idx++) {
    free(pick_text[idx]);
  }
  free(pick_text);
  free(pick_root);
  free(pick_code);
  free(root_done);
  free(worlds);
  free(wide_moves);
  move_list_destroy(pool);
  move_list_destroy(wide);
  string_builder_destroy(name);
  string_builder_destroy(codes);
  error_stack_destroy(errors);
  vnt_peg_net_destroy(net);
  vnt_peg2_chooser_destroy(chooser);
  config_destroy(config);
}

// ---- Late-game sims with the PEG choosers in the rollouts -------------------
// Per-worker fixed-depth endgame search for rollouts (the bag is empty, so
// both racks are known in the rollout's world): vnt_fixed_endgame's search
// with each worker's solver context kept between calls.
typedef struct VntPegSimEndgame {
  int plies;
  int workers;
  EndgameCtx **ctx;
  EndgameResults **results;
  ThreadControl **control;
  int64_t *calls;
  double *ms;
} VntPegSimEndgame;

static VntPegSimEndgame vnt_peg_sim_endgame_create(int plies, int workers) {
  VntPegSimEndgame endgame = {
      .plies = plies,
      .workers = workers,
      .ctx = calloc_or_die((size_t)workers, sizeof(EndgameCtx *)),
      .results = malloc_or_die(sizeof(EndgameResults *) * (size_t)workers),
      .control = malloc_or_die(sizeof(ThreadControl *) * (size_t)workers),
      .calls = calloc_or_die((size_t)workers, sizeof(int64_t)),
      .ms = calloc_or_die((size_t)workers, sizeof(double)),
  };
  for (int worker = 0; worker < workers; worker++) {
    endgame.results[worker] = endgame_results_create();
    endgame.control[worker] = thread_control_create();
  }
  return endgame;
}

static void vnt_peg_sim_endgame_destroy(VntPegSimEndgame *endgame) {
  for (int worker = 0; worker < endgame->workers; worker++) {
    if (endgame->ctx[worker] != NULL) {
      endgame_ctx_destroy(endgame->ctx[worker]);
    }
    endgame_results_destroy(endgame->results[worker]);
    thread_control_destroy(endgame->control[worker]);
  }
  free(endgame->ctx);
  free(endgame->results);
  free(endgame->control);
  free(endgame->calls);
  free(endgame->ms);
}

static void vnt_peg_sim_endgame_move(VntPegSimEndgame *endgame, int worker,
                                     const Game *game, Move *move) {
  const int64_t start = ctimer_monotonic_ns();
  EndgameArgs args = {
      .thread_control = endgame->control[worker],
      .game = game,
      .plies = endgame->plies,
      .tt_fraction_of_mem = 0,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .incremental_movegen = true,
      .enable_pv_display = true,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
  };
  EndgameResults *results = endgame->results[worker];
  endgame_solve_inline(&endgame->ctx[worker], &args, results);
  const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  if (endgame_results_get_status(results) != ENDGAME_RESULT_STATUS_FINISHED ||
      endgame_results_get_depth(results, ENDGAME_RESULT_BEST) !=
          endgame->plies ||
      pv->num_moves < 1) {
    log_fatal("pegsim rollout endgame search did not finish");
  }
  small_move_to_move(move, &pv->moves[0], game_get_board(game));
  endgame->calls[worker]++;
  endgame->ms[worker] += (double)(ctimer_monotonic_ns() - start) / 1e6;
}

// SimArgs.rollout_move_fn for pegsim. With nets, the 1PEG network chooses
// with one tile in the bag and the 2PEG chooser with two (both from the
// on-turn player's information: their rack, the board and the unseen counts);
// with an endgame search, every bag-empty ply is that search's move; every
// other ply is static. Moves are played in the rollout's own world, so the
// rollout's result is that world's.
typedef struct VntPegSimHook {
  bool nets;
  VntPegNet *net;
  VntPeg2Chooser *chooser;
  VntPegSimEndgame *endgame;
} VntPegSimHook;

static bool vnt_peg_sim_rollout_move(void *context, int worker_index,
                                     const Game *game, Move *move) {
  VntPegSimHook *hook = context;
  if (hook->endgame != NULL && !game_over(game) &&
      bag_get_letters(game_get_bag(game)) == 0) {
    vnt_peg_sim_endgame_move(hook->endgame, worker_index, game, move);
    return true;
  }
  return hook->nets &&
         (vnt_peg_net_pick(hook->net, worker_index, game, move) ||
          vnt_peg2_chooser_pick(hook->chooser, worker_index, game, move));
}

enum {
  VNT_PEG_SIM_ARMS = 6,
};

// Sums of the choosers' per-slot call counts and milliseconds.
static void vnt_peg_sim_chooser_totals(const VntPegNet *net,
                                       const VntPeg2Chooser *chooser,
                                       int64_t *calls1, double *ms1,
                                       int64_t *calls2, double *ms2) {
  *calls1 = *calls2 = 0;
  *ms1 = *ms2 = 0;
  for (int slot_idx = 0; slot_idx < net->slots; slot_idx++) {
    *calls1 += net->slot[slot_idx].calls;
    *ms1 += net->slot[slot_idx].ms;
  }
  for (int slot_idx = 0; slot_idx < chooser->slots; slot_idx++) {
    *calls2 += chooser->slot[slot_idx].calls;
    *ms2 += chooser->slot[slot_idx].ms;
  }
}

// Late-game sims (VNT_SIM_BAG tiles in the bag at each root, default 7). Each
// root's static top-N tile placements (no exchanges, no pass) are simmed with
// plies-ply rollouts by round robin, with the same seed in every arm
// (VNT_SIM_SEED + root, so iteration k of a candidate is the same world in
// every arm), no inference, no bag cycling, rollout PAT off and utility
// (1, 0.5, 100). Arms (VNT_SIM_ARMS, default "0,1,2"): 0 static rollouts,
// iterations per candidate; 1 the same with the nets at 1 and 2 tiles in the
// bag (vnt_peg_sim_rollout_move); 2 static rollouts for arm 1's wall time on
// this root, no iteration cap (needs arm 1); 3 arm 1 with every bag-empty ply
// chosen by a VNT_SIM_EG_PLIES-ply endgame search (default 2); 4 static
// rollouts with that endgame search; 5 static rollouts for arm 3's wall time
// (needs arm 3). The choosers' pick caches are cleared before every arm, so
// each arm's time includes its own chooser work. VNT_SIM_THREADS threads
// (default 1). VNT_SIM_CHECK=1 turns on the choosers' hidden-tile checks (each
// rollout decision recomputed with a hidden tile swapped; fatal on a
// difference). Fields: positions prefix, output prefix, candidates, iterations
// per candidate, plies, 1PEG first, second|-, shortlist, w_spread, scale, 2PEG
// model, 2PEG first|-, stage N, top K. Writes PREFIX.sim.csv (per root and arm:
// the chosen candidate's static rank and its utility, win% and equity means,
// the sim's iterations and ms, the choosers' calls and ms in this sim; quoted
// move) and PREFIX.cands.csv (every candidate's samples and means per arm).
// Resumes by root.
static void vnt_peg_sim(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 15) {
    log_fatal("pegsim needs positions prefix, output prefix, candidates, "
              "iterations, plies, first, second|-, shortlist, w_spread, "
              "scale, 2PEG model, 2PEG first|-, stage N, top K");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  const int max_candidates = atoi(string_splitter_get_item(fields, 3));
  const int iterations = atoi(string_splitter_get_item(fields, 4));
  const int plies = atoi(string_splitter_get_item(fields, 5));
  const char *bag_env = getenv("VNT_SIM_BAG");
  const int bag_size = bag_env != NULL ? atoi(bag_env) : RACK_SIZE;
  const char *threads_env = getenv("VNT_SIM_THREADS");
  const int threads =
      threads_env != NULL && atoi(threads_env) > 1 ? atoi(threads_env) : 1;
  const char *seed_env = getenv("VNT_SIM_SEED");
  const uint64_t base_seed =
      seed_env != NULL ? strtoull(seed_env, NULL, 10) : 0;
  const char *arms_env = getenv("VNT_SIM_ARMS");
  bool run_arm[VNT_PEG_SIM_ARMS] = {false};
  const char *arms_text = arms_env != NULL ? arms_env : "0,1,2";
  for (const char *cursor = arms_text; *cursor != '\0'; cursor++) {
    if (*cursor >= '0' && *cursor < '0' + VNT_PEG_SIM_ARMS) {
      run_arm[*cursor - '0'] = true;
    }
  }
  const char *eg_env = getenv("VNT_SIM_EG_PLIES");
  const int eg_plies = eg_env != NULL ? atoi(eg_env) : 2;
  if (max_candidates < 1 || iterations < 1 || plies < 1 || plies > MAX_PLIES ||
      bag_size < 1 || bag_size > RACK_SIZE || (run_arm[2] && !run_arm[1]) ||
      (run_arm[5] && !run_arm[3]) || eg_plies < 1 || eg_plies > MAX_PLIES) {
    log_fatal("pegsim invalid candidates, iterations, plies, bag, arms or "
              "endgame plies");
  }
  // Roots already in the output are done.
  char *out_path = get_formatted_string("%s.sim.csv", prefix);
  int done_capacity = 64, num_done = 0;
  int *done = malloc_or_die(sizeof(int) * (size_t)done_capacity);
  char *line = NULL;
  size_t capacity = 0;
  FILE *previous = fopen(out_path, "r");
  const bool fresh = previous == NULL;
  if (previous != NULL) {
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) > 0) {
      if (num_done == done_capacity) {
        done_capacity *= 2;
        done = realloc_or_die(done, sizeof(int) * (size_t)done_capacity);
      }
      done[num_done++] = atoi(line);
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  char *cands_path = get_formatted_string("%s.cands.csv", prefix);
  FILE *cands_out = fopen_or_die(cands_path, "a");
  free(cands_path);
  if (fresh) {
    fprintf(output, "root,arm,rank,candidates,utility,win,equity,iterations,"
                    "ms,net1_calls,net1_ms,net2_calls,net2_ms,eg_calls,eg_ms,"
                    "move\n");
    fprintf(cands_out, "root,arm,rank,samples,utility,win,equity\n");
  }
  Config *config = vnt_peg2_config();
  ErrorStack *errors = error_stack_create();
  config_load_win_pcts(config, errors);
  if (!error_stack_is_empty(errors)) {
    error_stack_print_and_reset(errors);
    log_fatal("pegsim could not load the win percentages");
  }
  Game *root = config_get_game(config);
  VntPegNet *net = vnt_peg_net_create(
      string_splitter_get_item(fields, 6), string_splitter_get_item(fields, 7),
      atoi(string_splitter_get_item(fields, 8)),
      strtod(string_splitter_get_item(fields, 9), NULL),
      strtod(string_splitter_get_item(fields, 10), NULL), root, threads);
  VntPeg2Chooser *chooser = vnt_peg2_chooser_create(
      string_splitter_get_item(fields, 11),
      string_splitter_get_item(fields, 12),
      atoi(string_splitter_get_item(fields, 13)),
      atoi(string_splitter_get_item(fields, 14)), root, threads);
  net->cache = true;
  chooser->cache = true;
  const char *check_env = getenv("VNT_SIM_CHECK");
  net->check = check_env != NULL && atoi(check_env) != 0;
  chooser->check = net->check;
  VntPegSimEndgame endgame = vnt_peg_sim_endgame_create(eg_plies, threads);
  VntPegSimHook hooks[VNT_PEG_SIM_ARMS] = {
      {.nets = false},
      {.nets = true, .net = net, .chooser = chooser},
      {.nets = false},
      {.nets = true, .net = net, .chooser = chooser, .endgame = &endgame},
      {.nets = false, .endgame = &endgame},
      {.nets = false},
  };
  MoveList *wide = move_list_create(VNT_PEG2_WIDE_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  MoveList *pool = move_list_create(max_candidates);
  SimResults *results = sim_results_create(0.0);
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id =
        vnt_peg2_next_root(positions, &line, &capacity, root, errors, bag_size,
                           &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    bool skip = false;
    for (int idx = 0; idx < num_done; idx++) {
      skip |= done[idx] == root_id;
    }
    if (skip) {
      continue;
    }
    const int n_placements = vnt_peg2_wide_pool(root, wide, wide_moves);
    move_list_reset(pool);
    for (int rank = 0; rank < n_placements && rank < max_candidates; rank++) {
      move_list_add_move(pool, &wide_moves[rank]);
    }
    const int cands = move_list_get_count(pool);
    if (cands == 0) {
      log_fatal("pegsim root %d has no placement", root_id);
    }
    double arm_seconds[VNT_PEG_SIM_ARMS] = {0};
    for (int arm = 0; arm < VNT_PEG_SIM_ARMS; arm++) {
      if (!run_arm[arm]) {
        continue;
      }
      vnt_peg_net_reset(net);
      vnt_peg2_chooser_reset(chooser);
      const bool timed = arm == 2 || arm == 5;
      const double time_limit = arm == 2   ? arm_seconds[1]
                                : arm == 5 ? arm_seconds[3]
                                           : 0.0;
      int64_t calls1_before, calls2_before, calls1_after, calls2_after;
      double ms1_before, ms2_before, ms1_after, ms2_after;
      vnt_peg_sim_chooser_totals(net, chooser, &calls1_before, &ms1_before,
                                 &calls2_before, &ms2_before);
      ThreadControl *control = thread_control_create();
      thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
      SimArgs sim_args;
      const uint64_t max_iterations =
          timed ? UINT64_C(1000000000000000)
                : (uint64_t)iterations * (uint64_t)cands;
      sim_args_fill(plies, pool, cands, NULL, config_get_win_pcts(config), NULL,
                    control, root, false, false, threads, 0, cands, plies,
                    base_seed + (uint64_t)root_id, max_iterations, 1, 0.0,
                    BAI_THRESHOLD_NONE, time_limit,
                    BAI_SAMPLING_RULE_ROUND_ROBIN, 0.0, 1.0, 0.5, 100.0, false,
                    NULL, &sim_args);
      sim_args.bag_cycle = false;
      sim_args.pat_rollout_disabled = true;
      if (hooks[arm].nets || hooks[arm].endgame != NULL) {
        sim_args.rollout_move_fn = vnt_peg_sim_rollout_move;
        sim_args.rollout_move_context = &hooks[arm];
      }
      int64_t eg_calls_before = 0;
      double eg_ms_before = 0;
      for (int worker = 0; worker < threads; worker++) {
        eg_calls_before += endgame.calls[worker];
        eg_ms_before += endgame.ms[worker];
      }
      const int64_t start = ctimer_monotonic_ns();
      if (cands > 1) {
        simulate_without_ctx(&sim_args, results, errors);
      }
      const double seconds = (double)(ctimer_monotonic_ns() - start) / 1e9;
      if (!error_stack_is_empty(errors)) {
        error_stack_print_and_reset(errors);
        log_fatal("pegsim sim failed on root %d", root_id);
      }
      thread_control_destroy(control);
      arm_seconds[arm] = seconds;
      vnt_peg_sim_chooser_totals(net, chooser, &calls1_after, &ms1_after,
                                 &calls2_after, &ms2_after);
      int64_t eg_calls = -eg_calls_before;
      double eg_ms = -eg_ms_before;
      for (int worker = 0; worker < threads; worker++) {
        eg_calls += endgame.calls[worker];
        eg_ms += endgame.ms[worker];
      }
      // Each simmed play's static rank, by its text.
      int best_rank = 0;
      double best_utility = NAN, best_win = NAN, best_equity = NAN;
      const int best_index =
          cands > 1 ? sim_results_get_best_move_index(results) : -1;
      for (int play_idx = 0; play_idx < (cands > 1 ? cands : 0); play_idx++) {
        const SimmedPlay *simmed_play =
            sim_results_get_simmed_play(results, play_idx);
        string_builder_clear(name);
        string_builder_add_move(name, game_get_board(root),
                                simmed_play_get_move(simmed_play),
                                game_get_ld(root), false);
        int rank = -1;
        StringBuilder *other = string_builder_create();
        for (int idx = 0; idx < cands && rank < 0; idx++) {
          string_builder_clear(other);
          string_builder_add_move(other, game_get_board(root),
                                  move_list_get_move(pool, idx),
                                  game_get_ld(root), false);
          if (strings_equal(string_builder_peek(name),
                            string_builder_peek(other))) {
            rank = idx;
          }
        }
        string_builder_destroy(other);
        const Stat *utility = simmed_play_get_utility_stat(simmed_play);
        const double win =
            stat_get_mean(simmed_play_get_win_pct_stat(simmed_play));
        const double equity =
            stat_get_mean(simmed_play_get_equity_stat(simmed_play));
        fprintf(cands_out, "%d,%d,%d,%llu,%.9g,%.9g,%.9g\n", root_id, arm, rank,
                (unsigned long long)stat_get_num_samples(utility),
                stat_get_mean(utility), win, equity);
        if (play_idx == best_index) {
          best_rank = rank;
          best_utility = stat_get_mean(utility);
          best_win = win;
          best_equity = equity;
        }
      }
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root),
                              move_list_get_move(pool, best_rank),
                              game_get_ld(root), false);
      fprintf(
          output,
          "%d,%d,%d,%d,%.9g,%.9g,%.9g,%llu,%.9g,%lld,%.9g,%lld,%.9g,%lld,"
          "%.9g,\"%s\"\n",
          root_id, arm, best_rank, cands, best_utility, best_win, best_equity,
          (unsigned long long)(cands > 1
                                   ? sim_results_get_iteration_count(results)
                                   : 0),
          1000.0 * seconds, (long long)(calls1_after - calls1_before),
          ms1_after - ms1_before, (long long)(calls2_after - calls2_before),
          ms2_after - ms2_before, (long long)eg_calls, eg_ms,
          string_builder_peek(name));
    }
    fflush(cands_out);
    fflush(output);
    fsync(fileno(cands_out));
    fsync(fileno(output));
  }
  free(line);
  free(done);
  fclose(positions);
  fclose(output);
  fclose(cands_out);
  string_builder_destroy(name);
  sim_results_destroy(results);
  move_list_destroy(pool);
  move_list_destroy(wide);
  free(wide_moves);
  error_stack_destroy(errors);
  vnt_peg_sim_endgame_destroy(&endgame);
  vnt_peg_net_destroy(net);
  vnt_peg2_chooser_destroy(chooser);
  config_destroy(config);
}

// pegbudget arms (codes in PREFIX.pegb.csv; b = the budget's index).
enum {
  VNT_PB_MAX_BUDGETS = 8,
  VNT_PB_ARM_NET = 0,         // the 2PEG chooser's pick, no search
  VNT_PB_ARM_PEG = 100,       // default PEG (no pass), budget b
  VNT_PB_ARM_PEG_NET = 200,   // PEG with the 1PEG net nested, budget b
  VNT_PB_ARM_NET_ORDER = 300, // PEG with the net nested, over the chooser's
                              // top K in its order, budget b
  VNT_PB_ARM_HYBRID = 400,    // the chooser's top K in its order, then the
                              // rest of the placements by PEG's greedy stage
                              // 0, first stage VNT_PB_HYBRID_WIDTH wide
  VNT_PB_ARM_HYBRID_UTILITY = 500, // hybrid ranked by utility, no greedy tail
                                   // below VNT_PB_HYBRID_GREEDY_MIN_MS
  VNT_PB_ORDER_K = 16,
  VNT_PB_HYBRID_WIDTH = 48,
  VNT_PB_HYBRID_GREEDY_MIN_MS = 500,
};

// Per-solve progress for pegbudget: first-stage completions (live mode) and
// when the first halving stage began.
typedef struct VntPegBudgetProgress {
  int stage1_done;
  int64_t start_ns;
  int64_t stage1_ns;
} VntPegBudgetProgress;

static void vnt_peg_budget_stage_start(int stage_idx, int k_cands, int inner_d,
                                       int emptier_plies, void *user_data) {
  (void)k_cands;
  (void)inner_d;
  (void)emptier_plies;
  VntPegBudgetProgress *progress = user_data;
  if (stage_idx == 1 && progress->stage1_ns == 0) {
    progress->stage1_ns = ctimer_monotonic_ns();
  }
}

// Live-mode completions of the first halving stage (PegArgs.on_cand_done).
static void vnt_peg_budget_cand_done(int stage_idx, int cand_rank,
                                     const Move *cand, double win_pct,
                                     double mean_spread, int scen_done,
                                     int64_t completed_ns, bool reordered,
                                     void *user_data) {
  (void)cand_rank;
  (void)cand;
  (void)win_pct;
  (void)mean_spread;
  (void)scen_done;
  (void)completed_ns;
  (void)reordered;
  if (stage_idx == 1) {
    ((VntPegBudgetProgress *)user_data)->stage1_done++;
  }
}

// One budgeted root PEG pick: nested 1PEG chooser (NULL = the staged inner
// peg) over only (n_only moves), schedule top_k (NULL = PEG's default),
// ranked by utility or by win% first. With n_ranked > 0 the first n_ranked
// keep their order ahead of the rest and the halving stages run live (one
// candidate at a time). Writes the pick, wall ms,
// the last completed stage, whether it was partial, how many candidates the
// first halving stage finished (live only) and when it began (ms after the
// start; -1 = never).
static void vnt_peg_budget_pick(
    const Game *game, int threads, PegPeg1Chooser chooser, void *chooser_ctx,
    const Move *const *only, int n_only, int n_ranked, const int *top_k,
    int n_top_k, bool rank_by_utility, double budget_s, Move *best, double *ms,
    int *stage, bool *partial, int *stage1_done, double *stage1_start_ms) {
  static const int caps[] = {8, 4, 2};
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  PegPoll *poll = n_ranked > 0 ? peg_poll_create() : NULL;
  VntPegBudgetProgress progress = {0};
  PegArgs args;
  peg_args_fill(game, control, threads, budget_s, /*max_stage=*/0,
                /*greedy_seed_only=*/false, top_k, top_k != NULL ? n_top_k : 0,
                /*inner_top_k=*/0, PEG_OPP_RATIONAL, /*scenario_stride=*/0,
                /*nested_enabled=*/true, /*nested_cand_cap=*/0, caps,
                (int)(sizeof(caps) / sizeof(caps[0])), /*nested_stride=*/0,
                /*nested_strides_by_bag=*/NULL, /*nested_n_strides_by_bag=*/0,
                /*nested_emptier_ply_cap=*/0, PEG_NESTED_DEFAULT_DEPTH,
                vnt_best_reply_disabled(), chooser, chooser_ctx, NULL, NULL,
                /*eval_bag_order=*/NULL, /*eval_bag_order_len=*/0, only, n_only,
                n_ranked, rank_by_utility, /*protect_moves=*/NULL,
                /*n_protect_moves=*/0,
                /*include_per_scenario=*/false, vnt_peg_budget_stage_start,
                n_ranked > 0 ? vnt_peg_budget_cand_done : NULL,
                /*on_scenario_done=*/NULL, &progress, poll,
                /*shared_endgame_tt=*/NULL,
                /*checkpoint=*/NULL, /*resume=*/false, &args);
  PegResult result;
  ErrorStack *errors = error_stack_create();
  progress.start_ns = ctimer_monotonic_ns();
  peg_solve(&args, &result, errors);
  *ms = (double)(ctimer_monotonic_ns() - progress.start_ns) / 1e6;
  if (!error_stack_is_empty(errors) || result.n_top_cands < 1) {
    error_stack_print_and_reset(errors);
    log_fatal("pegbudget peg_solve failed");
  }
  move_copy(best, &result.best_move);
  *stage = result.last_completed_stage;
  *partial = result.last_stage_partial;
  *stage1_done = progress.stage1_done;
  *stage1_start_ms =
      progress.stage1_ns != 0
          ? (double)(progress.stage1_ns - progress.start_ns) / 1e6
          : -1;
  peg_result_destroy(&result);
  error_stack_destroy(errors);
  if (poll != NULL) {
    peg_poll_destroy(poll);
  }
  thread_control_destroy(control);
}

// The 2PEG chooser's ranking of root's static top 64 (its staged best N by
// the full model, then the rest by the first model): writes up to max moves
// in order into order[], returns how many, and the chooser's pick rank.
static int vnt_peg_budget_rank(VntPeg2Chooser *chooser, const Game *root,
                               Move *candidates, const Move **order, int max,
                               int *pick_rank) {
  VntPeg2ChooserSlot *slot = &chooser->slot[0];
  VntPeg2PickTiming timing = {0};
  int count, scored;
  float scores[VNT_PEG1_POOL], first_scores[VNT_PEG1_POOL];
  *pick_rank = vnt_peg2_cheap_pick(root, chooser->model, chooser->first,
                                   chooser->stage_n, chooser->top_k,
                                   &slot->work, candidates, &count, &scored,
                                   scores, first_scores, &timing, NULL);
  int idx[VNT_PEG1_POOL];
  double key[VNT_PEG1_POOL];
  for (int rank = 0; rank < count; rank++) {
    idx[rank] = rank;
    // Fully scored ranks first (by the full model), then by the first model.
    key[rank] = !isnan(scores[rank])     ? 1e9 + scores[rank]
                : chooser->first != NULL ? (double)first_scores[rank]
                                         : -1e18;
  }
  for (int left = 1; left < count; left++) {
    const int cur = idx[left];
    int pos = left;
    while (pos > 0 && (key[idx[pos - 1]] < key[cur] ||
                       (key[idx[pos - 1]] == key[cur] && idx[pos - 1] > cur))) {
      idx[pos] = idx[pos - 1];
      pos--;
    }
    idx[pos] = cur;
  }
  const int n = count < max ? count : max;
  for (int pos = 0; pos < n; pos++) {
    order[pos] = &candidates[idx[pos]];
  }
  if (*pick_rank >= 0 && n > 0 && order[0] != &candidates[*pick_rank]) {
    log_fatal("pegbudget ranking disagrees with the chooser's pick");
  }
  return n;
}

// Matched-budget 2PEG picks (pegbudget/PROTOCOL-ENGINE.txt). Fields:
// positions prefix (2 tiles in the bag), output prefix, budgets in seconds
// (comma-separated), 1PEG first, second|-, shortlist, w_spread, scale, 2PEG
// model, 2PEG first|-, stage N, top K. VNT_PB_THREADS (default 10) threads
// per PEG solve; VNT_PB_ARMS a subset of
// "net,peg,pegnet,order,hybrid,hybridu" (default "net,peg,pegnet,order").
// Writes PREFIX.pegb.csv: root, code, arm, budget_s, ms, net_ms, stage,
// partial, stage1_done, s1_ms (when the first halving stage began, -1 = never),
// quoted move (a peg3oracle picks file). Roots already in the output are
// skipped.
static void vnt_peg_budget(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 13) {
    log_fatal("pegbudget needs positions prefix, output prefix, budgets, "
              "first, second|-, shortlist, w_spread, scale, 2PEG model, 2PEG "
              "first|-, stage N, top K");
  }
  const char *data = string_splitter_get_item(fields, 1);
  const char *prefix = string_splitter_get_item(fields, 2);
  double budgets[VNT_PB_MAX_BUDGETS];
  int n_budgets = 0;
  StringSplitter *budget_items =
      split_string(string_splitter_get_item(fields, 3), ',', true);
  for (int idx = 0; idx < string_splitter_get_number_of_items(budget_items);
       idx++) {
    if (n_budgets == VNT_PB_MAX_BUDGETS) {
      log_fatal("pegbudget takes at most %d budgets", VNT_PB_MAX_BUDGETS);
    }
    budgets[n_budgets] =
        strtod(string_splitter_get_item(budget_items, idx), NULL);
    if (budgets[n_budgets] <= 0) {
      log_fatal("pegbudget budgets must be positive");
    }
    n_budgets++;
  }
  string_splitter_destroy(budget_items);
  const char *threads_env = getenv("VNT_PB_THREADS");
  const int threads =
      threads_env != NULL && atoi(threads_env) > 0 ? atoi(threads_env) : 10;
  const char *arms_env = getenv("VNT_PB_ARMS");
  StringSplitter *arm_items = split_string(
      arms_env != NULL ? arms_env : "net,peg,pegnet,order", ',', true);
  bool arm_net = false, arm_peg = false, arm_peg_net = false, arm_order = false,
       arm_hybrid = false, arm_hybrid_utility = false;
  for (int idx = 0; idx < string_splitter_get_number_of_items(arm_items);
       idx++) {
    const char *item = string_splitter_get_item(arm_items, idx);
    arm_net |= strings_equal(item, "net");
    arm_peg |= strings_equal(item, "peg");
    arm_peg_net |= strings_equal(item, "pegnet");
    arm_order |= strings_equal(item, "order");
    arm_hybrid |= strings_equal(item, "hybrid");
    arm_hybrid_utility |= strings_equal(item, "hybridu");
  }
  string_splitter_destroy(arm_items);
  char *out_path = get_formatted_string("%s.pegb.csv", prefix);
  char *line = NULL;
  size_t capacity = 0;
  int done_capacity = 64, n_done = 0;
  int *done_roots = malloc_or_die(sizeof(int) * (size_t)done_capacity);
  FILE *previous = fopen(out_path, "r");
  bool fresh = previous == NULL;
  if (previous != NULL) {
    fresh = fgetc(previous) == EOF;
    rewind(previous);
    (void)getline_ignore_carriage_return(&line, &capacity, previous);
    while (getline_ignore_carriage_return(&line, &capacity, previous) >= 0) {
      int root_id;
      if (sscanf(line, "%d,", &root_id) == 1 &&
          (n_done == 0 || done_roots[n_done - 1] != root_id)) {
        if (n_done == done_capacity) {
          done_capacity *= 2;
          done_roots =
              realloc_or_die(done_roots, sizeof(int) * (size_t)done_capacity);
        }
        done_roots[n_done++] = root_id;
      }
    }
    fclose(previous);
  }
  FILE *output = fopen_or_die(out_path, "a");
  free(out_path);
  if (fresh) {
    fprintf(output, "root,code,arm,budget_s,ms,net_ms,stage,partial,"
                    "stage1_done,s1_ms,move\n");
  }
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  VntPegNet *net = vnt_peg_net_create(
      string_splitter_get_item(fields, 4), string_splitter_get_item(fields, 5),
      atoi(string_splitter_get_item(fields, 6)),
      strtod(string_splitter_get_item(fields, 7), NULL),
      strtod(string_splitter_get_item(fields, 8), NULL), root, threads + 1);
  VntPeg2Chooser *chooser = vnt_peg2_chooser_create(
      string_splitter_get_item(fields, 9), string_splitter_get_item(fields, 10),
      atoi(string_splitter_get_item(fields, 11)),
      atoi(string_splitter_get_item(fields, 12)), root, 1);
  net->cache = true;
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *wide = move_list_create(VNT_PEG2_WIDE_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  const Move **placements = malloc_or_die(sizeof(Move *) * VNT_PEG2_WIDE_CAP);
  const Move **hybrid = malloc_or_die(sizeof(Move *) * VNT_PEG2_WIDE_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv", data);
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = vnt_peg2_next_root(positions, &line, &capacity, root,
                                           errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    bool skip = false;
    for (int idx = 0; idx < n_done && !skip; idx++) {
      skip = done_roots[idx] == root_id;
    }
    if (skip) {
      continue;
    }
    int unseen[VALUE_NET_TILE_TYPES];
    if (vnt_peg1_public_unseen(root, unseen) != VNT_PEG2_UNSEEN) {
      log_fatal("pegbudget root does not have nine unseen tiles");
    }
    Move candidates[VNT_PEG1_POOL];
    int pass_rank;
    const int count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("pegbudget candidate replay mismatch");
    }
    const int n_placements = vnt_peg2_wide_pool(root, wide, wide_moves);
    for (int rank = 0; rank < n_placements; rank++) {
      placements[rank] = &wide_moves[rank];
    }
    // Each row: code, arm name, budget, ms, net ms, stage, partial, done, pick.
    Move pick;
    if (arm_net) {
      const Move *order[VNT_PEG1_POOL];
      int pick_rank;
      const int64_t start = ctimer_monotonic_ns();
      vnt_peg_budget_rank(chooser, root, candidates, order, 1, &pick_rank);
      const double net_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
      move_copy(&pick, order[0]);
      string_builder_clear(name);
      string_builder_add_move(name, game_get_board(root), &pick,
                              game_get_ld(root), false);
      fprintf(output, "%d,%d,net,0,%.3f,%.3f,0,0,0,-1,\"%s\"\n", root_id,
              VNT_PB_ARM_NET, net_ms, net_ms, string_builder_peek(name));
    }
    static const int order_top_k[] = {VNT_PB_ORDER_K, 8, 4, 2};
    static const int hybrid_top_k[] = {VNT_PB_HYBRID_WIDTH, 16, 8, 4, 2};
    for (int bud = 0; bud < n_budgets; bud++) {
      for (int arm = 0; arm < 5; arm++) {
        if ((arm == 0 && !arm_peg) || (arm == 1 && !arm_peg_net) ||
            (arm == 2 && !arm_order) || (arm == 3 && !arm_hybrid) ||
            (arm == 4 && !arm_hybrid_utility)) {
          continue;
        }
        double ms = 0, net_ms = 0, stage1_start_ms = -1;
        int stage = 0, stage1_done = 0;
        bool partial = false;
        vnt_peg_net_reset(net);
        if (arm >= 2) {
          const Move *order[VNT_PEG1_POOL];
          int pick_rank;
          const int64_t start = ctimer_monotonic_ns();
          const int n_order = vnt_peg_budget_rank(
              chooser, root, candidates, order, VNT_PB_ORDER_K, &pick_rank);
          // hybrid: the chooser's top K, then every other placement (hybridu:
          // only when the budget reaches VNT_PB_HYBRID_GREEDY_MIN_MS).
          int n_only = n_order;
          for (int rank = 0; rank < n_order; rank++) {
            hybrid[rank] = order[rank];
          }
          const bool greedy_tail =
              arm == 3 ||
              (arm == 4 && budgets[bud] * 1000 >= VNT_PB_HYBRID_GREEDY_MIN_MS);
          if (greedy_tail) {
            for (int rank = 0; rank < n_placements; rank++) {
              bool in_order = false;
              for (int pos = 0; pos < n_order && !in_order; pos++) {
                in_order = compare_moves_without_equity(placements[rank],
                                                        order[pos], true) == -1;
              }
              if (!in_order) {
                hybrid[n_only++] = placements[rank];
              }
            }
          }
          net_ms = (double)(ctimer_monotonic_ns() - start) / 1e6;
          const double left = budgets[bud] - net_ms / 1e3;
          if (n_only < 2 || left <= 0) {
            move_copy(&pick, order[0]);
          } else {
            vnt_peg_budget_pick(
                root, threads, vnt_peg_net_chooser, net, hybrid, n_only,
                n_order, greedy_tail ? hybrid_top_k : order_top_k,
                greedy_tail ? (int)(sizeof(hybrid_top_k) / sizeof(int))
                            : (int)(sizeof(order_top_k) / sizeof(int)),
                arm == 4, left, &pick, &ms, &stage, &partial, &stage1_done,
                &stage1_start_ms);
            if (stage1_start_ms >= 0) {
              stage1_start_ms += net_ms;
            }
          }
          ms += net_ms;
        } else {
          vnt_peg_budget_pick(
              root, threads, arm == 1 ? vnt_peg_net_chooser : NULL,
              arm == 1 ? net : NULL, placements, n_placements,
              /*n_ranked=*/0, /*top_k=*/NULL, /*n_top_k=*/0,
              /*rank_by_utility=*/false, budgets[bud], &pick, &ms, &stage,
              &partial, &stage1_done, &stage1_start_ms);
        }
        static const char *const arm_names[] = {"peg", "pegnet", "order",
                                                "hybrid", "hybridu"};
        static const int arm_codes[] = {VNT_PB_ARM_PEG, VNT_PB_ARM_PEG_NET,
                                        VNT_PB_ARM_NET_ORDER, VNT_PB_ARM_HYBRID,
                                        VNT_PB_ARM_HYBRID_UTILITY};
        string_builder_clear(name);
        string_builder_add_move(name, game_get_board(root), &pick,
                                game_get_ld(root), false);
        fprintf(output, "%d,%d,%s,%g,%.3f,%.3f,%d,%d,%d,%.3f,\"%s\"\n", root_id,
                arm_codes[arm] + bud, arm_names[arm], budgets[bud], ms, net_ms,
                stage, partial, stage1_done, stage1_start_ms,
                string_builder_peek(name));
      }
    }
    fflush(output);
  }
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  free(hybrid);
  free(placements);
  free(wide_moves);
  move_list_destroy(wide);
  move_list_destroy(pool);
  vnt_peg2_chooser_destroy(chooser);
  vnt_peg_net_destroy(net);
  config_destroy(config);
  free(done_roots);
  free(line);
}

// Where each pick of a pegbudget run falls in the candidate orders. Fields:
// positions prefix, pegb csv, output path, 2PEG model, 2PEG first|-, stage N,
// top K. Writes root, code, net_rank (the 2PEG chooser's order over the
// static top 64; -1 = outside it) and equity_rank (every placement in equity
// order; -1 = not found), both 0-based.
static void vnt_peg_rank(const StringSplitter *fields) {
  if (string_splitter_get_number_of_items(fields) != 8) {
    log_fatal("pegrank needs positions prefix, pegb csv, output, 2PEG model, "
              "2PEG first|-, stage N, top K");
  }
  int num_picks = 0, pick_capacity = 64;
  int *pick_root = malloc_or_die(sizeof(int) * (size_t)pick_capacity);
  int *pick_code = malloc_or_die(sizeof(int) * (size_t)pick_capacity);
  char **pick_text = malloc_or_die(sizeof(char *) * (size_t)pick_capacity);
  char *line = NULL;
  size_t capacity = 0;
  FILE *picks = fopen_or_die(string_splitter_get_item(fields, 2), "r");
  (void)getline_ignore_carriage_return(&line, &capacity, picks);
  while (getline_ignore_carriage_return(&line, &capacity, picks) > 0) {
    int root_id, code;
    char *open = strchr(line, '"'), *close = strrchr(line, '"');
    if (sscanf(line, "%d,%d,", &root_id, &code) != 2 || open == NULL ||
        close <= open) {
      log_fatal("pegrank invalid pick row");
    }
    *close = '\0';
    if (num_picks == pick_capacity) {
      pick_capacity *= 2;
      pick_root =
          realloc_or_die(pick_root, sizeof(int) * (size_t)pick_capacity);
      pick_code =
          realloc_or_die(pick_code, sizeof(int) * (size_t)pick_capacity);
      pick_text =
          realloc_or_die(pick_text, sizeof(char *) * (size_t)pick_capacity);
    }
    pick_root[num_picks] = root_id;
    pick_code[num_picks] = code;
    pick_text[num_picks] = string_duplicate(open + 1);
    num_picks++;
  }
  fclose(picks);
  FILE *output = vnt_fopen_new_or_die(string_splitter_get_item(fields, 3), "w");
  fprintf(output, "root,code,net_rank,equity_rank\n");
  Config *config = vnt_peg2_config();
  Game *root = config_get_game(config);
  VntPeg2Chooser *chooser = vnt_peg2_chooser_create(
      string_splitter_get_item(fields, 4), string_splitter_get_item(fields, 5),
      atoi(string_splitter_get_item(fields, 6)),
      atoi(string_splitter_get_item(fields, 7)), root, 1);
  MoveList *pool = move_list_create(VNT_PEG1_POOL + 1);
  MoveList *wide = move_list_create(VNT_PEG2_WIDE_CAP);
  Move *wide_moves = malloc_or_die(sizeof(Move) * VNT_PEG2_WIDE_CAP);
  ErrorStack *errors = error_stack_create();
  StringBuilder *name = string_builder_create();
  char *path = get_formatted_string("%s.positions.csv",
                                    string_splitter_get_item(fields, 1));
  FILE *positions = fopen_or_die(path, "r");
  free(path);
  (void)getline_ignore_carriage_return(&line, &capacity, positions);
  while (true) {
    int expected;
    uint64_t source_seed;
    const int root_id = vnt_peg2_next_root(positions, &line, &capacity, root,
                                           errors, 2, &expected, &source_seed);
    if (root_id < 0) {
      break;
    }
    Move candidates[VNT_PEG1_POOL];
    int pass_rank;
    const int count = vnt_peg1_pool(root, pool, candidates, &pass_rank);
    if (count != expected) {
      log_fatal("pegrank candidate replay mismatch");
    }
    const Move *order[VNT_PEG1_POOL];
    int pick_rank;
    const int n_order = vnt_peg_budget_rank(chooser, root, candidates, order,
                                            VNT_PEG1_POOL, &pick_rank);
    const int n_wide = vnt_peg2_wide_pool(root, wide, wide_moves);
    for (int pick_idx = 0; pick_idx < num_picks; pick_idx++) {
      if (pick_root[pick_idx] != root_id) {
        continue;
      }
      int net_rank = -1, equity_rank = -1;
      for (int pos = 0; pos < n_order && net_rank < 0; pos++) {
        string_builder_clear(name);
        string_builder_add_move(name, game_get_board(root), order[pos],
                                game_get_ld(root), false);
        if (strings_equal(string_builder_peek(name), pick_text[pick_idx])) {
          net_rank = pos;
        }
      }
      for (int rank = 0; rank < n_wide && equity_rank < 0; rank++) {
        string_builder_clear(name);
        string_builder_add_move(name, game_get_board(root), &wide_moves[rank],
                                game_get_ld(root), false);
        if (strings_equal(string_builder_peek(name), pick_text[pick_idx])) {
          equity_rank = rank;
        }
      }
      fprintf(output, "%d,%d,%d,%d\n", root_id, pick_code[pick_idx], net_rank,
              equity_rank);
    }
  }
  fclose(positions);
  fclose(output);
  string_builder_destroy(name);
  error_stack_destroy(errors);
  free(wide_moves);
  move_list_destroy(wide);
  move_list_destroy(pool);
  vnt_peg2_chooser_destroy(chooser);
  config_destroy(config);
  for (int pick_idx = 0; pick_idx < num_picks; pick_idx++) {
    free(pick_text[pick_idx]);
  }
  free(pick_text);
  free(pick_code);
  free(pick_root);
  free(line);
}

// pegnet:<mode>:<fields>: the PEG-net harness modes (1PEG/2PEG/3PEG data,
// choosers, evaluation, oracles and pegsim), split from value_net_test.c.
void peg_net_test_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  const char *mode = string_splitter_get_item(fields, 0);
  if (strings_equal(mode, "peg1eval")) {
    vnt_peg1_eval(fields);
  } else if (strings_equal(mode, "peg1data")) {
    vnt_peg1_data(fields);
  } else if (strings_equal(mode, "peg1feat")) {
    vnt_peg1_features(fields);
  } else if (strings_equal(mode, "peg1roots")) {
    vnt_peg1_roots(fields);
  } else if (strings_equal(mode, "peg1play")) {
    vnt_peg1_play(fields);
  } else if (strings_equal(mode, "peg1net")) {
    vnt_peg1_net_eval(fields);
  } else if (strings_equal(mode, "peg1bench")) {
    vnt_peg1_bench(fields);
  } else if (strings_equal(mode, "peg1feat2")) {
    vnt_peg1_features_v2(fields);
  } else if (strings_equal(mode, "peg1net2")) {
    vnt_peg1_net2_eval(fields);
  } else if (strings_equal(mode, "peg1games")) {
    vnt_peg1_games(fields);
  } else if (strings_equal(mode, "peg1pos")) {
    vnt_peg1_positions(fields);
  } else if (strings_equal(mode, "peg1deep")) {
    vnt_peg1_deep(fields);
  } else if (strings_equal(mode, "peg1cascade")) {
    vnt_peg1_cascade_eval(fields);
  } else if (strings_equal(mode, "peg1util")) {
    vnt_peg1_util_eval(fields);
  } else if (strings_equal(mode, "peg1pred")) {
    vnt_peg1_predict(fields);
  } else if (strings_equal(mode, "peg1games3")) {
    vnt_peg1_games3(fields);
  } else if (strings_equal(mode, "peg1greedy")) {
    vnt_peg1_greedy_eval(fields);
  } else if (strings_equal(mode, "peg1exact")) {
    vnt_peg1_exact_eval(fields);
  } else if (strings_equal(mode, "peg2pos")) {
    vnt_peg2_positions(fields);
  } else if (strings_equal(mode, "peg2data")) {
    vnt_peg2_data(fields);
  } else if (strings_equal(mode, "peg2eval")) {
    vnt_peg2_eval(fields);
  } else if (strings_equal(mode, "peg2feat")) {
    vnt_peg2_features(fields);
  } else if (strings_equal(mode, "peg2pick")) {
    vnt_peg2_pick_eval(fields);
  } else if (strings_equal(mode, "peg2wide")) {
    vnt_peg2_wide_eval(fields);
  } else if (strings_equal(mode, "peg3pos")) {
    vnt_peg3_positions(fields);
  } else if (strings_equal(mode, "peg3eval")) {
    vnt_peg3_eval(fields);
  } else if (strings_equal(mode, "pegsim")) {
    vnt_peg_sim(fields);
  } else if (strings_equal(mode, "pegnpos")) {
    // VNT_POS_BAG tiles in the bag (1..RACK_SIZE); peg1pos/peg2pos/peg3pos
    // are the fixed sizes.
    const char *bag_env = getenv("VNT_POS_BAG");
    const int bag_size = bag_env != NULL ? atoi(bag_env) : 0;
    if (bag_size < 1 || bag_size > RACK_SIZE) {
      log_fatal("pegnpos needs VNT_POS_BAG in 1..%d", RACK_SIZE);
    }
    vnt_pegn_positions(fields, bag_size);
  } else if (strings_equal(mode, "peg3oracle")) {
    vnt_peg3_oracle(fields);
  } else if (strings_equal(mode, "pegbudget")) {
    vnt_peg_budget(fields);
  } else if (strings_equal(mode, "pegrank")) {
    vnt_peg_rank(fields);
  } else {
    log_fatal("unknown pegnet spec: %s", spec);
  }
  string_splitter_destroy(fields);
}
