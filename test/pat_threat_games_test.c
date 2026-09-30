#include "pat_threat_games_test.h"

#include "../src/def/bai_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_features.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/play_chooser.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Research harness: game pairs of a static player that re-ranks its top
// moves by a sampled threat check against the same static player without
// it (both with PTG_PAT). For each of its top PTG_K moves within PTG_MARGIN
// points of the best, the treated player plays the move on a copy, deals
// the opponent PTG_R racks from the unseen pool (the same racks for every
// candidate) and takes the opponent's best reply score: over the whole
// board (PTG_MODE full) or only among plays covering an empty double or
// triple word square near any candidate's tiles (PTG_MODE partial). It
// then plays the candidate with the highest equity minus PTG_WEIGHT times
// its mean threat. Each pair plays one seed twice with the seats swapped.
//
// Environment:
//   PTG_OUT          CSV, appended: pair,spread_a,spread_b,changed_a,
//                    changed_b (spreads from the treated player's side;
//                    changed counts decisions the check altered)
//   PTG_WORKER / PTG_NUM_WORKERS   pair sharding
//   PTG_DEADLINE     Unix time; no pair starts at or after it
//   PTG_MODE         full, partial, fast or fastfull (default partial;
//                    see PTGSettings)
//   PTG_WEIGHT       default 0.3
//   PTG_K, PTG_R, PTG_MARGIN   defaults 5, 32, 20
//   PTG_PAT          default CSW24_hsp
//   PTG_SEED         default 20260928
//   PTG_MODE sim     the treated player is a PlayChooser simulating its top
//                    candidates instead (static endgame), one thread:
//   PTG_SIM_MS       per-move budget in milliseconds
//   PTG_SIM_CANDS, PTG_SIM_PLIES, PTG_SIM_MINP   default 15, 2, 30
//   PTG_ROLLOUT      rollout PAT: none, tws, tws_windows or all (default)
// CSV columns 6 and 7 are the treated player's decision wall time in
// microseconds and its turn count, both games together; columns 8 to 10 are
// CPU microseconds: the treated player's decisions, its opening top-move
// generation, and the plain opponent's decisions. PTG_EXTRA appends config
// arguments (for example -patcand false -patrollout false).

enum {
  PTG_MAX_K = 128,
  PCD_STATIC_POOL_CAP = 60,
  PTG_MAX_R = 256,
  PTG_POOL_CAP = 128,
  PTG_NAME_CAP = 64,
  // Pre-move plays kept per rack for fastfull: those within
  // PTG_PRE_MARGIN points of the rack's best, up to PTG_PRE_CAP.
  PTG_PRE_CAP = 64,
  PTG_PRE_MARGIN = 30,
};

static long ptg_env_long(const char *name, long default_value) {
  const char *value = getenv(name);
  return value ? strtol(value, NULL, 10) : default_value;
}

static uint64_t ptg_next(uint64_t *state) {
  *state += 0x9e3779b97f4a7c15ULL;
  uint64_t x = *state;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

static uint64_t ptg_now_ns(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static uint64_t ptg_cpu_ns(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

// CPU time in the treated player's opening top-move generation, and the
// plain opponent's whole decision, summed over a pair (columns 9 and 10).
static uint64_t ptg_first_gen_cpu_ns;
static uint64_t ptg_opponent_cpu_ns;
static uint64_t ptg_treated_cpu_ns;
// Movegen equity margin in points; 0 keeps every move up to the list's size.
static double ptg_gen_margin;
// Threat-check pruning (PTG_PRUNE): a candidate stops once even the lowest
// reply (PTG_REPLY_LO per rack, default 0) on its remaining racks cannot
// lift it above the best value so far. Rack searches run vs. possible.
static bool ptg_prune;
static double ptg_reply_lo;
// Configurable (approximate) lower bound on a candidate's mean threat, for
// example its p1 over sampled decisions; 0 disables it.
static double ptg_threat_lo_mean;
static long ptg_racks_run;
static long ptg_racks_possible;
static FILE *ptg_trace;
static uint64_t ptg_dup_cpu_ns;
static uint64_t ptg_ff_pre_ns;
static uint64_t ptg_ff_lane_ns;
static uint64_t ptg_ff_full_ns;
static long ptg_ff_calls_lane;
static long ptg_ff_calls_full;
static long ptg_ff_floor_hits;
static long ptg_ff_lane_improved;
static long ptg_ff_pairs;
static long ptg_ff_pre_plays;
static double ptg_ff_best_value;
static uint64_t ptg_ff_all_ns;
static long ptg_ff_all_calls;
static long ptg_ff_racks_run;
static long ptg_ff_racks_possible;

// Whether a candidate with equity eq_value and threat_sum over the racks
// searched so far could still beat best_value.
static bool ptg_can_still_win(double eq_value, double weight, double threat_sum,
                              int racks_left, int num_racks,
                              double best_value) {
  double min_mean =
      (threat_sum + racks_left * ptg_reply_lo) / (double)num_racks;
  if (min_mean < ptg_threat_lo_mean) {
    min_mean = ptg_threat_lo_mean;
  }
  return eq_value - weight * min_mean > best_value;
}

static Equity ptg_slack;
static long ptg_off_turns;
static double ptg_open_exch_const = -1.0;
static int ptg_w_hi_bag = 1000;
static int ptg_w_lo_bag = -1;
static double ptg_w_hi;
static double ptg_w_lo;
static double ptg_open_stat_sum;
static long ptg_open_stat_n;
static bool ptg_open_stat;
static FILE *ptg_open_stat_file;
static double ptg_open_slope_kept;
static double ptg_open_slope_full;
static long ptg_off_searches;

static void ptg_generate(const Game *game, MoveList *list, move_record_t record,
                         move_sort_t sort, uint64_t lane_mask,
                         const uint32_t *cover, bool use_floor, Equity floor) {
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = record,
      .move_sort_type = sort,
      .override_kwg = NULL,
      .eq_margin_movegen = int_to_equity(ptg_gen_margin),
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .lane_mask = lane_mask,
      .lane_cover_masks = cover,
      .use_best_floor = use_floor,
      .best_floor = floor,
      // Threat searches (by score) never want the opponent's exchanges,
      // which do not depend on our move; skipping their leave walk halves
      // the search. The player's own ranking (by equity) keeps them.
      .skip_exchanges = sort == MOVE_SORT_SCORE,
      .best_slack = sort == MOVE_SORT_SCORE ? ptg_slack : 0,
  };
  generate_moves(&args);
}

// Marks the empty double and triple word squares within RACK_SIZE of a
// fresh tile of move, in the tile's lane or one lane over, as lanes to
// search and squares a play must cover.
static void ptg_add_units(const Board *board, const Move *move,
                          uint64_t *lane_mask, uint32_t *cover) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int tile_row = move_get_row_start(move) + (vertical ? tile_idx : 0);
    const int tile_col = move_get_col_start(move) + (vertical ? 0 : tile_idx);
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        if (!board_is_empty(board, row, col) ||
            bonus_square_get_word_multiplier(
                board_get_bonus_square(board, row, col)) < 2) {
          continue;
        }
        if (abs(tile_row - row) <= 1 && abs(tile_col - col) <= RACK_SIZE) {
          *lane_mask |= (uint64_t)1 << row;
          cover[row] |= (uint32_t)1 << col;
        }
        if (abs(tile_col - col) <= 1 && abs(tile_row - row) <= RACK_SIZE) {
          *lane_mask |= (uint64_t)1 << (BOARD_DIM + col);
          cover[BOARD_DIM + col] |= (uint32_t)1 << row;
        }
      }
    }
  }
}

// Round-robin player kinds (PTG_A is the treated seat, PTG_B the other):
// static equity without PAT, static equity with PAT, or the threat check.
enum {
  PTG_KIND_NOPAT = 0,
  PTG_KIND_PAT = 1,
  PTG_KIND_THREAT = 2,
  PTG_KIND_THREAT_NOPAT = 3
};
static int ptg_kind_a = PTG_KIND_THREAT;
static int ptg_kind_b = PTG_KIND_PAT;

static int ptg_parse_kind(const char *name, int fallback) {
  if (name == NULL) {
    return fallback;
  }
  if (strcmp(name, "nopat") == 0) {
    return PTG_KIND_NOPAT;
  }
  if (strcmp(name, "pat") == 0) {
    return PTG_KIND_PAT;
  }
  if (strcmp(name, "threatnopat") == 0) {
    return PTG_KIND_THREAT_NOPAT;
  }
  assert(strcmp(name, "threat") == 0);
  return PTG_KIND_THREAT;
}

typedef struct PTGSettings {
  bool partial;
  // Fast premium check: each candidate searches only its own premium lanes,
  // and each rack's search starts from that rack's best play on the
  // unchanged board (B), so it only looks for plays that beat it; the
  // threat is the mean of max(B, best premium play).
  bool fast;
  // Fast full check: the opponent's best reply over the whole board, found
  // as the better of the best pre-move play the candidate does not disturb
  // and the best play touching the candidate (see ptg_fast_full_threats).
  bool fast_full;
  // Racks dealt disjointly from one shuffle of the pool (see ptg_choose)
  // rather than drawn independently.
  bool partition;
  // Sequential check: racks are searched in turn for every live candidate,
  // and a candidate leaves once its paired reply difference from the current
  // leader makes it worse by seq_z standard errors (after seq_min_racks).
  bool seq;
  bool seq_floor;
  double seq_z;
  int seq_min_racks;
  // Offense: weight on a survivor's follow-up score (after the opponent's
  // best reply) less off_base times its leave's follow-up on the unchanged
  // board, with off_draws follow-up racks per opponent rack.
  double off_w;
  double off_base;
  int off_draws;
  // The check runs only while the bag holds at least this many tiles.
  int min_bag;
  double weight;
  int k;
  int r;
  double margin;
} PTGSettings;

// A pre-move play of the opponent: its score and the squares it fills.
typedef struct PTGPlay {
  Equity score;
  int num_squares;
  int squares[RACK_SIZE];
} PTGPlay;

static int ptg_move_squares(const Move *move, int *squares) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return 0;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  int count = 0;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = move_get_row_start(move) + (vertical ? tile_idx : 0);
    const int col = move_get_col_start(move) + (vertical ? 0 : tile_idx);
    squares[count++] = row * BOARD_DIM + col;
  }
  return count;
}

static Equity ptg_best_placement_score(const MoveList *list) {
  if (move_list_get_count(list) > 0) {
    const Move *reply = move_list_get_move(list, 0);
    if (move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      return move_get_score(reply);
    }
  }
  return 0;
}

static void ptg_candidate_disturbance(const Board *board, const Move *move,
                                      bool *disturbed, uint64_t *lane_mask_out,
                                      uint32_t *cover) {
  uint64_t lane_mask = 0;
  int fresh[RACK_SIZE];
  const int num_fresh = ptg_move_squares(move, fresh);
  for (int fresh_idx = 0; fresh_idx < num_fresh; fresh_idx++) {
    const int fresh_row = fresh[fresh_idx] / BOARD_DIM;
    const int fresh_col = fresh[fresh_idx] % BOARD_DIM;
    disturbed[fresh[fresh_idx]] = true;
    for (int dir = 0; dir < 2; dir++) {
      for (int side = -1; side <= 1; side += 2) {
        int row = fresh_row;
        int col = fresh_col;
        while (row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM &&
               !board_is_empty(board, row, col)) {
          row += (dir == 1) ? side : 0;
          col += (dir == 0) ? side : 0;
        }
        if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM ||
            board_get_is_brick(board, row, col)) {
          continue;
        }
        disturbed[row * BOARD_DIM + col] = true;
        lane_mask |= (uint64_t)1 << row;
        cover[row] |= (uint32_t)1 << col;
        lane_mask |= (uint64_t)1 << (BOARD_DIM + col);
        cover[BOARD_DIM + col] |= (uint32_t)1 << row;
      }
    }
  }
  *lane_mask_out = lane_mask;
}

// The opponent's best reply score on the current rack of `after`, given the
// floor clear_value (-1: none) and the candidate's lanes.
static Equity ptg_ff_rack_value(Game *after, Equity clear_value,
                                uint64_t lane_mask, const uint32_t *cover,
                                MoveList *reply_list, bool verify) {
  Equity value = clear_value;
  ptg_ff_pairs++;
  if (value < 0) {
    // Every listed play is disturbed: search the whole board.
    const uint64_t full_start = ptg_cpu_ns();
    ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0, NULL,
                 false, 0);
    value = ptg_best_placement_score(reply_list);
    ptg_ff_full_ns += ptg_cpu_ns() - full_start;
    ptg_ff_calls_full++;
  } else if (lane_mask != 0) {
    const uint64_t lane_start = ptg_cpu_ns();
    ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE,
                 lane_mask, cover, true, value);
    const Equity touching = ptg_best_placement_score(reply_list);
    if (touching > value) {
      value = touching;
      ptg_ff_lane_improved++;
    }
    ptg_ff_lane_ns += ptg_cpu_ns() - lane_start;
    ptg_ff_calls_lane++;
  }
  if (verify) {
    ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0, NULL,
                 false, 0);
    const Equity check = ptg_best_placement_score(reply_list);
    if (check != value) {
      log_fatal("fastfull mismatch: full %d vs fast %d", equity_to_int(check),
                equity_to_int(value));
    }
  }
  return value;
}

// The fast full check's per-candidate threats: the mean over racks of the
// opponent's best reply score after the candidate. A post-move play either
// puts a tile on A, the empty squares bounding a run through one of the
// candidate's tiles in either direction (every hook, extension and
// playthrough of it), or it is a pre-move play that fills no square of the
// candidate or of A, unchanged in word and score. So the best reply is the
// better of B, the best pre-move play clear of those squares (from each
// rack's plays within PTG_PRE_MARGIN of its best), and the best play on A,
// searched only in A's lanes and only for plays beating B. A rack whose
// listed plays are all disturbed falls back to a full search.
static void ptg_build_pre_rack(Game *passed, Player *passed_opponent,
                               const Rack *rack, MoveList *pre_list,
                               MoveList *reply_list, PTGPlay *row,
                               int *count_out, bool pre_best_only,
                               int pre_margin_pts) {
  rack_copy(player_get_rack(passed_opponent), rack);
  move_list_reset(pre_list);
  const MoveGenArgs args = {
      .game = passed,
      .move_list = pre_list,
      .move_record_type = pre_best_only ? MOVE_RECORD_BEST
                                        : MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = int_to_equity(pre_margin_pts),
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .skip_exchanges = true,
  };
  generate_moves(&args);
  if (getenv("PTG_TIME_ALL") != NULL) {
    const uint64_t all_start = ptg_cpu_ns();
    ptg_generate(passed, reply_list, MOVE_RECORD_ALL, MOVE_SORT_SCORE, 0, NULL,
                 false, 0);
    ptg_ff_all_ns += ptg_cpu_ns() - all_start;
    ptg_ff_all_calls++;
  }
  move_list_sort_moves(pre_list);
  const int count = move_list_get_count(pre_list);
  *count_out = count;
  ptg_ff_pre_plays += count;
  for (int play_idx = 0; play_idx < count; play_idx++) {
    const Move *move = move_list_get_move(pre_list, play_idx);
    PTGPlay *play = &row[play_idx];
    play->score = move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE
                      ? move_get_score(move)
                      : 0;
    play->num_squares = ptg_move_squares(move, play->squares);
  }
}

static void ptg_fast_full_threats(const Game *game, const Move *const *cands,
                                  int num_cands, const Rack *racks,
                                  int num_racks, MoveList *reply_list,
                                  MoveList *pre_list, bool verify,
                                  double weight, double seq_z, int seq_min,
                                  double *threats) {
  static PTGPlay pre[PTG_MAX_R][PTG_PRE_CAP];
  // PTG_PRE_BEST keeps only each rack's best pre-move play; a candidate that
  // disturbs it sends that rack to the full search.
  const bool pre_best_only = getenv("PTG_PRE_BEST") != NULL;
  const int pre_margin_pts =
      (int)ptg_env_long("PTG_PRE_MARGIN_PTS", PTG_PRE_MARGIN);
  int pre_count[PTG_MAX_R];
  const uint64_t pre_start = ptg_cpu_ns();
  ptg_ff_best_value = -1e18;
  Game *passed = game_duplicate(game);
  Move *pass = move_create();
  move_set_as_pass(pass);
  play_move(pass, passed, NULL);
  move_destroy(pass);
  Player *passed_opponent =
      game_get_player(passed, game_get_player_on_turn_index(passed));
  const bool lazy_pre = seq_z > 0.0 && getenv("PTG_LAZY_PRE") != NULL;
  bool pre_built[PTG_MAX_R] = {false};
  for (int rack_idx = 0; rack_idx < num_racks && !lazy_pre; rack_idx++) {
    ptg_build_pre_rack(passed, passed_opponent, &racks[rack_idx], pre_list,
                       reply_list, pre[rack_idx], &pre_count[rack_idx],
                       pre_best_only, pre_margin_pts);
    pre_built[rack_idx] = true;
  }
  if (!lazy_pre) {
    game_destroy(passed);
  }
  ptg_ff_pre_ns += ptg_cpu_ns() - pre_start;

  if (seq_z > 0.0) {
    Game *afters[PTG_MAX_K];
    bool live[PTG_MAX_K];
    double cand_eq[PTG_MAX_K];
    double sum[PTG_MAX_K] = {0.0};
    uint64_t lane_masks[PTG_MAX_K];
    static uint32_t covers[PTG_MAX_K][2 * BOARD_DIM];
    static Equity clears[PTG_MAX_K][PTG_MAX_R];
    static double replies[PTG_MAX_K][PTG_MAX_R];
    static bool dists[PTG_MAX_K][BOARD_DIM * BOARD_DIM];
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      afters[cand_idx] = game_duplicate(game);
      play_move(cands[cand_idx], afters[cand_idx], NULL);
      memset(dists[cand_idx], 0, sizeof(dists[cand_idx]));
      lane_masks[cand_idx] = 0;
      memset(covers[cand_idx], 0, sizeof(covers[cand_idx]));
      ptg_candidate_disturbance(game_get_board(afters[cand_idx]),
                                cands[cand_idx], dists[cand_idx],
                                &lane_masks[cand_idx], covers[cand_idx]);
      cand_eq[cand_idx] = equity_to_double(move_get_equity(cands[cand_idx]));
      live[cand_idx] = true;
    }
    int num_live = num_cands;
    int racks_done = 0;
    ptg_ff_racks_possible += (long)num_cands * num_racks;
    while (racks_done < num_racks && num_live > 1) {
      if (!pre_built[racks_done]) {
        const uint64_t lazy_start = ptg_cpu_ns();
        ptg_build_pre_rack(passed, passed_opponent, &racks[racks_done],
                           pre_list, reply_list, pre[racks_done],
                           &pre_count[racks_done], pre_best_only,
                           pre_margin_pts);
        pre_built[racks_done] = true;
        ptg_ff_pre_ns += ptg_cpu_ns() - lazy_start;
      }
      for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
        clears[cand_idx][racks_done] = -1;
        for (int play_idx = 0; play_idx < pre_count[racks_done]; play_idx++) {
          const PTGPlay *play = &pre[racks_done][play_idx];
          bool clear = true;
          for (int sq_idx = 0; sq_idx < play->num_squares && clear; sq_idx++) {
            clear = !dists[cand_idx][play->squares[sq_idx]];
          }
          if (clear) {
            clears[cand_idx][racks_done] = play->score;
            break;
          }
        }
      }
      for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
        if (!live[cand_idx]) {
          continue;
        }
        Player *opponent = game_get_player(
            afters[cand_idx], game_get_player_on_turn_index(afters[cand_idx]));
        rack_copy(player_get_rack(opponent), &racks[racks_done]);
        const double value = equity_to_double(ptg_ff_rack_value(
            afters[cand_idx], clears[cand_idx][racks_done],
            lane_masks[cand_idx], covers[cand_idx], reply_list, verify));
        replies[cand_idx][racks_done] = value;
        sum[cand_idx] += value;
        ptg_ff_racks_run++;
      }
      racks_done++;
      if (racks_done < seq_min) {
        continue;
      }
      int leader = -1;
      double leader_value = -1e18;
      for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
        const double value =
            cand_eq[cand_idx] - weight * sum[cand_idx] / racks_done;
        if (live[cand_idx] && value > leader_value) {
          leader_value = value;
          leader = cand_idx;
        }
      }
      for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
        if (!live[cand_idx] || cand_idx == leader) {
          continue;
        }
        double diff_sum = 0.0;
        double diff_sq = 0.0;
        for (int rack_idx = 0; rack_idx < racks_done; rack_idx++) {
          const double diff =
              replies[cand_idx][rack_idx] - replies[leader][rack_idx];
          diff_sum += diff;
          diff_sq += diff * diff;
        }
        const double mean = diff_sum / racks_done;
        const double variance =
            (diff_sq - racks_done * mean * mean) / (racks_done - 1);
        const double std_err =
            sqrt((variance > 0.0 ? variance : 0.0) / racks_done);
        if ((cand_eq[cand_idx] - cand_eq[leader]) -
                weight * (mean - seq_z * std_err) <
            0.0) {
          live[cand_idx] = false;
          num_live--;
        }
      }
    }
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      threats[cand_idx] = live[cand_idx] ? sum[cand_idx] / racks_done : -1.0;
      game_destroy(afters[cand_idx]);
    }
    if (lazy_pre) {
      game_destroy(passed);
    }
    return;
  }

  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    Game *after = game_duplicate(game);
    play_move(cands[cand_idx], after, NULL);
    const Board *board = game_get_board(after);
    Player *opponent =
        game_get_player(after, game_get_player_on_turn_index(after));
    bool disturbed[BOARD_DIM * BOARD_DIM] = {false};
    uint64_t lane_mask = 0;
    uint32_t cover[2 * BOARD_DIM] = {0};
    ptg_candidate_disturbance(board, cands[cand_idx], disturbed, &lane_mask,
                              cover);
    double total = 0.0;
    // The first pre-move play the candidate leaves alone is a floor on the
    // rack's reply (-1: every listed play is disturbed), so the floors
    // bound the candidate's threat from below, blocking credit included.
    Equity clear_value[PTG_MAX_R];
    double floor_sum = 0.0;
    for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
      clear_value[rack_idx] = -1;
      for (int play_idx = 0; play_idx < pre_count[rack_idx]; play_idx++) {
        const PTGPlay *play = &pre[rack_idx][play_idx];
        bool clear = true;
        for (int sq_idx = 0; sq_idx < play->num_squares && clear; sq_idx++) {
          clear = !disturbed[play->squares[sq_idx]];
        }
        if (clear) {
          clear_value[rack_idx] = play->score;
          floor_sum += equity_to_double(play->score);
          break;
        }
      }
    }
    const double cand_equity =
        equity_to_double(move_get_equity(cands[cand_idx]));
    threats[cand_idx] = -1.0;
    if (ptg_prune && cand_idx > 0 &&
        !ptg_can_still_win(cand_equity, weight, floor_sum, 0, num_racks,
                           ptg_ff_best_value)) {
      ptg_ff_racks_possible += num_racks;
      game_destroy(after);
      continue;
    }
    bool killed = false;
    double remaining_floor = floor_sum;
    ptg_ff_racks_possible += num_racks;
    // Racks with a floor are searched first: their cheap lane searches can
    // kill the candidate before any full-board fallback is paid for.
    int order[PTG_MAX_R];
    int order_count = 0;
    for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
      if (clear_value[rack_idx] >= 0) {
        order[order_count++] = rack_idx;
      }
    }
    for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
      if (clear_value[rack_idx] < 0) {
        order[order_count++] = rack_idx;
      }
    }
    for (int order_idx = 0; order_idx < num_racks; order_idx++) {
      const int rack_idx = order[order_idx];
      if (ptg_prune && cand_idx > 0 &&
          !ptg_can_still_win(cand_equity, weight, total + remaining_floor, 0,
                             num_racks, ptg_ff_best_value)) {
        killed = true;
        break;
      }
      ptg_ff_racks_run++;
      if (clear_value[rack_idx] >= 0) {
        remaining_floor -= equity_to_double(clear_value[rack_idx]);
      }
      rack_copy(player_get_rack(opponent), &racks[rack_idx]);
      const Equity value = ptg_ff_rack_value(
          after, clear_value[rack_idx], lane_mask, cover, reply_list, verify);
      total += equity_to_double(value);
    }
    if (!killed) {
      threats[cand_idx] = total / num_racks;
      const double cand_value = cand_equity - weight * threats[cand_idx];
      if (cand_value > ptg_ff_best_value) {
        ptg_ff_best_value = cand_value;
      }
    }
    game_destroy(after);
  }
}

// One candidate's best reply on one rack, kept as a floor for the other
// candidates: it counts for them if it is also a legal play, unchanged in
// score, on the board before either candidate moved and after theirs.
typedef struct PTGReplyInfo {
  Equity score;
  bool usable;
  bool vertical;
  int num_placed;
  int placed[RACK_SIZE];
  int start_row;
  int start_col;
  int length;
} PTGReplyInfo;

// True if a play touches none of a candidate's disturbed squares with its
// tiles and none of the candidate's tiles within its word, including the
// squares just beyond the word's ends.
static bool ptg_reply_clear_of(const PTGReplyInfo *info, const bool *disturbed,
                               const bool *fresh) {
  for (int placed_idx = 0; placed_idx < info->num_placed; placed_idx++) {
    if (disturbed[info->placed[placed_idx]]) {
      return false;
    }
  }
  for (int pos = -1; pos <= info->length; pos++) {
    const int row = info->start_row + (info->vertical ? pos : 0);
    const int col = info->start_col + (info->vertical ? 0 : pos);
    if (row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM &&
        fresh[row * BOARD_DIM + col]) {
      return false;
    }
  }
  return true;
}

static void ptg_reply_info_from_move(PTGReplyInfo *info, const Move *move,
                                     const bool *disturbed, const bool *fresh) {
  info->usable = false;
  if (move == NULL || move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  info->score = move_get_score(move);
  info->vertical = board_is_dir_vertical(move_get_dir(move));
  info->num_placed = ptg_move_squares(move, info->placed);
  info->start_row = move_get_row_start(move);
  info->start_col = move_get_col_start(move);
  info->length = move_get_tiles_length(move);
  info->usable = ptg_reply_clear_of(info, disturbed, fresh);
}

// Sums the KLV values of an exchange's kept tiles and of the whole rack,
// each tile valued alone (no synergies).
static void ptg_exchange_tile_sums(const Game *game, const Move *exchange,
                                   double *kept_sum, double *full_sum) {
  const Player *player =
      game_get_player(game, game_get_player_on_turn_index(game));
  const Rack *rack = player_get_rack(player);
  const KLV *klv = player_get_klv(player);
  const int dist_size = rack_get_dist_size(rack);
  int thrown[MAX_ALPHABET_SIZE] = {0};
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(exchange);
       tile_idx++) {
    thrown[move_get_tile(exchange, tile_idx)]++;
  }
  *kept_sum = 0.0;
  *full_sum = 0.0;
  for (int ml = 0; ml < dist_size; ml++) {
    const int count = rack_get_letter(rack, ml);
    if (count == 0) {
      continue;
    }
    Rack single;
    rack_set_dist_size_and_reset(&single, dist_size);
    rack_add_letter(&single, (MachineLetter)ml);
    const double value = equity_to_double(klv_get_leave_value(klv, &single));
    *full_sum += count * value;
    *kept_sum += (count - thrown[ml]) * value;
  }
}

// Sequential threat check (see PTGSettings.seq): returns the chosen
// candidate's index.
static int ptg_choose_sequential(
    const Game *game, const PTGSettings *settings, const Move *const *cands,
    int num_cands, const Rack *racks, int num_racks, MoveList *reply_list,
    const MachineLetter *pool, int pool_size, uint64_t rng_seed, bool no_kill,
    double *out_threat_value, double *out_offense_value) {
  const char *trace_path = getenv("PCD_REPLY_TRACE");
  FILE *reply_trace = trace_path != NULL ? fopen(trace_path, "w") : NULL;
  bool trace_selected[PTG_MAX_K] = {false};
  if (trace_path != NULL) {
    assert(reply_trace != NULL);
    fprintf(reply_trace, "candidate,rack,reply_score\n");
    const char *trace_ranks = getenv("PCD_TRACE_RANKS");
    assert(trace_ranks != NULL);
    char rank_text[1024];
    snprintf(rank_text, sizeof(rank_text), "%s", trace_ranks);
    for (char *token = strtok(rank_text, ","); token != NULL;
         token = strtok(NULL, ",")) {
      const int rank = (int)strtol(token, NULL, 10);
      assert(rank > 0 && rank <= num_cands);
      trace_selected[rank - 1] = true;
    }
  }
  static Move *reply_mv[PTG_MAX_K][PTG_MAX_R];
  static bool reply_has[PTG_MAX_K][PTG_MAX_R];
  Game *after[PTG_MAX_K];
  double cand_equity[PTG_MAX_K];
  static double reply[PTG_MAX_K][PTG_MAX_R];
  static bool disturbed[PTG_MAX_K][BOARD_DIM * BOARD_DIM];
  static bool fresh[PTG_MAX_K][BOARD_DIM * BOARD_DIM];
  bool live[PTG_MAX_K];
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    after[cand_idx] = game_duplicate(game);
    play_move(cands[cand_idx], after[cand_idx], NULL);
    cand_equity[cand_idx] = equity_to_double(move_get_equity(cands[cand_idx]));
    live[cand_idx] = true;
    memset(disturbed[cand_idx], 0, sizeof(disturbed[cand_idx]));
    memset(fresh[cand_idx], 0, sizeof(fresh[cand_idx]));
    uint64_t unused_lanes = 0;
    uint32_t unused_cover[2 * BOARD_DIM] = {0};
    ptg_candidate_disturbance(game_get_board(after[cand_idx]), cands[cand_idx],
                              disturbed[cand_idx], &unused_lanes, unused_cover);
    int fresh_squares[RACK_SIZE];
    const int num_fresh = ptg_move_squares(cands[cand_idx], fresh_squares);
    for (int fresh_idx = 0; fresh_idx < num_fresh; fresh_idx++) {
      fresh[cand_idx][fresh_squares[fresh_idx]] = true;
    }
  }
  int num_live = num_cands;
  int racks_done = 0;
  double sum[PTG_MAX_K] = {0.0};
  while (racks_done < num_racks && num_live > 1) {
    PTGReplyInfo infos[PTG_MAX_K];
    int num_infos = 0;
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      if (!live[cand_idx]) {
        continue;
      }
      Player *opponent = game_get_player(
          after[cand_idx], game_get_player_on_turn_index(after[cand_idx]));
      rack_copy(player_get_rack(opponent), &racks[racks_done]);
      double score;
      if (ptg_open_exch_const >= 0.0 && !ptg_open_stat &&
          move_get_type(cands[cand_idx]) == GAME_EVENT_EXCHANGE &&
          board_get_tiles_played(game_get_board(game)) == 0) {
        double kept_sum;
        double full_sum;
        ptg_exchange_tile_sums(game, cands[cand_idx], &kept_sum, &full_sum);
        score = ptg_open_exch_const + ptg_open_slope_kept * kept_sum +
                ptg_open_slope_full * full_sum;
        reply_has[cand_idx][racks_done] = false;
      } else if (!settings->seq_floor) {
        ptg_generate(after[cand_idx], reply_list, MOVE_RECORD_BEST,
                     MOVE_SORT_SCORE, 0, NULL, false, 0);
        score = equity_to_double(ptg_best_placement_score(reply_list));
        if (ptg_open_stat &&
            move_get_type(cands[cand_idx]) == GAME_EVENT_EXCHANGE &&
            board_get_tiles_played(game_get_board(game)) == 0) {
          ptg_open_stat_sum += score;
          ptg_open_stat_n++;
          double kept_sum;
          double full_sum;
          ptg_exchange_tile_sums(game, cands[cand_idx], &kept_sum, &full_sum);
          if (ptg_open_stat_file != NULL) {
            (void)fprintf(ptg_open_stat_file, "%.1f,%.3f,%.3f\n", score,
                          kept_sum, full_sum);
          }
        }
        if (settings->off_w != 0.0) {
          const bool has_reply =
              move_list_get_count(reply_list) > 0 &&
              move_get_type(move_list_get_move(reply_list, 0)) ==
                  GAME_EVENT_TILE_PLACEMENT_MOVE;
          reply_has[cand_idx][racks_done] = has_reply;
          if (has_reply) {
            if (reply_mv[cand_idx][racks_done] == NULL) {
              reply_mv[cand_idx][racks_done] = move_create();
            }
            move_copy(reply_mv[cand_idx][racks_done],
                      move_list_get_move(reply_list, 0));
          }
        }
      } else {
        const PTGReplyInfo *floor_info = NULL;
        for (int info_idx = 0; info_idx < num_infos; info_idx++) {
          if (infos[info_idx].usable &&
              (floor_info == NULL ||
               infos[info_idx].score > floor_info->score) &&
              ptg_reply_clear_of(&infos[info_idx], disturbed[cand_idx],
                                 fresh[cand_idx])) {
            floor_info = &infos[info_idx];
          }
        }
        const Equity floor = floor_info != NULL ? floor_info->score : 0;
        ptg_generate(after[cand_idx], reply_list, MOVE_RECORD_BEST,
                     MOVE_SORT_SCORE, 0, NULL, floor_info != NULL, floor);
        const Equity found = ptg_best_placement_score(reply_list);
        PTGReplyInfo *mine = &infos[num_infos];
        if (floor_info != NULL && found <= floor) {
          *mine = *floor_info;
          score = equity_to_double(floor);
          ptg_ff_floor_hits++;
        } else {
          ptg_reply_info_from_move(mine,
                                   move_list_get_count(reply_list) > 0
                                       ? move_list_get_move(reply_list, 0)
                                       : NULL,
                                   disturbed[cand_idx], fresh[cand_idx]);
          score = equity_to_double(found);
        }
        num_infos++;
      }
      if (getenv("PTG_VERIFY") != NULL && settings->seq_floor) {
        ptg_generate(after[cand_idx], reply_list, MOVE_RECORD_BEST,
                     MOVE_SORT_SCORE, 0, NULL, false, 0);
        const Equity check = ptg_best_placement_score(reply_list);
        if (equity_to_double(check) != score) {
          log_fatal("seq floor mismatch: full %d vs floor %d",
                    equity_to_int(check), (int)score);
        }
      }
      if (reply_trace != NULL && trace_selected[cand_idx]) {
        fprintf(reply_trace, "%d,%d,%.6f\n", cand_idx + 1, racks_done + 1,
                score);
      }
      reply[cand_idx][racks_done] = score;
      sum[cand_idx] += score;
      ptg_racks_run++;
    }
    racks_done++;
    if (racks_done < settings->seq_min_racks) {
      continue;
    }
    int leader = -1;
    double leader_value = -1e18;
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      if (!live[cand_idx]) {
        continue;
      }
      const double value =
          cand_equity[cand_idx] - settings->weight * sum[cand_idx] / racks_done;
      if (value > leader_value) {
        leader_value = value;
        leader = cand_idx;
      }
    }
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      if (!live[cand_idx] || cand_idx == leader) {
        continue;
      }
      double diff_sum = 0.0;
      double diff_sq = 0.0;
      for (int rack_idx = 0; rack_idx < racks_done; rack_idx++) {
        const double diff = reply[cand_idx][rack_idx] - reply[leader][rack_idx];
        diff_sum += diff;
        diff_sq += diff * diff;
      }
      const double mean = diff_sum / racks_done;
      const double variance =
          (diff_sq - racks_done * mean * mean) / (racks_done - 1);
      const double std_err =
          sqrt((variance > 0.0 ? variance : 0.0) / racks_done);
      // Candidate minus leader value, best case at seq_z standard errors.
      const double best_case =
          (cand_equity[cand_idx] - cand_equity[leader]) -
          settings->weight * (mean - settings->seq_z * std_err);
      if (!no_kill && best_case < 0.0) {
        live[cand_idx] = false;
        num_live--;
      }
    }
  }
  ptg_racks_possible += (long)num_cands * num_racks;
  double offense_term[PTG_MAX_K] = {0.0};
  if (settings->off_w != 0.0 && num_live > 1) {
    assert(!settings->seq_floor);
    ptg_off_turns++;
    const int our_idx = game_get_player_on_turn_index(game);
    static MachineLetter draw_pool[PTG_MAX_R][PTG_POOL_CAP];
    int draw_pool_sizes[PTG_MAX_R];
    const bool conditioned = ptg_env_long("PCD_CONDITION_DRAWS", 0) != 0;
    uint64_t off_rng = rng_seed * 2654435761ULL + 12345ULL;
    for (int rack_idx = 0; rack_idx < racks_done; rack_idx++) {
      Rack excluded;
      rack_copy(&excluded, &racks[rack_idx]);
      int size = 0;
      for (int tile_idx = 0; tile_idx < pool_size; tile_idx++) {
        const MachineLetter tile = pool[tile_idx];
        if (conditioned && rack_get_letter(&excluded, tile) > 0) {
          rack_take_letter(&excluded, tile);
        } else {
          draw_pool[rack_idx][size++] = tile;
        }
      }
      assert(!conditioned || rack_get_total_letters(&excluded) == 0);
      draw_pool_sizes[rack_idx] = size;
      for (int draw_idx = 0; draw_idx < size - 1; draw_idx++) {
        const int pick =
            draw_idx + (int)(ptg_next(&off_rng) % (uint64_t)(size - draw_idx));
        const MachineLetter tile = draw_pool[rack_idx][pick];
        draw_pool[rack_idx][pick] = draw_pool[rack_idx][draw_idx];
        draw_pool[rack_idx][draw_idx] = tile;
      }
    }
    Move *pass_move = move_create();
    move_set_as_pass(pass_move);
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      if (!live[cand_idx]) {
        continue;
      }
      Rack leave;
      rack_copy(&leave, player_get_rack(game_get_player(game, our_idx)));
      for (int tile_idx = 0; tile_idx < move_get_tiles_length(cands[cand_idx]);
           tile_idx++) {
        const MachineLetter tile = move_get_tile(cands[cand_idx], tile_idx);
        if (tile == PLAYED_THROUGH_MARKER) {
          continue;
        }
        rack_take_letter(&leave,
                         get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile);
      }
      const int draw_count =
          RACK_SIZE - rack_get_total_letters(&leave) < draw_pool_sizes[0]
              ? RACK_SIZE - rack_get_total_letters(&leave)
              : draw_pool_sizes[0];
      double after_sum = 0.0;
      double base_sum = 0.0;
      Game *base_game = game_duplicate(game);
      for (int rack_idx = 0; rack_idx < racks_done; rack_idx++) {
        Game *reply_game = game_duplicate(after[cand_idx]);
        rack_copy(player_get_rack(game_get_player(
                      reply_game, game_get_player_on_turn_index(reply_game))),
                  &racks[rack_idx]);
        if (reply_has[cand_idx][rack_idx]) {
          play_move(reply_mv[cand_idx][rack_idx], reply_game, NULL);
        } else {
          play_move(pass_move, reply_game, NULL);
        }
        for (int draw_no = 0; draw_no < settings->off_draws; draw_no++) {
          const int offset = draw_no * draw_count;
          Rack *after_rack =
              player_get_rack(game_get_player(reply_game, our_idx));
          Rack *base_rack =
              player_get_rack(game_get_player(base_game, our_idx));
          rack_copy(after_rack, &leave);
          rack_copy(base_rack, &leave);
          for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
            const MachineLetter tile =
                draw_pool[rack_idx]
                         [(offset + draw_idx) % draw_pool_sizes[rack_idx]];
            rack_add_letter(after_rack, tile);
            rack_add_letter(base_rack, tile);
          }
          ptg_generate(reply_game, reply_list, MOVE_RECORD_BEST,
                       MOVE_SORT_SCORE, 0, NULL, false, 0);
          after_sum += equity_to_double(ptg_best_placement_score(reply_list));
          ptg_generate(base_game, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE,
                       0, NULL, false, 0);
          base_sum += equity_to_double(ptg_best_placement_score(reply_list));
          ptg_off_searches += 2;
        }
        game_destroy(reply_game);
      }
      game_destroy(base_game);
      const double samples = (double)racks_done * settings->off_draws;
      offense_term[cand_idx] =
          settings->off_w *
          (after_sum / samples - settings->off_base * base_sum / samples);
    }
    move_destroy(pass_move);
  }
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    if (out_threat_value != NULL) {
      out_threat_value[cand_idx] =
          cand_equity[cand_idx] - settings->weight * sum[cand_idx] / racks_done;
    }
    if (out_offense_value != NULL) {
      out_offense_value[cand_idx] =
          cand_equity[cand_idx] + offense_term[cand_idx];
    }
  }
  int best_idx = 0;
  double best_value = -1e18;
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    if (!live[cand_idx]) {
      continue;
    }
    const double value = cand_equity[cand_idx] -
                         settings->weight * sum[cand_idx] / racks_done +
                         offense_term[cand_idx];
    if (value > best_value) {
      best_value = value;
      best_idx = cand_idx;
    }
  }
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    game_destroy(after[cand_idx]);
  }
  if (reply_trace != NULL) {
    fclose(reply_trace);
  }
  return best_idx;
}

static int ptg_sample_racks(const Game *game, const PTGSettings *settings,
                            uint64_t rng_seed, Rack *racks, MachineLetter *pool,
                            int *out_pool_size) {
  const int on_turn = game_get_player_on_turn_index(game);
  const LetterDistribution *ld = game_get_ld(game);
  uint8_t unseen[MAX_ALPHABET_SIZE];
  pat_compute_unseen_counts(board_get_readonly_lanes(game_get_board(game), 0),
                            ld, player_get_rack(game_get_player(game, on_turn)),
                            unseen);
  int pool_size = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    for (int count = 0; count < unseen[ml]; count++) {
      pool[pool_size++] = (MachineLetter)ml;
    }
  }
  const int rack_size = pool_size < RACK_SIZE ? pool_size : RACK_SIZE;
  uint64_t rng = rng_seed;
  const int num_racks = settings->r;
  if (settings->partition) {
    // Disjoint deals: each shuffle of the pool is dealt into as many racks
    // as fill, so each unseen tile lands in at most one rack per deal, and
    // deals repeat until settings->r racks are dealt.
    int rack_idx = 0;
    while (rack_idx < num_racks) {
      MachineLetter shuffled[PTG_POOL_CAP];
      memcpy(shuffled, pool, sizeof(MachineLetter) * (size_t)pool_size);
      for (int draw_idx = 0; draw_idx < pool_size - 1; draw_idx++) {
        const int pick =
            draw_idx + (int)(ptg_next(&rng) % (uint64_t)(pool_size - draw_idx));
        const MachineLetter tile = shuffled[pick];
        shuffled[pick] = shuffled[draw_idx];
        shuffled[draw_idx] = tile;
      }
      for (int start = 0;
           start + rack_size <= pool_size && rack_idx < num_racks;
           start += rack_size) {
        rack_set_dist_size_and_reset(&racks[rack_idx], ld_get_size(ld));
        for (int tile_idx = 0; tile_idx < rack_size; tile_idx++) {
          rack_add_letter(&racks[rack_idx], shuffled[start + tile_idx]);
        }
        rack_idx++;
      }
    }
  } else {
    for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
      MachineLetter shuffled[PTG_POOL_CAP];
      memcpy(shuffled, pool, sizeof(MachineLetter) * (size_t)pool_size);
      rack_set_dist_size_and_reset(&racks[rack_idx], ld_get_size(ld));
      for (int draw_idx = 0; draw_idx < rack_size; draw_idx++) {
        const int pick =
            draw_idx + (int)(ptg_next(&rng) % (uint64_t)(pool_size - draw_idx));
        const MachineLetter tile = shuffled[pick];
        shuffled[pick] = shuffled[draw_idx];
        shuffled[draw_idx] = tile;
        rack_add_letter(&racks[rack_idx], tile);
      }
    }
  }
  *out_pool_size = pool_size;
  return num_racks;
}

// The treated player's move: its top moves re-ranked by the threat check.
// Returns whether the check changed the choice.
static bool ptg_choose(const Game *game, const PTGSettings *settings,
                       MoveList *top_list, MoveList *reply_list,
                       MoveList *pre_list, Move *chosen, uint64_t rng_seed) {
  const uint64_t first_gen_start = ptg_cpu_ns();
  ptg_slack =
      getenv("PTG_SLACK") ? double_to_equity(atof(getenv("PTG_SLACK"))) : 0;
  ptg_gen_margin = getenv("PTG_MARGIN_GEN") ? settings->margin : 0.0;
  ptg_generate(game, top_list,
               getenv("PTG_MARGIN_GEN") ? MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST
                                        : MOVE_RECORD_ALL,
               MOVE_SORT_EQUITY, 0, NULL, false, 0);
  ptg_gen_margin = 0.0;
  move_list_sort_moves(top_list);
  ptg_first_gen_cpu_ns += ptg_cpu_ns() - first_gen_start;
  const Move *best = move_list_get_move(top_list, 0);
  move_copy(chosen, best);
  const int bag = bag_get_letters(game_get_bag(game));
  PTGSettings phase_settings = *settings;
  if (bag >= ptg_w_hi_bag) {
    phase_settings.weight = ptg_w_hi;
  } else if (bag <= ptg_w_lo_bag) {
    phase_settings.weight = ptg_w_lo;
  }
  settings = &phase_settings;
  if (bag == 0 || bag < settings->min_bag ||
      move_get_type(best) == GAME_EVENT_PASS) {
    return false;
  }
  const double best_equity = equity_to_double(move_get_equity(best));
  int num_cands = 0;
  const Move *cands[PTG_MAX_K];
  for (int move_idx = 0;
       move_idx < move_list_get_count(top_list) && num_cands < settings->k;
       move_idx++) {
    const Move *move = move_list_get_move(top_list, move_idx);
    // A pass's equity is a sentinel, and it is never a threat candidate.
    if (move_get_type(move) == GAME_EVENT_PASS) {
      break;
    }
    if (equity_to_double(move_get_equity(move)) <
        best_equity - settings->margin) {
      break;
    }
    cands[num_cands++] = move;
  }
  if (num_cands < 2) {
    return false;
  }
  MachineLetter pool[PTG_POOL_CAP];
  int pool_size = 0;
  Rack racks[PTG_MAX_R];
  const int num_racks =
      ptg_sample_racks(game, settings, rng_seed, racks, pool, &pool_size);
  uint64_t lane_mask = 0;
  uint32_t cover[2 * BOARD_DIM] = {0};
  if (settings->partial && !settings->fast) {
    for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
      ptg_add_units(game_get_board(game), cands[cand_idx], &lane_mask, cover);
    }
  }
  // B per rack: its best score on the unchanged board, opponent to move.
  Equity baseline[PTG_MAX_R];
  if (settings->fast) {
    Game *passed = game_duplicate(game);
    Move *pass = move_create();
    move_set_as_pass(pass);
    play_move(pass, passed, NULL);
    move_destroy(pass);
    Player *opponent =
        game_get_player(passed, game_get_player_on_turn_index(passed));
    for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
      rack_copy(player_get_rack(opponent), &racks[rack_idx]);
      ptg_generate(passed, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                   NULL, false, 0);
      baseline[rack_idx] =
          move_list_get_count(reply_list) > 0
              ? move_get_score(move_list_get_move(reply_list, 0))
              : 0;
    }
    game_destroy(passed);
  }
  double fast_full_threats[PTG_MAX_K];
  if (settings->fast_full) {
    ptg_fast_full_threats(game, cands, num_cands, racks, num_racks, reply_list,
                          pre_list, getenv("PTG_VERIFY") != NULL,
                          settings->weight, settings->seq_z,
                          settings->seq_min_racks, fast_full_threats);
  }
  if (settings->seq && !settings->fast_full) {
    const int seq_idx = ptg_choose_sequential(
        game, settings, cands, num_cands, racks, num_racks, reply_list, pool,
        pool_size, rng_seed, false, NULL, NULL);
    move_copy(chosen, cands[seq_idx]);
    return seq_idx != 0;
  }
  int best_idx = 0;
  double best_value = -1e18;
  double trace_threat[PTG_MAX_K];
  for (int cand_idx = 0; cand_idx < num_cands; cand_idx++) {
    double threat = 0.0;
    bool killed = false;
    const double cand_equity =
        equity_to_double(move_get_equity(cands[cand_idx]));
    const bool prune_now = ptg_prune && cand_idx > 0;
    trace_threat[cand_idx] = -1.0;
    ptg_racks_possible += num_racks;
    if (settings->fast_full) {
      threat = fast_full_threats[cand_idx];
      if (threat < 0.0) {
        continue;
      }
    } else if (settings->fast) {
      uint64_t own_mask = 0;
      uint32_t own_cover[2 * BOARD_DIM] = {0};
      ptg_add_units(game_get_board(game), cands[cand_idx], &own_mask,
                    own_cover);
      Game *after = NULL;
      Player *opponent = NULL;
      if (own_mask != 0) {
        after = game_duplicate(game);
        play_move(cands[cand_idx], after, NULL);
        opponent = game_get_player(after, game_get_player_on_turn_index(after));
      }
      for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
        Equity value = baseline[rack_idx];
        if (after != NULL) {
          rack_copy(player_get_rack(opponent), &racks[rack_idx]);
          ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE,
                       own_mask, own_cover, true, baseline[rack_idx]);
          if (move_list_get_count(reply_list) > 0) {
            const Move *reply = move_list_get_move(reply_list, 0);
            if (move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                move_get_score(reply) > value) {
              value = move_get_score(reply);
            }
          }
        }
        if (after != NULL && getenv("PTG_VERIFY") != NULL) {
          // The floor must only prune: max(B, unrestricted best) agrees.
          ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE,
                       own_mask, own_cover, false, 0);
          Equity check = baseline[rack_idx];
          if (move_list_get_count(reply_list) > 0) {
            const Move *reply = move_list_get_move(reply_list, 0);
            if (move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
                move_get_score(reply) > check) {
              check = move_get_score(reply);
            }
          }
          if (check != value) {
            log_fatal("floor mismatch: %d vs %d", equity_to_int(check),
                      equity_to_int(value));
          }
        }
        threat += equity_to_double(value);
      }
      if (after != NULL) {
        game_destroy(after);
      }
      threat /= num_racks;
    } else if (move_get_type(cands[cand_idx]) ==
                   GAME_EVENT_TILE_PLACEMENT_MOVE &&
               (!settings->partial || lane_mask != 0)) {
      const uint64_t dup_start = ptg_cpu_ns();
      Game *after = game_duplicate(game);
      play_move(cands[cand_idx], after, NULL);
      ptg_dup_cpu_ns += ptg_cpu_ns() - dup_start;
      Player *opponent =
          game_get_player(after, game_get_player_on_turn_index(after));
      for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
        if (prune_now &&
            !ptg_can_still_win(cand_equity, settings->weight, threat,
                               num_racks - rack_idx, num_racks, best_value)) {
          killed = true;
          break;
        }
        ptg_racks_run++;
        rack_copy(player_get_rack(opponent), &racks[rack_idx]);
        if (settings->partial) {
          ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE,
                       lane_mask, cover, false, 0);
        } else {
          ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                       NULL, false, 0);
        }
        if (move_list_get_count(reply_list) > 0) {
          const Move *reply = move_list_get_move(reply_list, 0);
          if (move_get_type(reply) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
            threat += equity_to_double(move_get_score(reply));
          }
        }
      }
      game_destroy(after);
      threat /= num_racks;
    } else if (!settings->partial) {
      // A non-placement leaves the board as it is: the full reply there.
      Game *after = game_duplicate(game);
      play_move(cands[cand_idx], after, NULL);
      Player *opponent =
          game_get_player(after, game_get_player_on_turn_index(after));
      for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
        if (prune_now &&
            !ptg_can_still_win(cand_equity, settings->weight, threat,
                               num_racks - rack_idx, num_racks, best_value)) {
          killed = true;
          break;
        }
        ptg_racks_run++;
        rack_copy(player_get_rack(opponent), &racks[rack_idx]);
        ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                     NULL, false, 0);
        if (move_list_get_count(reply_list) > 0) {
          threat += equity_to_double(
              move_get_score(move_list_get_move(reply_list, 0)));
        }
      }
      game_destroy(after);
      threat /= num_racks;
    }
    if (killed) {
      continue;
    }
    trace_threat[cand_idx] = threat;
    if (getenv("PTG_DEBUG") != NULL) {
      (void)fprintf(stderr, "DBG seed %llu cand %d eq %.3f threat %.4f\n",
                    (unsigned long long)rng_seed, cand_idx, cand_equity,
                    threat);
    }
    const double value = cand_equity - settings->weight * threat;
    if (value > best_value) {
      best_value = value;
      best_idx = cand_idx;
    }
  }
  if (ptg_trace != NULL) {
    for (int cand_idx = 1; cand_idx < num_cands; cand_idx++) {
      if (trace_threat[cand_idx] >= 0.0 && trace_threat[0] >= 0.0) {
        fprintf(ptg_trace, "%d,%.3f,%.3f,%.3f\n", cand_idx,
                best_equity -
                    equity_to_double(move_get_equity(cands[cand_idx])),
                trace_threat[0], trace_threat[cand_idx]);
      }
    }
  }
  move_copy(chosen, cands[best_idx]);
  return best_idx != 0;
}

// Plays one game from seed with treated seat `treated`; returns the treated
// player's final spread and counts changed decisions.
typedef struct PTGSimCandContext {
  PTGSettings settings;
  MoveList *top_list;
  MoveList *reply_list;
  uint64_t seed;
  long turn;
  int pool_size;
  int num_static;
  int num_threat;
  int num_offense;
} PTGSimCandContext;

static void ptg_top_indices(const double *values, int count, int num_top,
                            bool *selected) {
  bool taken[PTG_MAX_K] = {false};
  for (int rank = 0; rank < num_top && rank < count; rank++) {
    int best = -1;
    for (int idx = 0; idx < count; idx++) {
      if (!taken[idx] && (best < 0 || values[idx] > values[best])) {
        best = idx;
      }
    }
    taken[best] = true;
    selected[best] = true;
  }
}

// Sim candidate source: the top static plays, the top plays by threat value,
// and the top plays by offense value, all drawn from a pool of the best
// static plays.
static bool ptg_sim_candidates(void *context, Game *game, MoveList *out) {
  PTGSimCandContext *ctx = (PTGSimCandContext *)context;
  const PTGSettings *settings = &ctx->settings;
  if (bag_get_letters(game_get_bag(game)) == 0) {
    return false;
  }
  ptg_generate(game, ctx->top_list, MOVE_RECORD_ALL, MOVE_SORT_EQUITY, 0, NULL,
               false, 0);
  move_list_sort_moves(ctx->top_list);
  const double best_equity =
      equity_to_double(move_get_equity(move_list_get_move(ctx->top_list, 0)));
  int num_cands = 0;
  const Move *cands[PTG_MAX_K];
  for (int move_idx = 0; move_idx < move_list_get_count(ctx->top_list) &&
                         num_cands < ctx->pool_size;
       move_idx++) {
    const Move *move = move_list_get_move(ctx->top_list, move_idx);
    if (move_get_type(move) == GAME_EVENT_PASS ||
        equity_to_double(move_get_equity(move)) <
            best_equity - settings->margin) {
      break;
    }
    cands[num_cands++] = move;
  }
  if (num_cands < 2) {
    return false;
  }
  MachineLetter pool[PTG_POOL_CAP];
  int pool_size = 0;
  Rack racks[PTG_MAX_R];
  const uint64_t rng_seed = ctx->seed * 1000003 + (uint64_t)ctx->turn++;
  const int num_racks =
      ptg_sample_racks(game, settings, rng_seed, racks, pool, &pool_size);
  double threat_value[PTG_MAX_K];
  double offense_value[PTG_MAX_K];
  (void)ptg_choose_sequential(game, settings, cands, num_cands, racks,
                              num_racks, ctx->reply_list, pool, pool_size,
                              rng_seed, true, threat_value, offense_value);
  bool selected[PTG_MAX_K] = {false};
  for (int idx = 0; idx < ctx->num_static && idx < num_cands; idx++) {
    selected[idx] = true;
  }
  ptg_top_indices(threat_value, num_cands, ctx->num_threat, selected);
  ptg_top_indices(offense_value, num_cands, ctx->num_offense, selected);
  move_list_reset(out);
  for (int idx = 0; idx < num_cands; idx++) {
    if (selected[idx]) {
      move_list_add_move(out, cands[idx]);
    }
  }
  move_list_sort_moves(out);
  return true;
}

static int ptg_play_game(Game *game, uint64_t seed, int treated,
                         const PTGSettings *settings, MoveList *top_list,
                         MoveList *reply_list, MoveList *plain_list,
                         MoveList *pre_list, Move *chosen, int *changed,
                         PlayChooser *chooser, uint64_t *elapsed_ns,
                         long *treated_turns) {
  game_reset(game);
  game_seed(game, seed);
  game_set_starting_player_index(game, 0);
  draw_starting_racks(game);
  for (int seat = 0; seat < 2; seat++) {
    const int seat_kind = seat == treated ? ptg_kind_a : ptg_kind_b;
    player_set_pat_usage(
        game_get_player(game, seat),
        seat_kind == PTG_KIND_NOPAT || seat_kind == PTG_KIND_THREAT_NOPAT, 0);
  }
  int turn = 0;
  while (!game_over(game)) {
    const int on_turn = game_get_player_on_turn_index(game);
    const int on_turn_kind = on_turn == treated ? ptg_kind_a : ptg_kind_b;
    if (on_turn_kind == PTG_KIND_THREAT ||
        on_turn_kind == PTG_KIND_THREAT_NOPAT) {
      const uint64_t start_ns = ptg_now_ns();
      const uint64_t start_cpu_ns = ptg_cpu_ns();
      if (chooser != NULL) {
        ErrorStack *error_stack = error_stack_create();
        play_chooser_choose_move(chooser, game, chosen, error_stack);
        assert(error_stack_is_empty(error_stack));
        error_stack_destroy(error_stack);
      } else if (ptg_choose(game, settings, top_list, reply_list, pre_list,
                            chosen, seed * 1000003 + (uint64_t)turn)) {
        (*changed)++;
      }
      *elapsed_ns += ptg_now_ns() - start_ns;
      ptg_treated_cpu_ns += ptg_cpu_ns() - start_cpu_ns;
      (*treated_turns)++;
      play_move(chosen, game, NULL);
    } else {
      const uint64_t start_cpu_ns = ptg_cpu_ns();
      const Move *plain_move = get_top_equity_move(game, plain_list);
      ptg_opponent_cpu_ns += ptg_cpu_ns() - start_cpu_ns;
      play_move(plain_move, game, NULL);
    }
    turn++;
  }
  return equity_to_int(player_get_score(game_get_player(game, treated)) -
                       player_get_score(game_get_player(game, 1 - treated)));
}

void test_pat_threat_games(void) {
  const char *out_path = getenv("PTG_OUT");
  if (!out_path) {
    log_fatal("set PTG_OUT");
  }
  const long worker = ptg_env_long("PTG_WORKER", 0);
  const long num_workers = ptg_env_long("PTG_NUM_WORKERS", 1);
  const long deadline = ptg_env_long("PTG_DEADLINE", 0);
  const char *mode = getenv("PTG_MODE") ? getenv("PTG_MODE") : "partial";
  const char *weight_text = getenv("PTG_WEIGHT");
  PTGSettings settings = {
      .partial = strcmp(mode, "full") != 0,
      .fast = strcmp(mode, "fast") == 0,
      .fast_full = strcmp(mode, "fastfull") == 0,
      .partition = ptg_env_long("PTG_PARTITION", 0) != 0,
      .seq = ptg_env_long("PTG_SEQ_Z10", 0) > 0,
      .seq_floor = ptg_env_long("PTG_SEQ_FLOOR", 0) != 0,
      .seq_z = (double)ptg_env_long("PTG_SEQ_Z10", 0) / 10.0,
      .seq_min_racks = (int)ptg_env_long("PTG_SEQ_MIN", 8),
      .off_w = (double)ptg_env_long("PTG_OFF_W100", 0) / 100.0,
      .off_base = (double)ptg_env_long("PTG_OFF_B10", 5) / 10.0,
      .off_draws = (int)ptg_env_long("PTG_OFF_D", 1),
      .min_bag = (int)ptg_env_long("PTG_MIN_BAG", 1),
      .weight = weight_text ? strtod(weight_text, NULL) : 0.3,
      .k = (int)ptg_env_long("PTG_K", 5),
      .r = (int)ptg_env_long("PTG_R", 32),
      .margin = (double)ptg_env_long("PTG_MARGIN", 20),
  };
  if (settings.k > PTG_MAX_K) {
    settings.k = PTG_MAX_K;
  }
  if (settings.r > PTG_MAX_R) {
    settings.r = PTG_MAX_R;
  }
  const bool sim_mode = strcmp(mode, "sim") == 0;
  ptg_kind_a = ptg_parse_kind(getenv("PTG_A"), PTG_KIND_THREAT);
  if (getenv("PTG_OPEN_EXCH") != NULL) {
    ptg_open_exch_const = atof(getenv("PTG_OPEN_EXCH"));
  }
  ptg_open_stat = getenv("PTG_OPEN_STAT") != NULL;
  if (ptg_open_stat) {
    ptg_open_stat_file = fopen(getenv("PTG_OPEN_STAT"), "w");
  }
  ptg_open_slope_kept =
      atof(getenv("PTG_OPEN_SLOPE_K") ? getenv("PTG_OPEN_SLOPE_K") : "0");
  ptg_open_slope_full =
      atof(getenv("PTG_OPEN_SLOPE_F") ? getenv("PTG_OPEN_SLOPE_F") : "0");
  ptg_w_hi_bag = (int)ptg_env_long("PTG_W_HI_BAG", 1000);
  ptg_w_lo_bag = (int)ptg_env_long("PTG_W_LO_BAG", -1);
  ptg_w_hi = atof(getenv("PTG_W_HI") ? getenv("PTG_W_HI") : "0.7");
  ptg_w_lo = atof(getenv("PTG_W_LO") ? getenv("PTG_W_LO") : "0.7");
  ptg_kind_b = ptg_parse_kind(getenv("PTG_B"), PTG_KIND_PAT);
  const long max_pairs = ptg_env_long("PTG_MAX_PAIRS", 0);
  ptg_prune = ptg_env_long("PTG_PRUNE", 0) != 0;
  ptg_reply_lo = (double)ptg_env_long("PTG_REPLY_LO", 0);
  ptg_threat_lo_mean = (double)ptg_env_long("PTG_THREAT_LO_MEAN", 0);
  if (getenv("PTG_TRACE") != NULL) {
    ptg_trace = fopen(getenv("PTG_TRACE"), "a");
  }
  const char *pat = getenv("PTG_PAT") ? getenv("PTG_PAT") : "CSW24_hsp";
  const uint64_t base_seed = (uint64_t)ptg_env_long("PTG_SEED", 20260928);
  char command[512];
  (void)snprintf(command, sizeof(command),
                 "set -lex CSW24 -leaves CSW24 -wmp true -pat %s -s1 equity "
                 "-s2 equity -r1 all -r2 all -numplays 1 -threads 1 %s",
                 pat, getenv("PTG_EXTRA") ? getenv("PTG_EXTRA") : "");
  Config *config = config_create_or_die(command);
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  ErrorStack *setup_errors = error_stack_create();
  WinPct *win_pcts = NULL;
  PlayChooserStrategy strategy = {0};
  if (sim_mode) {
    win_pcts = win_pct_create(config_get_data_paths(config), "winpct_english",
                              setup_errors);
    assert(error_stack_is_empty(setup_errors));
    const char *rollout = getenv("PTG_ROLLOUT") ? getenv("PTG_ROLLOUT") : "all";
    strategy = (PlayChooserStrategy){
        .pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM,
        .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
        .sim_plies = (int)ptg_env_long("PTG_SIM_PLIES", 2),
        .sim_max_candidates = (int)ptg_env_long("PTG_SIM_CANDS", 15),
        .sim_min_play_iterations = (int)ptg_env_long("PTG_SIM_MINP", 30),
        .fixed_seconds_per_move =
            (double)ptg_env_long("PTG_SIM_MS", 10) / 1000.0,
        .win_pcts = win_pcts,
        .num_threads = 1,
    };
    if (ptg_env_long("PTG_UNION", 0) != 0) {
      strategy.sim_max_candidates = (int)(ptg_env_long("PTG_UNION_KS", 0) +
                                          ptg_env_long("PTG_UNION_KT", 3) +
                                          ptg_env_long("PTG_UNION_KO", 3));
    }
    if (strcmp(rollout, "none") == 0) {
      strategy.pat_rollout_disabled = true;
    } else if (strcmp(rollout, "tws") == 0) {
      strategy.pat_rollout_disabled_classes_mask =
          PAT_CLASS_MASK_ALL & ~PAT_CLASS_MASK_TWS_ONLY;
    } else if (strcmp(rollout, "tws_windows") == 0) {
      strategy.pat_rollout_disabled_classes_mask =
          PAT_CLASS_MASK_ALL &
          ~((1u << PAT_PREMIUM_TWS) | PAT_CLASS_MASK_WINDOWS);
    } else {
      assert(strcmp(rollout, "all") == 0);
    }
  }
  PTGSimCandContext union_ctx = {0};
  MoveList *top_list = move_list_create(PTG_MAX_K * 4);
  MoveList *reply_list = move_list_create(1);
  MoveList *plain_list = move_list_create(1);
  MoveList *pre_list = move_list_create(PTG_PRE_CAP);
  Move *chosen = move_create();
  // Pairs already in the file are skipped, so a restart resumes.
  long done_max = -1;
  FILE *in = fopen(out_path, "r");
  if (in) {
    char line[256];
    while (fgets(line, sizeof(line), in)) {
      const long pair = strtol(line, NULL, 10);
      if (pair > done_max) {
        done_max = pair;
      }
    }
    (void)fclose(in);
  }
  FILE *out = fopen(out_path, "a");
  assert(out);
  for (long pair = worker;; pair += num_workers) {
    if (pair <= done_max) {
      continue;
    }
    if (max_pairs > 0 && pair >= max_pairs) {
      break;
    }
    if (deadline > 0 && (long)time(NULL) >= deadline) {
      break;
    }
    const uint64_t seed = ptg_next(&(uint64_t){base_seed + (uint64_t)pair});
    int changed_a = 0;
    int changed_b = 0;
    uint64_t elapsed_ns = 0;
    long treated_turns = 0;
    ptg_first_gen_cpu_ns = 0;
    ptg_opponent_cpu_ns = 0;
    ptg_treated_cpu_ns = 0;
    ptg_racks_run = 0;
    ptg_racks_possible = 0;
    PlayChooser *chooser = NULL;
    if (sim_mode) {
      strategy.seed = seed;
      if (ptg_env_long("PTG_UNION", 0) != 0) {
        union_ctx.settings = settings;
        union_ctx.settings.r = (int)ptg_env_long("PTG_UNION_R", 16);
        union_ctx.settings.margin =
            (double)ptg_env_long("PTG_UNION_MARGIN", 12);
        union_ctx.settings.off_w = 0.25;
        union_ctx.settings.off_base = 0.5;
        union_ctx.settings.off_draws = 1;
        union_ctx.top_list = top_list;
        union_ctx.reply_list = reply_list;
        union_ctx.seed = seed;
        union_ctx.turn = 0;
        union_ctx.pool_size = (int)ptg_env_long("PTG_UNION_N", 12);
        union_ctx.num_static = (int)ptg_env_long("PTG_UNION_KS", 0);
        union_ctx.num_threat = (int)ptg_env_long("PTG_UNION_KT", 3);
        union_ctx.num_offense = (int)ptg_env_long("PTG_UNION_KO", 3);
        strategy.sim_candidates_fn = ptg_sim_candidates;
        strategy.sim_candidates_context = &union_ctx;
      }
      chooser = play_chooser_create(&strategy);
    }
    const int spread_a = ptg_play_game(
        game, seed, 0, &settings, top_list, reply_list, plain_list, pre_list,
        chosen, &changed_a, chooser, &elapsed_ns, &treated_turns);
    const int spread_b = ptg_play_game(
        game, seed, 1, &settings, top_list, reply_list, plain_list, pre_list,
        chosen, &changed_b, chooser, &elapsed_ns, &treated_turns);
    play_chooser_destroy(chooser);
    fprintf(out, "%ld,%d,%d,%d,%d,%llu,%ld,%llu,%llu,%llu,%ld,%ld\n", pair,
            spread_a, spread_b, changed_a, changed_b,
            (unsigned long long)(elapsed_ns / 1000), treated_turns,
            (unsigned long long)(ptg_treated_cpu_ns / 1000),
            (unsigned long long)(ptg_first_gen_cpu_ns / 1000),
            (unsigned long long)(ptg_opponent_cpu_ns / 1000), ptg_racks_run,
            ptg_racks_possible);
    (void)fflush(out);
  }
  (void)fclose(out);
  (void)fprintf(stderr, "dup+play cpu us total %llu\n",
                (unsigned long long)(ptg_dup_cpu_ns / 1000));
  if (ptg_ff_all_calls > 0) {
    (void)fprintf(stderr, "ALL us/call %.1f (%ld calls)\n",
                  (double)ptg_ff_all_ns / 1000.0 / (double)ptg_ff_all_calls,
                  ptg_ff_all_calls);
  }
  if (ptg_ff_pairs > 0) {
    (void)fprintf(
        stderr,
        "FF pre_us %llu lane_us %llu full_us %llu pairs %ld lane_calls "
        "%ld improved %ld full_calls %ld pre_plays %ld racks %ld/%ld\n",
        (unsigned long long)(ptg_ff_pre_ns / 1000),
        (unsigned long long)(ptg_ff_lane_ns / 1000),
        (unsigned long long)(ptg_ff_full_ns / 1000), ptg_ff_pairs,
        ptg_ff_calls_lane, ptg_ff_lane_improved, ptg_ff_calls_full,
        ptg_ff_pre_plays, ptg_ff_racks_run, ptg_ff_racks_possible);
  }
  if (ptg_open_stat_file != NULL) {
    (void)fclose(ptg_open_stat_file);
    ptg_open_stat_file = NULL;
  }
  if (ptg_open_stat_n > 0) {
    (void)fprintf(stderr, "open exchange reply mean %.3f over %ld\n",
                  ptg_open_stat_sum / (double)ptg_open_stat_n, ptg_open_stat_n);
  }
  if (ptg_off_turns > 0) {
    (void)fprintf(stderr, "offense turns %ld searches %ld\n", ptg_off_turns,
                  ptg_off_searches);
  }
  if (ptg_ff_floor_hits > 0) {
    (void)fprintf(stderr, "seq floor hits %ld\n", ptg_ff_floor_hits);
  }
  if (ptg_trace != NULL) {
    (void)fclose(ptg_trace);
    ptg_trace = NULL;
  }
  move_destroy(chosen);
  move_list_destroy(plain_list);
  move_list_destroy(pre_list);
  move_list_destroy(reply_list);
  move_list_destroy(top_list);
  win_pct_destroy(win_pcts);
  error_stack_destroy(setup_errors);
  config_destroy(config);
}

// Oracle for the opening exchange constant: for seeded opening racks, sims the
// top static candidates (4 plies, static policy) and records, per candidate,
// the sim value, the static equity, and the mean best opponent reply over
// OXO_R sampled opponent racks, plus the KLV single-tile sums of our rack.
// The reply is a direct measurement, so the constant's calibration and the
// choice rules can be scored against the sim offline.
void test_open_exchange_oracle(void) {
  const char *out_path = getenv("OXO_OUT");
  if (!out_path) {
    log_fatal("set OXO_OUT");
  }
  const int num_racks_wanted = (int)ptg_env_long("OXO_RACKS", 1000);
  const int reply_racks = (int)ptg_env_long("OXO_R", 208);
  const int num_plays = (int)ptg_env_long("OXO_PLAYS", 12);
  const int iterations = (int)ptg_env_long("OXO_ITERS", 400);
  const uint64_t seed_base = (uint64_t)ptg_env_long("OXO_SEED", 8100000000L);
  const long worker = ptg_env_long("OXO_WORKER", 0);
  const long num_workers = ptg_env_long("OXO_NUM_WORKERS", 1);
  char *set_cmd = get_formatted_string(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity -r1 all "
      "-r2 all -numplays %d -plies 4 -threads %d -iter %d -sr rr -scond none "
      "-threshold none",
      num_plays, (int)ptg_env_long("OXO_THREADS", 10), num_plays * iterations);
  Config *config = config_create_or_die(set_cmd);
  free(set_cmd);
  StringBuilder *cgp_sb = string_builder_create();
  string_builder_add_string(cgp_sb, "cgp ");
  for (int row = 0; row < BOARD_DIM; row++) {
    string_builder_add_formatted_string(cgp_sb, "%s%d", row > 0 ? "/" : "",
                                        BOARD_DIM);
  }
  string_builder_add_string(cgp_sb, " / 0/0 0");
  load_and_exec_config_or_die(config, string_builder_peek(cgp_sb));
  string_builder_destroy(cgp_sb);
  Game *game = config_get_game(config);
  MoveList *reply_list = move_list_create(1);
  FILE *out = fopen(out_path, "w");
  (void)fprintf(out, "rack,cand,type,count,static,sim,reply_mean,full_sum,"
                     "kept_sum,move\n");
  ptg_gen_margin = 0.0;
  for (int attempt = (int)worker; attempt < num_racks_wanted;
       attempt += (int)num_workers) {
    game_reset(game);
    game_seed(game, seed_base + (uint64_t)attempt);
    draw_starting_racks(game);
    load_and_exec_config_or_die(config, "gen");
    SimResults *sim_results = config_get_sim_results(config);
    if (config_simulate_and_return_status(config, NULL, NULL, sim_results) !=
        ERROR_STATUS_SUCCESS) {
      continue;
    }
    const int on_turn = game_get_player_on_turn_index(game);
    const LetterDistribution *ld = game_get_ld(game);
    uint8_t unseen[MAX_ALPHABET_SIZE];
    pat_compute_unseen_counts(
        board_get_readonly_lanes(game_get_board(game), 0), ld,
        player_get_rack(game_get_player(game, on_turn)), unseen);
    MachineLetter pool[PTG_POOL_CAP];
    int pool_size = 0;
    for (int ml = 0; ml < ld_get_size(ld); ml++) {
      for (int count = 0; count < unseen[ml]; count++) {
        pool[pool_size++] = (MachineLetter)ml;
      }
    }
    Rack *sample_racks = malloc_or_die(sizeof(Rack) * (size_t)reply_racks);
    uint64_t rng = seed_base * 31ULL + (uint64_t)attempt;
    int rack_idx = 0;
    while (rack_idx < reply_racks) {
      MachineLetter shuffled[PTG_POOL_CAP];
      memcpy(shuffled, pool, sizeof(MachineLetter) * (size_t)pool_size);
      for (int draw_idx = 0; draw_idx < pool_size - 1; draw_idx++) {
        const int pick =
            draw_idx + (int)(ptg_next(&rng) % (uint64_t)(pool_size - draw_idx));
        const MachineLetter tile = shuffled[pick];
        shuffled[pick] = shuffled[draw_idx];
        shuffled[draw_idx] = tile;
      }
      for (int start = 0;
           start + RACK_SIZE <= pool_size && rack_idx < reply_racks;
           start += RACK_SIZE) {
        rack_set_dist_size_and_reset(&sample_racks[rack_idx], ld_get_size(ld));
        for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
          rack_add_letter(&sample_racks[rack_idx], shuffled[start + tile_idx]);
        }
        rack_idx++;
      }
    }
    const int sim_plays = sim_results_get_number_of_plays(sim_results);
    for (int cand_idx = 0; cand_idx < sim_plays; cand_idx++) {
      const SimmedPlay *play =
          sim_results_get_simmed_play(sim_results, cand_idx);
      const Move *move = simmed_play_get_move(play);
      if (move_get_type(move) == GAME_EVENT_PASS) {
        continue;
      }
      double reply_sum = 0.0;
      for (int sample_idx = 0; sample_idx < reply_racks; sample_idx++) {
        Game *after = game_duplicate(game);
        play_move(move, after, NULL);
        rack_copy(player_get_rack(game_get_player(
                      after, game_get_player_on_turn_index(after))),
                  &sample_racks[sample_idx]);
        ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                     NULL, false, 0);
        reply_sum += equity_to_double(ptg_best_placement_score(reply_list));
        game_destroy(after);
      }
      double kept_sum = 0.0;
      double full_sum = 0.0;
      const bool is_exchange = move_get_type(move) == GAME_EVENT_EXCHANGE;
      if (is_exchange) {
        ptg_exchange_tile_sums(game, move, &kept_sum, &full_sum);
      } else {
        // Placements: kept_sum is the leave's; full_sum the whole rack's.
        const Player *player = game_get_player(game, on_turn);
        const Rack *rack = player_get_rack(player);
        const KLV *klv = player_get_klv(player);
        for (int ml = 0; ml < rack_get_dist_size(rack); ml++) {
          if (rack_get_letter(rack, ml) == 0) {
            continue;
          }
          Rack single;
          rack_set_dist_size_and_reset(&single, rack_get_dist_size(rack));
          rack_add_letter(&single, (MachineLetter)ml);
          full_sum += rack_get_letter(rack, ml) *
                      equity_to_double(klv_get_leave_value(klv, &single));
        }
      }
      StringBuilder *sb = string_builder_create();
      string_builder_add_move_description(sb, move, config_get_ld(config));
      (void)fprintf(out, "%d,%d,%c,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%s\n", attempt,
                    cand_idx, is_exchange ? 'E' : 'P',
                    is_exchange ? move_get_tiles_length(move)
                                : move_get_tiles_played(move),
                    equity_to_double(move_get_equity(move)),
                    stat_get_mean(simmed_play_get_equity_stat(play)),
                    reply_sum / reply_racks, full_sum, kept_sum,
                    string_builder_peek(sb));
      string_builder_destroy(sb);
    }
    (void)fflush(out);
    free(sample_racks);
  }
  (void)fclose(out);
  move_list_destroy(reply_list);
  config_destroy(config);
}

// Position-level candidate diversity research. All ranking and rollouts
// disable PAT. Equal-size arms fill overlaps with the next static move.
// PCD_IN: lines pos,cgp; PCD_OUT: per-candidate simulation statistics.
// PCD_MS/PCD_REF_MS: selection/reference budgets, default 15000/60000.
// PCD_WORKER/PCD_NUM_WORKERS shard positions, not candidate arms.
static const int *pcd_output_global_ranks;

static void pcd_fill_static(bool *selected, int count, int target) {
  int size = 0;
  for (int idx = 0; idx < count; idx++) {
    size += selected[idx];
  }
  for (int idx = 0; idx < count && size < target; idx++) {
    if (!selected[idx]) {
      selected[idx] = true;
      size++;
    }
  }
}

static int pcd_find_move(const Move *const *cands, int count,
                         const Move *move) {
  for (int idx = 0; idx < count; idx++) {
    if (compare_moves_without_equity(cands[idx], move, true) == -1) {
      return idx;
    }
  }
  return -1;
}

static void pcd_sim_arm(Game *game, WinPct *win_pcts, const Move *const *cands,
                        int count, const bool *selected, const double *threat,
                        const double *setup, long pos, const char *arm,
                        double seconds, bool reference, uint64_t seed,
                        SimCtx **sim_ctx, SimResults *results,
                        SimResults *reuse, FILE *out) {
  MoveList *list = move_list_create(count);
  for (int idx = 0; idx < count; idx++) {
    if (selected[idx]) {
      move_list_add_move(list, cands[idx]);
    }
  }
  const int num_plays = move_list_get_count(list);
  ThreadControl *control = thread_control_create();
  thread_control_set_status(control, THREAD_CONTROL_STATUS_STARTED);
  ErrorStack *errors = error_stack_create();
  SimArgs args = {0};
  const int plies = (int)ptg_env_long("PCD_PLIES", 4);
  sim_args_fill(
      plies, list, num_plays, NULL, win_pcts, NULL, control, game, false, false,
      1, 0, num_plays, plies, seed, UINT64_C(1000000000000000),
      (uint64_t)ptg_env_long("PCD_SIM_MINP", 30), 0.0, BAI_THRESHOLD_NONE,
      seconds,
      reference ? BAI_SAMPLING_RULE_ROUND_ROBIN : BAI_SAMPLING_RULE_TOP_TWO_IDS,
      (reference || ptg_env_long("PCD_WIDTH_ONLY", 0) != 0 ||
       ptg_env_long("PCD_SELECTION_NO_CUTOFF", 0) != 0)
          ? -1.0
          : 0.0,
      1.0, 0.0, 100.0, false, NULL, &args);
  args.pat_rollout_disabled = true;
  const uint64_t start = ptg_now_ns();
  if (reuse == NULL) {
    simulate(&args, sim_ctx, results, errors);
  } else {
    results = reuse;
  }
  const double wall_ms = (double)(ptg_now_ns() - start) / 1000000.0;
  assert(error_stack_is_empty(errors));
  const Move *best = sim_results_get_best_move(results);
  assert(best != NULL);
  const int best_idx = pcd_find_move(cands, count, best);
  assert(best_idx >= 0);
  for (int play_idx = 0; play_idx < sim_results_get_number_of_plays(results);
       play_idx++) {
    const SimmedPlay *play = sim_results_get_simmed_play(results, play_idx);
    const int idx = pcd_find_move(cands, count, simmed_play_get_move(play));
    assert(idx >= 0 && selected[idx]);
    const Stat *eq = simmed_play_get_equity_stat(play);
    const Stat *wp = simmed_play_get_win_pct_stat(play);
    StringBuilder *move_text = string_builder_create();
    string_builder_add_ucgi_move(move_text, cands[idx], game_get_board(game),
                                 game_get_ld(game));
    fprintf(
        out,
        "%ld,%d,%s,%d,%s,%.6f,%.6f,%.6f,%.6f,%.6f,%.9f,%.9f,%d,%d,%.3f,%llu",
        pos, bag_get_letters(game_get_bag(game)), arm, idx + 1,
        string_builder_peek(move_text),
        equity_to_double(move_get_equity(cands[idx])), threat[idx], setup[idx],
        stat_get_mean(eq), stat_get_sem(eq), stat_get_mean(wp),
        stat_get_sem(wp), idx == best_idx, num_plays, wall_ms,
        (unsigned long long)sim_results_get_iteration_count(results));
    if (pcd_output_global_ranks != NULL) {
      fprintf(out, ",%d", pcd_output_global_ranks[idx]);
    }
    fputc('\n', out);
    string_builder_destroy(move_text);
  }
  fflush(out);
  error_stack_destroy(errors);
  thread_control_destroy(control);
  move_list_destroy(list);
}

static void pcd_add_exchange_quota(Game *game, MoveList *top,
                                   MoveList *exchange_list, const Move **cands,
                                   int *count, int *global_ranks, bool *quota) {
  move_list_reset(exchange_list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = exchange_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      // Only zero tiles may be placed; exchanges are generated separately.
      .tiles_played_mask = 1,
  };
  generate_moves(&args);
  move_list_sort_moves(exchange_list);
  const int target = (int)ptg_env_long("PCD_EXCHANGE_QUOTA", 3);
  assert(target >= 0 && target <= PTG_MAX_K - PCD_STATIC_POOL_CAP);
  const double margin = (double)ptg_env_long("PCD_EXCHANGE_MARGIN", 25);
  const double best =
      equity_to_double(move_get_equity(move_list_get_move(top, 0)));
  int taken = 0;
  for (int move_idx = 0;
       move_idx < move_list_get_count(exchange_list) && taken < target;
       move_idx++) {
    const Move *move = move_list_get_move(exchange_list, move_idx);
    assert(move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE);
    if (move_get_type(move) != GAME_EVENT_EXCHANGE) {
      continue;
    }
    if (equity_to_double(move_get_equity(move)) < best - margin) {
      continue;
    }
    int idx = pcd_find_move(cands, *count, move);
    if (idx < 0) {
      assert(*count < PTG_MAX_K);
      idx = (*count)++;
      cands[idx] = move;
      global_ranks[idx] = -1;
      for (int static_idx = 0; static_idx < move_list_get_count(top);
           static_idx++) {
        if (compare_moves_without_equity(
                move, move_list_get_move(top, static_idx), true) == -1) {
          global_ranks[idx] = static_idx + 1;
          break;
        }
      }
    }
    quota[idx] = true;
    taken++;
  }
}

static void pcd_run_adaptive(Game *game, WinPct *win_pcts,
                             const Move *const *cands, int count,
                             int core_count, const bool *quota,
                             const double *threat, const double *setup,
                             long pos, uint64_t seed, SimCtx **sim_ctx,
                             SimResults *results, FILE *out) {
  const char *names[] = {"static25", "exchange", "checks", "checks_exchange",
                         "reference"};
  bool selected[5][PTG_MAX_K] = {{false}};
  for (int arm = 0; arm < 4; arm++) {
    pcd_fill_static(selected[arm], core_count, 25);
  }
  for (int idx = 0; idx < count; idx++) {
    selected[1][idx] = selected[1][idx] || quota[idx];
  }
  ptg_top_indices(threat, count, 15, selected[2]);
  ptg_top_indices(setup, count, 15, selected[2]);
  for (int idx = 0; idx < count; idx++) {
    selected[3][idx] = selected[2][idx] || quota[idx];
  }
  pcd_fill_static(selected[4], count, count);
  SimResults *cache[4] = {NULL};
  for (int step = 0; step < 4; step++) {
    const int arm = (step + (int)(pos % 4)) % 4;
    SimResults *reuse = NULL;
    for (int previous = 0; previous < 4; previous++) {
      if (cache[previous] != NULL && memcmp(selected[arm], selected[previous],
                                            sizeof(selected[arm])) == 0) {
        reuse = cache[previous];
        break;
      }
    }
    pcd_sim_arm(game, win_pcts, cands, count, selected[arm], threat, setup, pos,
                names[arm], (double)ptg_env_long("PCD_MS", 15000) / 1000.0,
                false, seed, sim_ctx, results, reuse, out);
    cache[arm] = sim_results_duplicate(reuse != NULL ? reuse : results);
  }
  pcd_sim_arm(game, win_pcts, cands, count, selected[4], threat, setup, pos,
              "reference", (double)ptg_env_long("PCD_REF_MS", 60000) / 1000.0,
              true, seed ^ UINT64_C(0xd1b54a32d192ed03), sim_ctx, results, NULL,
              out);
  for (int arm = 0; arm < 4; arm++) {
    sim_results_destroy(cache[arm]);
  }
}

static void pcd_run_explore(Game *game, WinPct *win_pcts,
                            const Move *const *cands, int count, int core_count,
                            const bool *quota, const double *threat,
                            const double *setup, long pos, uint64_t seed,
                            SimCtx **sim_ctx, SimResults *results, FILE *out) {
  const char *names[] = {"static25", "threat_wide", "setup_wide", "signals",
                         "exchange", "combined",    "reference"};
  const bool width_only = ptg_env_long("PCD_WIDTH_ONLY", 0) != 0;
  const bool tile_only = ptg_env_long("PCD_TILE_ONLY", 0) != 0;
  const bool conditioned_only =
      ptg_env_long("PCD_CONDITION_ONLY", 0) != 0 || width_only || tile_only;
  if (conditioned_only) {
    names[2] = "setup_conditioned";
    names[3] = "signals_conditioned";
    names[5] = "combined_conditioned";
  }
  bool selected[7][PTG_MAX_K] = {{false}};
  double stronger_threat[PTG_MAX_K];
  double stronger_setup[PTG_MAX_K];
  double raw_threat[PTG_MAX_K];
  double raw_setup[PTG_MAX_K];
  const double best = equity_to_double(move_get_equity(cands[0]));
  for (int arm = 0; arm < 6; arm++) {
    pcd_fill_static(selected[arm], core_count, 25);
  }
  for (int idx = 0; idx < count; idx++) {
    const double equity = equity_to_double(move_get_equity(cands[idx]));
    stronger_threat[idx] = equity + 2.0 * (threat[idx] - equity);
    stronger_setup[idx] = equity + 3.0 * (setup[idx] - equity);
    raw_threat[idx] = equity >= best - 25.0 ? threat[idx] - equity : -1e18;
    raw_setup[idx] = equity >= best - 25.0 ? setup[idx] - equity : -1e18;
    selected[4][idx] = selected[4][idx] || quota[idx];
  }
  ptg_top_indices(stronger_threat, count, 25, selected[1]);
  ptg_top_indices(stronger_setup, count, 25, selected[2]);
  ptg_top_indices(raw_threat, count, 5, selected[3]);
  ptg_top_indices(raw_setup, count, 5, selected[3]);
  for (int idx = 0; idx < count; idx++) {
    if (idx >= 25 &&
        equity_to_double(move_get_equity(cands[idx])) < best - 25.0) {
      selected[3][idx] = false;
    }
    selected[5][idx] = selected[1][idx] || selected[2][idx] ||
                       selected[3][idx] || selected[4][idx];
  }
  if (width_only) {
    names[0] = "static25_fulltime";
    names[2] = "adaptive_static";
    names[3] = "static60";
    names[5] = "adaptive_signals";
    memset(selected[2], 0, sizeof(selected[2]));
    memset(selected[3], 0, sizeof(selected[3]));
    memset(selected[5], 0, sizeof(selected[5]));
    for (int idx = 0; idx < core_count; idx++) {
      selected[2][idx] =
          equity_to_double(move_get_equity(cands[idx])) >= best - 15.0;
    }
    pcd_fill_static(selected[2], core_count, 8);
    pcd_fill_static(selected[3], core_count, 60);
    memcpy(selected[5], selected[2], sizeof(selected[5]));
    ptg_top_indices(raw_threat, count, 5, selected[5]);
    ptg_top_indices(raw_setup, count, 5, selected[5]);
    for (int idx = 0; idx < count; idx++) {
      if (!selected[2][idx] &&
          equity_to_double(move_get_equity(cands[idx])) < best - 25.0) {
        selected[5][idx] = false;
      }
      selected[5][idx] = selected[5][idx] || quota[idx];
    }
  }
  if (tile_only) {
    names[0] = "static25_fulltime";
    names[2] = "tile_quota";
    names[5] = "tile_exchange";
    memcpy(selected[2], selected[0], sizeof(selected[2]));
    for (int tiles = 1; tiles <= RACK_SIZE; tiles++) {
      for (int idx = 0; idx < core_count; idx++) {
        if (move_get_type(cands[idx]) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
            move_get_tiles_played(cands[idx]) == tiles &&
            equity_to_double(move_get_equity(cands[idx])) >= best - 35.0) {
          selected[2][idx] = true;
          break;
        }
      }
    }
    for (int idx = 0; idx < count; idx++) {
      selected[5][idx] = selected[2][idx] || quota[idx];
    }
  }
  pcd_fill_static(selected[6], count, count);
  SimResults *cache[6] = {NULL};
  for (int step = 0; step < 6; step++) {
    const int arm = (step + (int)(pos % 6)) % 6;
    if (tile_only && arm != 0 && arm != 2 && arm != 5) {
      continue;
    }
    if (!tile_only && conditioned_only && !(width_only && arm == 0) &&
        arm != 2 && arm != 3 && arm != 5) {
      continue;
    }
    SimResults *reuse = NULL;
    for (int previous = 0; previous < 6; previous++) {
      if (cache[previous] != NULL && memcmp(selected[arm], selected[previous],
                                            sizeof(selected[arm])) == 0) {
        reuse = cache[previous];
        break;
      }
    }
    pcd_sim_arm(game, win_pcts, cands, count, selected[arm], threat, setup, pos,
                names[arm], (double)ptg_env_long("PCD_MS", 15000) / 1000.0,
                false, seed, sim_ctx, results, reuse, out);
    cache[arm] = sim_results_duplicate(reuse != NULL ? reuse : results);
  }
  if (!conditioned_only || ptg_env_long("PCD_WIDTH_REFERENCE", 0) != 0 ||
      ptg_env_long("PCD_TILE_REFERENCE", 0) != 0) {
    pcd_sim_arm(game, win_pcts, cands, count, selected[6], threat, setup, pos,
                "reference", (double)ptg_env_long("PCD_REF_MS", 60000) / 1000.0,
                true, seed ^ UINT64_C(0xd1b54a32d192ed03), sim_ctx, results,
                NULL, out);
  }
  for (int arm = 0; arm < 6; arm++) {
    sim_results_destroy(cache[arm]);
  }
}

// Targeted blocking study: unchanged admission formula, plus a same-size
// static control. All identical candidate sets share their completed sim.
static void pcd_run_blocking(Game *game, WinPct *win_pcts,
                             const Move *const *cands, int count,
                             int core_count, const double *threat,
                             const double *setup, long pos, uint64_t seed,
                             SimCtx **sim_ctx, SimResults *results, FILE *out) {
  bool selected[4][PTG_MAX_K] = {{false}};
  double adjusted[PTG_MAX_K];
  pcd_fill_static(selected[0], core_count, 25);
  memcpy(selected[1], selected[0], sizeof(selected[1]));
  for (int candidate_idx = 0; candidate_idx < count; candidate_idx++) {
    const double equity =
        equity_to_double(move_get_equity(cands[candidate_idx]));
    adjusted[candidate_idx] = equity + 2.0 * (threat[candidate_idx] - equity);
  }
  ptg_top_indices(adjusted, count, 25, selected[1]);
  int blocking_count = 0;
  for (int candidate_idx = 0; candidate_idx < count; candidate_idx++) {
    blocking_count += selected[1][candidate_idx];
  }
  assert(blocking_count <= core_count);
  pcd_fill_static(selected[2], core_count, blocking_count);
  pcd_fill_static(selected[3], count, count);
  const char *names[] = {"static25", "threat_wide", "static_blocking_count"};
  SimResults *cache[3] = {NULL};
  for (int step = 0; step < 3; step++) {
    const int arm = (step + (int)(pos % 3)) % 3;
    SimResults *reuse = NULL;
    for (int previous = 0; previous < 3; previous++) {
      if (cache[previous] != NULL && memcmp(selected[arm], selected[previous],
                                            sizeof(selected[arm])) == 0) {
        reuse = cache[previous];
        break;
      }
    }
    pcd_sim_arm(game, win_pcts, cands, count, selected[arm], threat, setup, pos,
                names[arm], (double)ptg_env_long("PCD_MS", 15000) / 1000.0,
                false, seed, sim_ctx, results, reuse, out);
    cache[arm] = sim_results_duplicate(reuse != NULL ? reuse : results);
  }
  pcd_sim_arm(game, win_pcts, cands, count, selected[3], threat, setup, pos,
              "reference", (double)ptg_env_long("PCD_REF_MS", 60000) / 1000.0,
              true, seed ^ UINT64_C(0xd1b54a32d192ed03), sim_ctx, results, NULL,
              out);
  for (int arm = 0; arm < 3; arm++) {
    sim_results_destroy(cache[arm]);
  }
}

static void pcd_run_matched(Game *game, WinPct *win_pcts,
                            const Move *const *cands, int count, int core_count,
                            const double *threat, const double *setup, long pos,
                            uint64_t seed, SimCtx **sim_ctx,
                            SimResults *results, FILE *out) {
  const char *path = getenv("PCD_MATCHED_COUNTS");
  assert(path != NULL);
  FILE *counts = fopen(path, "r");
  assert(counts != NULL);
  int setup_count = 0;
  int combined_count = 0;
  char line[256];
  while (fgets(line, sizeof(line), counts)) {
    long input_pos = -1;
    int input_setup = 0;
    int input_combined = 0;
    if (sscanf(line, "%ld,%d,%d", &input_pos, &input_setup, &input_combined) ==
            3 &&
        input_pos == pos) {
      setup_count = input_setup;
      combined_count = input_combined;
      break;
    }
  }
  fclose(counts);
  assert(setup_count >= 25 && combined_count >= setup_count);
  assert(combined_count <= core_count);
  bool selected[2][PTG_MAX_K] = {{false}};
  pcd_fill_static(selected[0], core_count, setup_count);
  pcd_fill_static(selected[1], core_count, combined_count);
  const char *names[] = {"static_setup_count", "static_combined_count"};
  SimResults *cache = NULL;
  const int first = (int)(pos % 2);
  for (int step = 0; step < 2; step++) {
    const int arm = (first + step) % 2;
    SimResults *reuse = setup_count == combined_count ? cache : NULL;
    pcd_sim_arm(game, win_pcts, cands, count, selected[arm], threat, setup, pos,
                names[arm], (double)ptg_env_long("PCD_MS", 15000) / 1000.0,
                false, seed, sim_ctx, results, reuse, out);
    if (step == 0) {
      cache = sim_results_duplicate(results);
    }
  }
  sim_results_destroy(cache);
}

static void pcd_run_admission(Game *game, WinPct *win_pcts,
                              const Move *const *cands, int count,
                              int core_count, const double *threat,
                              const double *setup, long pos, uint64_t seed,
                              SimCtx **sim_ctx, SimResults *results,
                              FILE *out) {
  const char *ranks = getenv("PCD_ADMIT_RANKS");
  assert(ranks != NULL);
  bool selected[2][PTG_MAX_K] = {{false}};
  pcd_fill_static(selected[0], core_count, 25);
  memcpy(selected[1], selected[0], sizeof(selected[1]));
  while (*ranks != '\0') {
    char *end = NULL;
    const long rank = strtol(ranks, &end, 10);
    assert(end != ranks && rank >= 1 && rank <= count);
    selected[1][rank - 1] = true;
    if (*end == ',') {
      ranks = end + 1;
    } else {
      assert(*end == '\0');
      break;
    }
  }
  const char *names[] = {"static25_fulltime", "static25_admitted"};
  for (int step = 0; step < 2; step++) {
    const int arm = (step + (int)(pos % 2)) % 2;
    pcd_sim_arm(game, win_pcts, cands, count, selected[arm], threat, setup, pos,
                names[arm], (double)ptg_env_long("PCD_MS", 15000) / 1000.0,
                false, seed, sim_ctx, results, NULL, out);
  }
}

static void pcd_generate_positions(const char *path) {
  FILE *out = fopen(path, "w");
  assert(out != NULL);
  char *meta_path = get_formatted_string("%s.meta.csv", path);
  FILE *meta = fopen(meta_path, "w");
  assert(meta != NULL);
  free(meta_path);
  fprintf(meta, "pos,game,turn,bag\n");
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1 -threads 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  MoveList *list = move_list_create(1);
  const long games = ptg_env_long("PCD_GENERATE_GAMES", 128);
  long pos = 0;
  for (long game_idx = 0; game_idx < games; game_idx++) {
    game_reset(game);
    game_seed(game, ptg_next(&(uint64_t){(uint64_t)ptg_env_long(
                                             "PCD_GENERATE_SEED", 2026092919) +
                                         (uint64_t)game_idx}));
    game_set_starting_player_index(game, 0);
    draw_starting_racks(game);
    for (int seat = 0; seat < 2; seat++) {
      player_set_pat_usage(game_get_player(game, seat), true, 0);
    }
    int turn = 0;
    while (!game_over(game)) {
      assert(pos < 10000 && turn < 200);
      const int bag = bag_get_letters(game_get_bag(game));
      if (bag >= ptg_env_long("PCD_GENERATE_MIN_BAG", 7)) {
        char *cgp = game_get_cgp(game, true);
        fprintf(out, "%ld,%s\n", pos, cgp);
        fprintf(meta, "%ld,%ld,%d,%d\n", pos, game_idx, turn, bag);
        free(cgp);
        pos++;
      }
      const Move *move = get_top_equity_move(game, list);
      play_move(move, game, NULL);
      turn++;
    }
  }
  move_list_destroy(list);
  config_destroy(config);
  fclose(out);
  fclose(meta);
}

static void pcd_write_openings(const char *path) {
  FILE *out = fopen(path, "w");
  assert(out != NULL);
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1 -threads 1");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  const long count = ptg_env_long("PCD_OPEN_COUNT", 16);
  const uint64_t opening_seed =
      (uint64_t)ptg_env_long("PCD_OPEN_SEED", 20260929);
  for (long idx = 0; idx < count; idx++) {
    game_reset(game);
    game_seed(game, ptg_next(&(uint64_t){opening_seed + (uint64_t)idx}));
    game_set_starting_player_index(game, 0);
    draw_starting_racks(game);
    char *cgp = game_get_cgp(game, true);
    fprintf(out, "%ld,%s\n", 6000 + idx, cgp);
    free(cgp);
  }
  fclose(out);
  config_destroy(config);
}

// Pass-relative check diagnostics. Both branches use the same sampled
// opponent rack and the same candidate leave/refill for our next turn.
static void pcd_pass_relative_checks(
    const Game *game, const PTGSettings *settings, const Move *const *cands,
    int count, const Rack *racks, int num_racks, MoveList *reply_list,
    const MachineLetter *pool, int pool_size, uint64_t seed, long pos,
    double *threat, double *setup) {
  const int our_idx = game_get_player_on_turn_index(game);
  const int opponent_idx = 1 - our_idx;
  const bool conditioned = ptg_env_long("PCD_CONDITION_DRAWS", 1) != 0;
  Game *pass_replied[PTG_MAX_R];
  double pass_scores[PTG_MAX_R];
  MachineLetter draws[PTG_MAX_R][PTG_POOL_CAP];
  int draw_sizes[PTG_MAX_R];
  uint64_t draw_rng = seed * UINT64_C(2654435761) + UINT64_C(12345);
  Move *pass_move = move_create();
  move_set_as_pass(pass_move);
  for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    Rack excluded;
    rack_copy(&excluded, &racks[rack_idx]);
    int draw_size = 0;
    for (int tile_idx = 0; tile_idx < pool_size; tile_idx++) {
      const MachineLetter tile = pool[tile_idx];
      if (conditioned && rack_get_letter(&excluded, tile) > 0) {
        rack_take_letter(&excluded, tile);
      } else {
        draws[rack_idx][draw_size++] = tile;
      }
    }
    assert(!conditioned || rack_get_total_letters(&excluded) == 0);
    draw_sizes[rack_idx] = draw_size;
    for (int draw_idx = 0; draw_idx < draw_size - 1; draw_idx++) {
      const int pick = draw_idx + (int)(ptg_next(&draw_rng) %
                                        (uint64_t)(draw_size - draw_idx));
      const MachineLetter tile = draws[rack_idx][pick];
      draws[rack_idx][pick] = draws[rack_idx][draw_idx];
      draws[rack_idx][draw_idx] = tile;
    }
    Game *baseline = game_duplicate(game);
    play_move(pass_move, baseline, NULL);
    rack_copy(player_get_rack(game_get_player(baseline, opponent_idx)),
              &racks[rack_idx]);
    ptg_generate(baseline, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                 NULL, false, 0);
    pass_scores[rack_idx] =
        equity_to_double(ptg_best_placement_score(reply_list));
    const Move *reply =
        move_list_get_count(reply_list) > 0 &&
                move_get_type(move_list_get_move(reply_list, 0)) ==
                    GAME_EVENT_TILE_PLACEMENT_MOVE
            ? move_list_get_move(reply_list, 0)
            : pass_move;
    play_move(reply, baseline, NULL);
    pass_replied[rack_idx] = baseline;
  }
  const char *check_path = getenv("PCD_CHECK_OUT");
  FILE *check_out = check_path != NULL ? fopen(check_path, "a+") : NULL;
  if (check_path != NULL) {
    assert(check_out != NULL);
    fseek(check_out, 0, SEEK_END);
    if (ftell(check_out) == 0) {
      fprintf(check_out,
              "pos,pool_index,move,static_eq,pass_reply_mean,"
              "candidate_reply_mean,blocking_delta,blocking_adjustment,"
              "pass_followup_mean,candidate_followup_mean,setup_delta,"
              "setup_adjustment,racks,conditioned,terminal_replies\n");
    }
  }
  for (int cand_idx = 0; cand_idx < count; cand_idx++) {
    Rack leave;
    rack_copy(&leave, player_get_rack(game_get_player(game, our_idx)));
    for (int tile_idx = 0; tile_idx < move_get_tiles_length(cands[cand_idx]);
         tile_idx++) {
      const MachineLetter tile = move_get_tile(cands[cand_idx], tile_idx);
      if (tile != PLAYED_THROUGH_MARKER ||
          move_get_type(cands[cand_idx]) == GAME_EVENT_EXCHANGE) {
        rack_take_letter(&leave,
                         get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile);
      }
    }
    double reply_sum = 0.0;
    double pass_reply_sum = 0.0;
    double after_followup_sum = 0.0;
    double pass_followup_sum = 0.0;
    int terminal_replies = 0;
    for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
      Game *after = game_duplicate(game);
      play_move(cands[cand_idx], after, NULL);
      rack_copy(player_get_rack(game_get_player(after, opponent_idx)),
                &racks[rack_idx]);
      ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                   NULL, false, 0);
      reply_sum += equity_to_double(ptg_best_placement_score(reply_list));
      pass_reply_sum += pass_scores[rack_idx];
      const Move *reply =
          move_list_get_count(reply_list) > 0 &&
                  move_get_type(move_list_get_move(reply_list, 0)) ==
                      GAME_EVENT_TILE_PLACEMENT_MOVE
              ? move_list_get_move(reply_list, 0)
              : pass_move;
      play_move(reply, after, NULL);
      terminal_replies += game_over(after);
      const int missing = RACK_SIZE - rack_get_total_letters(&leave);
      const int draw_count =
          missing < draw_sizes[rack_idx] ? missing : draw_sizes[rack_idx];
      for (int draw_no = 0; draw_no < settings->off_draws; draw_no++) {
        Rack *after_rack = player_get_rack(game_get_player(after, our_idx));
        Rack *base_rack =
            player_get_rack(game_get_player(pass_replied[rack_idx], our_idx));
        rack_copy(after_rack, &leave);
        rack_copy(base_rack, &leave);
        for (int draw_idx = 0; draw_idx < draw_count; draw_idx++) {
          const MachineLetter tile =
              draws[rack_idx]
                   [(draw_no * draw_count + draw_idx) % draw_sizes[rack_idx]];
          rack_add_letter(after_rack, tile);
          rack_add_letter(base_rack, tile);
        }
        if (!game_over(after)) {
          ptg_generate(after, reply_list, MOVE_RECORD_BEST, MOVE_SORT_SCORE, 0,
                       NULL, false, 0);
          after_followup_sum +=
              equity_to_double(ptg_best_placement_score(reply_list));
        }
        if (!game_over(pass_replied[rack_idx])) {
          ptg_generate(pass_replied[rack_idx], reply_list, MOVE_RECORD_BEST,
                       MOVE_SORT_SCORE, 0, NULL, false, 0);
          pass_followup_sum +=
              equity_to_double(ptg_best_placement_score(reply_list));
        }
      }
      game_destroy(after);
    }
    const double equity = equity_to_double(move_get_equity(cands[cand_idx]));
    const double blocking_delta = (pass_reply_sum - reply_sum) / num_racks;
    const double samples = (double)num_racks * settings->off_draws;
    const double setup_delta =
        (after_followup_sum - pass_followup_sum) / samples;
    threat[cand_idx] = equity + settings->weight * blocking_delta;
    setup[cand_idx] = equity + settings->off_w * setup_delta;
    if (check_out != NULL) {
      StringBuilder *move_text = string_builder_create();
      string_builder_add_ucgi_move(move_text, cands[cand_idx],
                                   game_get_board(game), game_get_ld(game));
      fprintf(
          check_out,
          "%ld,%d,%s,%.6f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%d,%d,%d\n",
          pos, cand_idx + 1, string_builder_peek(move_text), equity,
          pass_reply_sum / num_racks, reply_sum / num_racks, blocking_delta,
          2.0 * settings->weight * blocking_delta, pass_followup_sum / samples,
          after_followup_sum / samples, setup_delta,
          3.0 * settings->off_w * setup_delta, num_racks, conditioned,
          terminal_replies);
      string_builder_destroy(move_text);
    }
  }
  if (check_out != NULL) {
    fclose(check_out);
  }
  for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    game_destroy(pass_replied[rack_idx]);
  }
  move_destroy(pass_move);
}

void test_pat_candidate_diversity(void) {
  const char *generate_out = getenv("PCD_GENERATE_OUT");
  if (generate_out != NULL) {
    pcd_generate_positions(generate_out);
    return;
  }
  const char *opening_out = getenv("PCD_OPEN_OUT");
  if (opening_out != NULL) {
    pcd_write_openings(opening_out);
    return;
  }
  const bool adaptive = ptg_env_long("PCD_ADAPTIVE", 0) != 0;
  const char *in_path = getenv("PCD_IN");
  const char *out_path = getenv("PCD_OUT");
  assert(in_path != NULL && out_path != NULL);
  FILE *in = fopen(in_path, "r");
  FILE *out = fopen(out_path, "a+");
  assert(in != NULL && out != NULL);
  // Resume only fully finished positions. Partial trailing positions are
  // repeated; the analysis keeps the last completed arm's rows.
  bool done[10000] = {false};
  uint64_t reference_seen[10000][2] = {{0}};
  int expected[10000] = {0};
  char line[65536];
  rewind(out);
  while (fgets(line, sizeof(line), out)) {
    long pos;
    int bag;
    char arm[64];
    int rank;
    if (sscanf(line, "%ld,%d,%63[^,],%d", &pos, &bag, arm, &rank) == 4 &&
        pos >= 0 && pos < 10000 && rank >= 1 && rank <= PTG_MAX_K &&
        strcmp(arm, "reference") == 0) {
      char *field = line;
      for (int field_idx = 0; field_idx < 13 && field != NULL; field_idx++) {
        field = strchr(field, ',');
        if (field != NULL) {
          field++;
        }
      }
      if (field != NULL) {
        expected[pos] = (int)strtol(field, NULL, 10);
        reference_seen[pos][(rank - 1) / 64] |= UINT64_C(1)
                                                << ((rank - 1) % 64);
      }
    }
  }
  for (int pos = 0; pos < 10000; pos++) {
    int count = 0;
    for (int rank = 0; rank < PTG_MAX_K; rank++) {
      count += (reference_seen[pos][rank / 64] >> (rank % 64)) & UINT64_C(1);
    }
    done[pos] = expected[pos] > 0 && count == expected[pos];
  }
  fseek(out, 0, SEEK_END);
  if (ftell(out) == 0) {
    fprintf(out, "pos,bag,arm,static_rank,move,static_eq,threat_value,setup_"
                 "value,sim_eq,sim_eq_sem,sim_wp,sim_wp_sem,chosen,candidates,"
                 "wall_ms,iterations");
    if (adaptive) {
      fprintf(out, ",global_static_rank");
    }
    fputc('\n', out);
  }
  const char *feature_path = getenv("PCD_FEATURE_OUT");
  FILE *feature_out = feature_path != NULL ? fopen(feature_path, "w") : NULL;
  if (feature_path != NULL) {
    assert(feature_out != NULL);
    fprintf(feature_out, "pos,pool_index,global_static_rank,type,tiles_played,"
                         "score,static_eq,leave,move\n");
  }
  Config *config = config_create_or_die(
      "set -lex CSW24 -leaves CSW24 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1 -threads 1");
  ErrorStack *errors = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), "winpct_english", errors);
  assert(error_stack_is_empty(errors));
  MoveList *top = move_list_create(10000);
  MoveList *exchange_list = adaptive ? move_list_create(128) : NULL;
  MoveList *reply_list = move_list_create(1);
  SimResults *results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  PTGSettings settings = {.partition = true,
                          .r = (int)ptg_env_long("PCD_CHECK_R", 64),
                          .weight = 0.7,
                          .margin = 20.0,
                          .off_w = 0.25,
                          .off_base = 0.5,
                          .off_draws = 1,
                          .seq_min_racks = 6};
  assert(settings.r > 1 && settings.r <= PTG_MAX_R);
  const long worker = ptg_env_long("PCD_WORKER", 0);
  const long workers = ptg_env_long("PCD_NUM_WORKERS", 1);
  const long deadline = ptg_env_long("PCD_DEADLINE", 0);
  const int static_pool_cap =
      (int)ptg_env_long("PCD_POOL", PCD_STATIC_POOL_CAP);
  assert(static_pool_cap > 25 && static_pool_cap <= PTG_MAX_K - 8);
  long input_idx = 0;
  while (fgets(line, sizeof(line), in)) {
    if (deadline > 0 && (long)time(NULL) >= deadline) {
      break;
    }
    char *end = NULL;
    const long pos = strtol(line, &end, 10);
    if (end == line || *end != ',') {
      continue;
    }
    const bool assigned = input_idx++ % workers == worker;
    assert(pos >= 0 && pos < 10000);
    if (!assigned || done[pos]) {
      continue;
    }
    end[strcspn(end, "\r\n")] = '\0';
    char *command = get_formatted_string("cgp %s", end + 1);
    load_and_exec_config_or_die(config, command);
    free(command);
    Game *game = config_get_game(config);
    for (int seat = 0; seat < 2; seat++) {
      player_set_pat_usage(game_get_player(game, seat), true, 0);
    }
    ptg_generate(game, top, MOVE_RECORD_ALL, MOVE_SORT_EQUITY, 0, NULL, false,
                 0);
    move_list_sort_moves(top);
    const Move *cands[PTG_MAX_K];
    int global_ranks[PTG_MAX_K];
    bool quota[PTG_MAX_K] = {false};
    int count = 0;
    for (int idx = 0; idx < move_list_get_count(top) &&
                      count < (adaptive ? static_pool_cap : PTG_MAX_K);
         idx++) {
      const Move *move = move_list_get_move(top, idx);
      if (move_get_type(move) == GAME_EVENT_PASS) {
        break;
      }
      global_ranks[count] = idx + 1;
      cands[count++] = move;
    }
    const int static_core_count = count;
    if (adaptive) {
      pcd_add_exchange_quota(game, top, exchange_list, cands, &count,
                             global_ranks, quota);
      pcd_output_global_ranks = global_ranks;
    } else {
      pcd_output_global_ranks = NULL;
    }
    if (count < 2) {
      continue;
    }
    if (feature_out != NULL) {
      const int on_turn = game_get_player_on_turn_index(game);
      for (int idx = 0; idx < count; idx++) {
        if (ptg_env_long("PCD_FEATURE_DEBUG", 0) != 0) {
          fprintf(stderr,
                  "FEATURE pos=%ld rank=%d type=%d length=%d played=%d\n", pos,
                  idx + 1, move_get_type(cands[idx]),
                  move_get_tiles_length(cands[idx]),
                  move_get_tiles_played(cands[idx]));
        }
        Rack leave;
        rack_copy(&leave, player_get_rack(game_get_player(game, on_turn)));
        for (int tile_idx = 0; tile_idx < move_get_tiles_length(cands[idx]);
             tile_idx++) {
          const MachineLetter tile = move_get_tile(cands[idx], tile_idx);
          if (tile != PLAYED_THROUGH_MARKER ||
              move_get_type(cands[idx]) == GAME_EVENT_EXCHANGE) {
            rack_take_letter(&leave, get_is_blanked(tile) ? BLANK_MACHINE_LETTER
                                                          : tile);
          }
        }
        StringBuilder *leave_text = string_builder_create();
        StringBuilder *move_text = string_builder_create();
        string_builder_add_rack(leave_text, &leave, game_get_ld(game), false);
        string_builder_add_ucgi_move(move_text, cands[idx],
                                     game_get_board(game), game_get_ld(game));
        fprintf(feature_out, "%ld,%d,%d,%d,%d,%.6f,%.6f,%s,%s\n", pos, idx + 1,
                global_ranks[idx], move_get_type(cands[idx]),
                move_get_tiles_played(cands[idx]),
                equity_to_double(move_get_score(cands[idx])),
                equity_to_double(move_get_equity(cands[idx])),
                string_builder_peek(leave_text),
                string_builder_peek(move_text));
        string_builder_destroy(leave_text);
        string_builder_destroy(move_text);
      }
      fflush(feature_out);
    }
    if (ptg_env_long("PCD_FEATURE_ONLY", 0) != 0) {
      continue;
    }
    MachineLetter pool[PTG_POOL_CAP];
    int pool_size = 0;
    Rack racks[PTG_MAX_R];
    const uint64_t seed = ptg_next(&(uint64_t){
        (uint64_t)ptg_env_long("PCD_SEED", 20260929) + (uint64_t)pos});
    const int num_racks =
        ptg_sample_racks(game, &settings, seed, racks, pool, &pool_size);
    double threat[PTG_MAX_K];
    double setup[PTG_MAX_K];
    if ((ptg_env_long("PCD_TILE_ONLY", 0) != 0 &&
         ptg_env_long("PCD_PASS_RELATIVE", 0) == 0) ||
        ptg_env_long("PCD_MATCHED_ONLY", 0) != 0 ||
        getenv("PCD_ADMIT_RANKS") != NULL) {
      for (int idx = 0; idx < count; idx++) {
        threat[idx] = equity_to_double(move_get_equity(cands[idx]));
        setup[idx] = threat[idx];
      }
    } else if (ptg_env_long("PCD_PASS_RELATIVE", 0) != 0) {
      pcd_pass_relative_checks(game, &settings, cands, count, racks, num_racks,
                               reply_list, pool, pool_size, seed, pos, threat,
                               setup);
    } else {
      ptg_choose_sequential(game, &settings, cands, count, racks, num_racks,
                            reply_list, pool, pool_size, seed, true, threat,
                            setup);
    }
    if (getenv("PCD_ADMIT_RANKS") != NULL) {
      pcd_run_admission(game, win_pcts, cands, count, static_core_count, threat,
                        setup, pos, seed, &sim_ctx, results, out);
      fprintf(stderr, "PCD_ADMISSION_DONE pos=%ld\n", pos);
      continue;
    }
    if (ptg_env_long("PCD_MATCHED_ONLY", 0) != 0) {
      pcd_run_matched(game, win_pcts, cands, count, static_core_count, threat,
                      setup, pos, seed, &sim_ctx, results, out);
      fprintf(stderr, "PCD_MATCHED_DONE pos=%ld\n", pos);
      continue;
    }
    if (ptg_env_long("PCD_REF_ONLY", 0) != 0) {
      bool reference_selected[PTG_MAX_K] = {false};
      pcd_fill_static(reference_selected, count, count);
      pcd_sim_arm(
          game, win_pcts, cands, count, reference_selected, threat, setup, pos,
          "reference", (double)ptg_env_long("PCD_REF_MS", 60000) / 1000.0, true,
          seed ^ UINT64_C(0xd1b54a32d192ed03), &sim_ctx, results, NULL, out);
      fprintf(stderr, "PCD_REFERENCE_DONE pos=%ld\n", pos);
      fflush(stderr);
      continue;
    }
    const char *validation_ranks = getenv("PCD_VALIDATE_RANKS");
    if (validation_ranks != NULL) {
      bool validate[PTG_MAX_K] = {false};
      char rank_text[1024];
      snprintf(rank_text, sizeof(rank_text), "%s", validation_ranks);
      for (char *token = strtok(rank_text, ","); token != NULL;
           token = strtok(NULL, ",")) {
        const int rank = (int)strtol(token, NULL, 10);
        assert(rank > 0 && rank <= count);
        validate[rank - 1] = true;
      }
      pcd_sim_arm(
          game, win_pcts, cands, count, validate, threat, setup, pos,
          "reference", (double)ptg_env_long("PCD_REF_MS", 60000) / 1000.0, true,
          seed ^ UINT64_C(0xa24baed4963ee407), &sim_ctx, results, NULL, out);
      fprintf(stderr, "PCD_VALIDATE_DONE pos=%ld ranks=%s\n", pos,
              validation_ranks);
      fflush(stderr);
      continue;
    }
    if (adaptive && ptg_env_long("PCD_BLOCKING_ONLY", 0) != 0) {
      pcd_run_blocking(game, win_pcts, cands, count, static_core_count, threat,
                       setup, pos, seed, &sim_ctx, results, out);
      fprintf(stderr, "PCD_BLOCKING_DONE pos=%ld bag=%d pool=%d\n", pos,
              bag_get_letters(game_get_bag(game)), count);
      fflush(stderr);
      continue;
    }
    if (adaptive && ptg_env_long("PCD_EXPLORE", 0) != 0) {
      pcd_run_explore(game, win_pcts, cands, count, static_core_count, quota,
                      threat, setup, pos, seed, &sim_ctx, results, out);
      fprintf(stderr, "PCD_EXPLORE_DONE pos=%ld bag=%d pool=%d\n", pos,
              bag_get_letters(game_get_bag(game)), count);
      fflush(stderr);
      continue;
    }
    if (adaptive) {
      pcd_run_adaptive(game, win_pcts, cands, count, static_core_count, quota,
                       threat, setup, pos, seed, &sim_ctx, results, out);
      fprintf(stderr, "PCD_ADAPTIVE_DONE pos=%ld bag=%d pool=%d\n", pos,
              bag_get_letters(game_get_bag(game)), count);
      fflush(stderr);
      continue;
    }
    const char *names[] = {"static15",      "static25",  "threat15",
                           "setup15",       "diverse15", "augmented",
                           "staticmatched", "reference"};
    bool selected[8][PTG_MAX_K] = {{false}};
    pcd_fill_static(selected[0], count, 15);
    pcd_fill_static(selected[1], count, 25);
    for (int arm = 2; arm < 5; arm++) {
      pcd_fill_static(selected[arm], count, 5);
    }
    ptg_top_indices(threat, count, 10, selected[2]);
    ptg_top_indices(setup, count, 10, selected[3]);
    ptg_top_indices(threat, count, 5, selected[4]);
    ptg_top_indices(setup, count, 5, selected[4]);
    for (int arm = 2; arm < 5; arm++) {
      pcd_fill_static(selected[arm], count, 15);
    }
    pcd_fill_static(selected[5], count, 15);
    ptg_top_indices(threat, count, 15, selected[5]);
    ptg_top_indices(setup, count, 15, selected[5]);
    int augmented_count = 0;
    for (int idx = 0; idx < count; idx++) {
      augmented_count += selected[5][idx];
    }
    pcd_fill_static(selected[6], count, augmented_count);
    pcd_fill_static(selected[7], count, count);
    SimResults *arm_cache[7] = {NULL};
    // Rotate arm order across positions to distribute thermal/time effects.
    for (int step = 0; step < 7; step++) {
      const int arm = (step + (int)(pos % 7)) % 7;
      SimResults *reuse = NULL;
      for (int previous = 0; previous < 7; previous++) {
        if (arm_cache[previous] != NULL &&
            memcmp(selected[arm], selected[previous], sizeof(selected[arm])) ==
                0) {
          reuse = arm_cache[previous];
          break;
        }
      }
      pcd_sim_arm(game, win_pcts, cands, count, selected[arm], threat, setup,
                  pos, names[arm],
                  (double)ptg_env_long("PCD_MS", 15000) / 1000.0, false, seed,
                  &sim_ctx, results, reuse, out);
      arm_cache[arm] = sim_results_duplicate(reuse != NULL ? reuse : results);
    }
    pcd_sim_arm(game, win_pcts, cands, count, selected[7], threat, setup, pos,
                names[7], (double)ptg_env_long("PCD_REF_MS", 60000) / 1000.0,
                true, seed ^ UINT64_C(0xd1b54a32d192ed03), &sim_ctx, results,
                NULL, out);
    for (int arm = 0; arm < 7; arm++) {
      sim_results_destroy(arm_cache[arm]);
    }
    fprintf(stderr, "PCD_DONE pos=%ld bag=%d pool=%d\n", pos,
            bag_get_letters(game_get_bag(game)), count);
    fflush(stderr);
  }
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(results);
  move_list_destroy(reply_list);
  move_list_destroy(top);
  if (exchange_list != NULL) {
    move_list_destroy(exchange_list);
  }
  win_pct_destroy(win_pcts);
  error_stack_destroy(errors);
  config_destroy(config);
  pcd_output_global_ranks = NULL;
  if (feature_out != NULL) {
    fclose(feature_out);
  }
  fclose(in);
  fclose(out);
}
