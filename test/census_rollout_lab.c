#include "census_rollout_lab.h"

#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/static_eval_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/best_move_carry.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/census_rollout.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/random_variable.h"
#include "../src/util/io_util.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Per-query timers; -DCRL_NO_QUERY_TIMERS compiles them out for profiling.
static inline int64_t crl_query_ns(void) {
#ifdef CRL_NO_QUERY_TIMERS
  return 0;
#else
  return ctimer_monotonic_ns();
#endif
}

enum {
  CRL_NUM_LANES = CENSUS_ROLLOUT_NUM_LANES,
  // Room for every play of a rack, so that a lane's best is never cut off.
  CRL_ALL_MOVES_CAPACITY = 1 << 17,
  CRL_MAX_WORKERS = 512,
};

// No play: below every equity a move has.
#define CRL_NO_PLAY CENSUS_ROLLOUT_NO_PLAY

const int crl_topk_values[CRL_NUM_TOPK] = {1, 2, 4, 8, CRL_NUM_LANES};

int crl_carry_buckets = 0;

typedef struct CrlVariantRecord {
  uint64_t skip_lanes;
  Equity floor_equity;
  bool use_floor_equity;
  bool hit;
  bool has_anchor_skips;
  uint16_t skip_anchor_cols[CRL_NUM_LANES];
} CrlVariantRecord;

// A board's best equity for the rack in each lane and in each anchor of it,
// CRL_NO_PLAY where there is no play, and each lane's anchor columns.
typedef struct CrlBoardValues {
  Equity lanes[CRL_NUM_LANES];
  Equity anchors[CRL_NUM_LANES][BOARD_DIM];
  uint16_t anchor_cols[CRL_NUM_LANES];
} CrlBoardValues;

typedef struct CrlRecord {
  uint64_t move_signature;
  bool eligible;
  CrlVariantRecord variants[CRL_NUM_VARIANTS];
} CrlRecord;

typedef struct CensusRolloutLab {
  crl_mode_t mode;
  int variant;
  bool replay_with_carry;
  int replay_parts;
  bool root_is_set;
  Game *boards[CRL_NUM_BOARDS];
  uint64_t board_hashes[CRL_NUM_BOARDS][CRL_NUM_LANES];
  MoveList *all_moves;
  CrlRecord *records;
  uint64_t number_of_records;
  uint64_t records_capacity;
  uint64_t next_record;
  // crl_get_ply_stats's sums over the workers.
  CrlPlyStats summed[CRL_MAX_PLIES];
  // The lazy mode's options, and its census, made with census_made_with.
  CensusRolloutOptions census_options;
  CensusRollout *census;
  CensusRolloutOptions census_made_with;
  bool verify;
  CrlLazyStats lazy_stats;
} CensusRolloutLab;

static CensusRolloutLab lab = {
    .census_options = {.max_ply = CENSUS_ROLLOUT_DEFAULT_MAX_PLY,
                       .low_bag = false,
                       .with_carry = true,
                       .build_threads = 1}};

crl_mode_t crl_get_mode(void) { return lab.mode; }

void crl_set_replay_parts(int parts) { lab.replay_parts = parts; }

void crl_new_sim(void) {
  lab.root_is_set = false;
  lab.next_record = 0;
}

void crl_clear_records(void) {
  lab.number_of_records = 0;
  lab.next_record = 0;
}

static void crl_workers_reset_stats(void);
static void crl_workers_sum_ply(int ply, CrlPlyStats *sum);

void crl_reset_stats(void) {
  crl_workers_reset_stats();
  memset(&lab.lazy_stats, 0, sizeof(lab.lazy_stats));
  if (lab.census != NULL) {
    census_rollout_reset_build_stats(lab.census);
  }
}

const CrlPlyStats *crl_get_ply_stats(int ply) {
  crl_workers_sum_ply(ply, &lab.summed[ply]);
  return &lab.summed[ply];
}

uint64_t crl_get_number_of_records(void) { return lab.number_of_records; }

static void crl_copy_board(int board, const Game *game) {
  if (lab.boards[board] == NULL) {
    lab.boards[board] = game_duplicate(game);
  } else {
    game_copy(lab.boards[board], game);
  }
  gen_hash_game_lanes(game, 0, lab.board_hashes[board]);
}

// A sim worker's state in the lab: what its rollouts count, so that workers
// on different threads share nothing they write, and its census worker.
typedef struct CrlWorker {
  CrlPlyStats stats[CRL_MAX_PLIES];
  CensusRolloutWorker *census_worker;
  // Verifying: every play, and the census-steered move.
  MoveList *all_moves;
  Move census_move;
  CrlBoardValues every_play;
  CrlBoardValues multi_tile;
} CrlWorker;

// Recording and replaying are single-threaded: they walk one list of
// generations in order.
static void crl_before_candidate(void *worker, const Game *game) {
  (void)worker;
  if (lab.mode == CRL_MODE_RECORD && !lab.root_is_set) {
    crl_copy_board(CRL_BOARD_ROOT, game);
    lab.root_is_set = true;
  }
}

static void crl_after_candidate(void *worker, const Game *game,
                                const Move *candidate) {
  if (lab.mode == CRL_MODE_RECORD) {
    crl_copy_board(CRL_BOARD_CAND, game);
  }
  if (lab.mode == CRL_MODE_LAZY) {
    census_rollout_worker_set_candidate(((CrlWorker *)worker)->census_worker,
                                        lab.census, candidate);
  }
}

static uint64_t crl_move_signature(const Move *move) {
  return census_rollout_move_signature(move);
}

// The lanes of a tile placement: the lane it is played in and, for a play of
// one tile, the other lane through that tile, since the generator finds it
// in either.
static uint64_t crl_move_lanes(const Move *move) {
  const int dir = move_get_dir(move);
  const bool vertical = board_is_dir_vertical(dir);
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  uint64_t lanes = (uint64_t)1
                   << (BOARD_DIM * dir + (vertical ? col_start : row_start));
  if (move_get_tiles_played(move) == 1) {
    const int length = move_get_tiles_length(move);
    for (int tile_idx = 0; tile_idx < length; tile_idx++) {
      if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
        continue;
      }
      const int row = row_start + (vertical ? tile_idx : 0);
      const int col = col_start + (vertical ? 0 : tile_idx);
      lanes |=
          (uint64_t)1 << (vertical
                              ? BOARD_DIM * BOARD_HORIZONTAL_DIRECTION + row
                              : BOARD_DIM * BOARD_VERTICAL_DIRECTION + col);
    }
  }
  return lanes;
}

static const Square *crl_lane_squares(const Game *game, int lane) {
  return census_rollout_lane_squares(game, lane);
}

static bool crl_square_is_empty(const Square *square) {
  return square_get_letter(square) == ALPHABET_EMPTY_SQUARE_MARKER;
}

static uint16_t crl_anchor_cols(const Square *squares) {
  return census_rollout_anchor_cols(squares);
}

static uint16_t crl_changed_cols(const Square *now, const Square *then) {
  return census_rollout_changed_cols(now, then);
}

static uint16_t crl_cols_between(int low, int high) {
  return (uint16_t)((((uint32_t)1 << (high + 1)) - 1) &
                    ~(((uint32_t)1 << low) - 1));
}

static void crl_anchor_zones(const Square *squares, uint16_t anchor_cols,
                             int rack_tiles, uint16_t *zones) {
  census_rollout_anchor_zones(squares, anchor_cols, rack_tiles, zones);
}

// The anchor whose plays hold a play spanning start to end of a lane: the
// leftmost anchor it covers, since the generator searches each anchor's plays
// no further left than the anchor before it. -1 if it covers none.
static int crl_anchor_of_span(uint16_t anchor_cols, int start, int end) {
  const uint32_t held = anchor_cols & crl_cols_between(start, end);
  return held != 0 ? __builtin_ctz(held) : -1;
}

static void crl_move_span(const Move *move, int *lane, int *start, int *end) {
  const int dir = move_get_dir(move);
  const bool vertical = board_is_dir_vertical(dir);
  *lane = BOARD_DIM * dir +
          (vertical ? move_get_col_start(move) : move_get_row_start(move));
  *start = vertical ? move_get_row_start(move) : move_get_col_start(move);
  *end = *start + move_get_tiles_length(move) - 1;
}

// A play of one tile in the other lane through its tile: the lane, and the
// run of the board's letters it joins there.
static void crl_single_tile_cross_span(const Game *game, const Move *move,
                                       int *lane, int *start, int *end) {
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int length = move_get_tiles_length(move);
  int tile_idx = 0;
  while (tile_idx < length - 1 &&
         move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
    tile_idx++;
  }
  const int row = move_get_row_start(move) + (vertical ? tile_idx : 0);
  const int col = move_get_col_start(move) + (vertical ? 0 : tile_idx);
  *lane = vertical ? BOARD_DIM * BOARD_HORIZONTAL_DIRECTION + row
                   : BOARD_DIM * BOARD_VERTICAL_DIRECTION + col;
  const int position = vertical ? col : row;
  const Square *squares = crl_lane_squares(game, *lane);
  *start = position;
  while (*start > 0 && !crl_square_is_empty(&squares[*start - 1])) {
    (*start)--;
  }
  *end = position;
  while (*end < BOARD_DIM - 1 && !crl_square_is_empty(&squares[*end + 1])) {
    (*end)++;
  }
}

static void crl_raise(Equity *best, Equity value) {
  if (value > *best) {
    *best = value;
  }
}

// Every lane's and anchor's best equity for the rack of the player on turn.
// A play of one tile counts in both of its lanes, since the generator finds
// it in either. Returns false if the plays did not all fit.
static bool crl_board_values_of(Game *game, MoveList *all_moves,
                                CrlBoardValues *values, bool multi_tile_only);

static bool crl_board_values(Game *game, CrlBoardValues *values) {
  return crl_board_values_of(game, lab.all_moves, values, false);
}

// crl_board_values into all_moves, counting with multi_tile_only only the
// plays of two or more tiles, whose lanes and anchors are not in doubt.
static bool crl_board_values_of(Game *game, MoveList *all_moves,
                                CrlBoardValues *values, bool multi_tile_only) {
  const MoveGenArgs args = {.game = game,
                            .move_list = all_moves,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    values->lanes[lane] = CRL_NO_PLAY;
    for (int col = 0; col < BOARD_DIM; col++) {
      values->anchors[lane][col] = CRL_NO_PLAY;
    }
    values->anchor_cols[lane] = crl_anchor_cols(crl_lane_squares(game, lane));
  }
  const int count = move_list_get_count(all_moves);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const Move *move = move_list_get_move(all_moves, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
        (multi_tile_only && move_get_tiles_played(move) < 2)) {
      continue;
    }
    const Equity equity = move_get_equity(move);
    int lane;
    int start;
    int end;
    crl_move_span(move, &lane, &start, &end);
    crl_raise(&values->lanes[lane], equity);
    int anchor = crl_anchor_of_span(values->anchor_cols[lane], start, end);
    if (anchor >= 0) {
      crl_raise(&values->anchors[lane][anchor], equity);
    }
    if (move_get_tiles_played(move) == 1) {
      crl_single_tile_cross_span(game, move, &lane, &start, &end);
      crl_raise(&values->lanes[lane], equity);
      anchor = crl_anchor_of_span(values->anchor_cols[lane], start, end);
      if (anchor >= 0) {
        crl_raise(&values->anchors[lane][anchor], equity);
      }
    }
  }
  return count < CRL_ALL_MOVES_CAPACITY;
}

// The census board's rack position: the player on turn holds rack.
static void crl_set_board_rack(Game *board_game, int player_index,
                               const Rack *rack) {
  game_set_player_on_turn_index(board_game, player_index);
  rack_copy(player_get_rack(game_get_player(board_game, player_index)), rack);
}

static bool crl_is_eligible(const Game *game) {
  return bag_get_letters(game_get_bag(game)) >= PEG_ADJUST_VALUES_LENGTH &&
         board_get_tiles_played(game_get_board(game)) > 0;
}

// What a cache of the census board's k best lanes for the rack gives a
// generation on the current board: lane_bests are the census board's,
// clean the lanes with anchors that are as they were on it, and anchored
// every lane with anchors now.
static CrlVariantRecord crl_variant_for(const Equity *lane_bests, int topk,
                                        uint64_t clean, uint64_t anchored) {
  int order[CRL_NUM_LANES];
  int number_of_lanes = 0;
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    if (lane_bests[lane] != CRL_NO_PLAY) {
      order[number_of_lanes++] = lane;
    }
  }
  // Insertion sort, best first; equal lanes by lane index.
  for (int idx = 1; idx < number_of_lanes; idx++) {
    const int lane = order[idx];
    int pos = idx;
    while (pos > 0 && lane_bests[order[pos - 1]] < lane_bests[lane]) {
      order[pos] = order[pos - 1];
      pos--;
    }
    order[pos] = lane;
  }
  const int listed = topk < number_of_lanes ? topk : number_of_lanes;
  // Whether the list holds every lane with a play: then a lane it leaves out
  // has none.
  const bool complete = listed == number_of_lanes;
  uint64_t listed_lanes = 0;
  int floor_lane = -1;
  for (int idx = 0; idx < listed; idx++) {
    listed_lanes |= (uint64_t)1 << order[idx];
    if (floor_lane < 0 && (clean & ((uint64_t)1 << order[idx]))) {
      floor_lane = order[idx];
    }
  }
  CrlVariantRecord record = {0};
  if (floor_lane < 0) {
    if (!complete) {
      // Every listed lane changed, and the cache knows nothing of the rest.
      return record;
    }
    // No clean lane has a play: every one of them can be skipped.
    record.hit = true;
    record.skip_lanes = clean & anchored;
    return record;
  }
  const Equity floor_equity = lane_bests[floor_lane];
  const Equity last_listed = lane_bests[order[listed - 1]];
  uint64_t skip = 0;
  for (uint64_t lanes = clean & anchored; lanes != 0; lanes &= lanes - 1) {
    const int lane = __builtin_ctzll(lanes);
    if (listed_lanes & ((uint64_t)1 << lane)) {
      if (lane_bests[lane] < floor_equity) {
        skip |= (uint64_t)1 << lane;
      }
    } else if (complete || last_listed < floor_equity) {
      skip |= (uint64_t)1 << lane;
    }
  }
  record.hit = true;
  record.skip_lanes = skip;
  record.floor_equity = floor_equity;
  record.use_floor_equity = true;
  return record;
}

static CrlRecord *crl_append_record(void) {
  if (lab.number_of_records == lab.records_capacity) {
    lab.records_capacity =
        lab.records_capacity == 0 ? 1 << 16 : lab.records_capacity * 2;
    lab.records =
        realloc_or_die(lab.records, sizeof(CrlRecord) * lab.records_capacity);
  }
  CrlRecord *record = &lab.records[lab.number_of_records++];
  memset(record, 0, sizeof(*record));
  return record;
}

// What a cache of every anchor's best on the census board gives a generation
// on the current board: the anchors of lanes that are as they were, and the
// anchors of changed lanes whose zones on both boards miss every changed
// square, hold the same plays on both, so their bests are known. The best of
// them is a floor some searched anchor reaches; every known anchor below it
// is skipped, and a lane all of whose anchors are is skipped whole. known
// receives the known anchors by lane.
static CrlVariantRecord crl_anchor_variant_for(
    const CrlBoardValues *census, const CrlBoardValues *now, uint64_t clean,
    const uint16_t *changed, const uint16_t (*zones_now)[BOARD_DIM],
    const uint16_t (*zones_census)[BOARD_DIM], uint16_t *known,
    uint64_t *clean_anchors_in_dirty_lanes, uint64_t *violations) {
  CrlVariantRecord record = {0};
  Equity floor_equity = CRL_NO_PLAY;
  bool any_known = false;
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    known[lane] = 0;
    const uint16_t anchors = now->anchor_cols[lane];
    if (anchors == 0) {
      continue;
    }
    if (clean & ((uint64_t)1 << lane)) {
      known[lane] = anchors;
    } else {
      for (uint32_t remaining = anchors & census->anchor_cols[lane];
           remaining != 0; remaining &= remaining - 1) {
        const int col = __builtin_ctz(remaining);
        if ((zones_now[lane][col] & changed[lane]) == 0 &&
            (zones_census[lane][col] & changed[lane]) == 0) {
          known[lane] |= (uint16_t)(1U << col);
        }
      }
      *clean_anchors_in_dirty_lanes +=
          (uint64_t)__builtin_popcount(known[lane]);
    }
    for (uint32_t remaining = known[lane]; remaining != 0;
         remaining &= remaining - 1) {
      const int col = __builtin_ctz(remaining);
      if (census->anchors[lane][col] != now->anchors[lane][col]) {
        (*violations)++;
      }
      any_known = true;
      crl_raise(&floor_equity, census->anchors[lane][col]);
    }
  }
  if (!any_known) {
    return record;
  }
  record.hit = true;
  record.has_anchor_skips = true;
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    uint16_t skip = 0;
    for (uint32_t remaining = known[lane]; remaining != 0;
         remaining &= remaining - 1) {
      const int col = __builtin_ctz(remaining);
      const Equity value = census->anchors[lane][col];
      if (value == CRL_NO_PLAY || value < floor_equity) {
        skip |= (uint16_t)(1U << col);
      }
    }
    if (skip != 0 && skip == now->anchor_cols[lane]) {
      record.skip_lanes |= (uint64_t)1 << lane;
    } else {
      record.skip_anchor_cols[lane] = skip;
    }
  }
  return record;
}

// The anchors a variant's generation searches: those of the lanes it does not
// skip, less the anchors it skips.
static uint64_t crl_searched_anchors(const CrlVariantRecord *record,
                                     const CrlBoardValues *now) {
  uint64_t searched = 0;
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    if (record->hit && (record->skip_lanes & ((uint64_t)1 << lane))) {
      continue;
    }
    uint16_t anchors = now->anchor_cols[lane];
    if (record->hit && record->has_anchor_skips) {
      anchors &= (uint16_t)~record->skip_anchor_cols[lane];
    }
    searched += (uint64_t)__builtin_popcount(anchors);
  }
  return searched;
}

static const Move *crl_record_query(Game *game, MoveList *move_list,
                                    CrlPlyStats *stats) {
  CrlRecord *record = crl_append_record();
  bool eligible = crl_is_eligible(game);
  CrlBoardValues now;
  CrlBoardValues census[CRL_NUM_BOARDS];
  uint64_t current_hashes[CRL_NUM_LANES];
  uint64_t clean[CRL_NUM_BOARDS] = {0};
  uint16_t changed[CRL_NUM_BOARDS][CRL_NUM_LANES] = {{0}};
  uint16_t known[CRL_NUM_BOARDS][CRL_NUM_LANES] = {{0}};
  if (eligible) {
    const int64_t hash_start = crl_query_ns();
    gen_hash_game_lanes(game, 0, current_hashes);
    stats->hash_ns += crl_query_ns() - hash_start;
    if (!crl_board_values(game, &now)) {
      stats->incomplete++;
      eligible = false;
    }
  }
  const int player_index = game_get_player_on_turn_index(game);
  const Rack *rack = player_get_rack(game_get_player(game, player_index));
  if (eligible) {
    for (int board = 0; board < CRL_NUM_BOARDS; board++) {
      crl_set_board_rack(lab.boards[board], player_index, rack);
      if (!crl_board_values(lab.boards[board], &census[board])) {
        stats->incomplete++;
        eligible = false;
      }
    }
  }
  if (eligible) {
    stats->eligible++;
    const int rack_tiles = rack_get_total_letters(rack);
    uint64_t anchored = 0;
    uint16_t zones_now[CRL_NUM_LANES][BOARD_DIM];
    uint16_t zones_census[CRL_NUM_LANES][BOARD_DIM];
    for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
      if (current_hashes[lane] != 0) {
        anchored |= (uint64_t)1 << lane;
      }
      stats->anchors += (uint64_t)__builtin_popcount(now.anchor_cols[lane]);
      crl_anchor_zones(crl_lane_squares(game, lane), now.anchor_cols[lane],
                       rack_tiles, zones_now[lane]);
    }
    stats->lanes_with_anchors += (uint64_t)__builtin_popcountll(anchored);
    for (int board = 0; board < CRL_NUM_BOARDS; board++) {
      for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
        const Square *census_squares =
            crl_lane_squares(lab.boards[board], lane);
        changed[board][lane] =
            crl_changed_cols(crl_lane_squares(game, lane), census_squares);
        crl_anchor_zones(census_squares, census[board].anchor_cols[lane],
                         rack_tiles, zones_census[lane]);
        if (current_hashes[lane] != 0 &&
            current_hashes[lane] == lab.board_hashes[board][lane]) {
          clean[board] |= (uint64_t)1 << lane;
          if (census[board].lanes[lane] != now.lanes[lane]) {
            stats->self_check_violations[board]++;
          }
        }
      }
      stats->dirty_lanes[board] +=
          (uint64_t)__builtin_popcountll(anchored & ~clean[board]);
      for (int topk_idx = 0; topk_idx < CRL_NUM_TOPK; topk_idx++) {
        const int variant = crl_variant(board, topk_idx);
        record->variants[variant] =
            crl_variant_for(census[board].lanes, crl_topk_values[topk_idx],
                            clean[board], anchored);
      }
      record->variants[crl_anchor_variant(board)] = crl_anchor_variant_for(
          &census[board], &now, clean[board], changed[board], zones_now,
          zones_census, known[board],
          &stats->clean_anchors_in_dirty_lanes[board],
          &stats->anchor_self_check_violations[board]);
    }
    for (int variant = 0; variant < CRL_NUM_VARIANTS; variant++) {
      const CrlVariantRecord *variant_record = &record->variants[variant];
      if (variant_record->hit) {
        stats->variant_hits[variant]++;
        stats->variant_searched_lanes[variant] +=
            (uint64_t)__builtin_popcountll(anchored &
                                           ~variant_record->skip_lanes);
      } else {
        stats->variant_searched_lanes[variant] +=
            (uint64_t)__builtin_popcountll(anchored);
      }
      stats->variant_searched_anchors[variant] +=
          crl_searched_anchors(variant_record, &now);
    }
  }
  record->eligible = eligible;
  const Move *best = get_top_equity_move_with_carry(game, move_list, NULL);
  record->move_signature = crl_move_signature(best);
  if (eligible && move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    const uint64_t best_lanes = crl_move_lanes(best);
    int lane;
    int start;
    int end;
    crl_move_span(best, &lane, &start, &end);
    const int anchor = crl_anchor_of_span(now.anchor_cols[lane], start, end);
    int cross_lane = -1;
    int cross_anchor = -1;
    if (move_get_tiles_played(best) == 1) {
      int cross_start;
      int cross_end;
      crl_single_tile_cross_span(game, best, &cross_lane, &cross_start,
                                 &cross_end);
      cross_anchor = crl_anchor_of_span(now.anchor_cols[cross_lane],
                                        cross_start, cross_end);
    }
    const uint16_t touch_zone =
        crl_cols_between(start > 0 ? start - 1 : 0,
                         end < BOARD_DIM - 1 ? end + 1 : BOARD_DIM - 1);
    for (int board = 0; board < CRL_NUM_BOARDS; board++) {
      if (best_lanes & clean[board]) {
        stats->best_in_clean_lane[board]++;
      }
      if ((anchor >= 0 && (known[board][lane] & (1U << anchor))) ||
          (cross_anchor >= 0 &&
           (known[board][cross_lane] & (1U << cross_anchor)))) {
        stats->best_in_known_anchor[board]++;
      }
      if ((changed[board][lane] & touch_zone) == 0) {
        stats->best_untouched[board]++;
      }
    }
  }
  return best;
}

static const Move *crl_timed_generation(Game *game, MoveList *move_list,
                                        BestMoveCarry *carry,
                                        const CrlVariantRecord *variant,
                                        CrlPlyStats *stats) {
  MoveGenArgs args = {.game = game,
                      .move_list = move_list,
                      .move_record_type = MOVE_RECORD_BEST,
                      .move_sort_type = MOVE_SORT_EQUITY,
                      .override_kwg = NULL,
                      .eq_margin_movegen = 0,
                      .target_equity = EQUITY_MAX_VALUE,
                      .best_move_carry = carry};
  if (variant != NULL) {
    if (lab.replay_parts & CRL_REPLAY_SKIPS) {
      args.skip_lanes = variant->skip_lanes;
      if (variant->has_anchor_skips) {
        args.skip_anchor_cols = variant->skip_anchor_cols;
      }
    }
    if (lab.replay_parts & CRL_REPLAY_FLOOR) {
      args.use_floor_equity = variant->use_floor_equity;
      args.floor_equity = variant->floor_equity;
    }
  }
  const int64_t start = crl_query_ns();
  generate_moves(&args);
  stats->generation_ns += crl_query_ns() - start;
  return move_list_get_move(move_list, 0);
}

// ---- The workers ----

// Every worker, for summing and resetting their stats. Workers are created
// and destroyed by the threads that start and end sims, which may be several
// (autoplay's sims), so the list has a lock.
static CrlWorker *crl_workers[CRL_MAX_WORKERS];
static int crl_number_of_workers;
// The stats of workers already destroyed: a sim's workers may end with it.
static CrlPlyStats crl_retired_stats[CRL_MAX_PLIES];
static uint64_t crl_retired_root_hits;
static uint64_t crl_retired_root_lookups;
static atomic_flag crl_workers_lock = ATOMIC_FLAG_INIT;

static void crl_workers_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&crl_workers_lock,
                                           memory_order_acquire)) {
  }
}

static void crl_workers_release(void) {
  atomic_flag_clear_explicit(&crl_workers_lock, memory_order_release);
}

static void *crl_worker_create(void *context) {
  (void)context;
  CrlWorker *worker = calloc_or_die(1, sizeof(CrlWorker));
  worker->census_worker = census_rollout_worker_create();
  crl_workers_acquire();
  if (crl_number_of_workers < CRL_MAX_WORKERS) {
    crl_workers[crl_number_of_workers++] = worker;
  }
  crl_workers_release();
  return worker;
}

static_assert(sizeof(CrlPlyStats) % sizeof(uint64_t) == 0,
              "CrlPlyStats holds only 64-bit counts");

// Adds stats into sum: every field of CrlPlyStats is a 64-bit count.
static void crl_add_ply_stats(CrlPlyStats *sum, const CrlPlyStats *stats) {
  uint64_t *sum_words = (uint64_t *)sum;
  const uint64_t *stats_words = (const uint64_t *)stats;
  for (size_t word = 0; word < sizeof(CrlPlyStats) / sizeof(uint64_t); word++) {
    sum_words[word] += stats_words[word];
  }
}

static void crl_worker_destroy(void *context, void *worker_pointer) {
  (void)context;
  CrlWorker *worker = worker_pointer;
  if (worker == NULL) {
    return;
  }
  crl_workers_acquire();
  for (int ply = 0; ply < CRL_MAX_PLIES; ply++) {
    crl_add_ply_stats(&crl_retired_stats[ply], &worker->stats[ply]);
  }
  crl_retired_root_hits +=
      census_rollout_worker_get_root_hits(worker->census_worker);
  crl_retired_root_lookups +=
      census_rollout_worker_get_root_lookups(worker->census_worker);
  for (int worker_idx = 0; worker_idx < crl_number_of_workers; worker_idx++) {
    if (crl_workers[worker_idx] == worker) {
      crl_workers[worker_idx] = crl_workers[--crl_number_of_workers];
      break;
    }
  }
  crl_workers_release();
  census_rollout_worker_destroy(worker->census_worker);
  move_list_destroy(worker->all_moves);
  free(worker);
}

static void crl_workers_reset_stats(void) {
  crl_workers_acquire();
  memset(crl_retired_stats, 0, sizeof(crl_retired_stats));
  crl_retired_root_hits = 0;
  crl_retired_root_lookups = 0;
  for (int worker_idx = 0; worker_idx < crl_number_of_workers; worker_idx++) {
    memset(crl_workers[worker_idx]->stats, 0,
           sizeof(crl_workers[worker_idx]->stats));
    census_rollout_worker_reset_counts(crl_workers[worker_idx]->census_worker);
  }
  crl_workers_release();
}

static void crl_workers_sum_ply(int ply, CrlPlyStats *sum) {
  crl_workers_acquire();
  *sum = crl_retired_stats[ply];
  for (int worker_idx = 0; worker_idx < crl_number_of_workers; worker_idx++) {
    crl_add_ply_stats(sum, &crl_workers[worker_idx]->stats[ply]);
  }
  crl_workers_release();
}

// ---- The lazy mode: the census rollout, timed and verified ----

void crl_set_lazy_options(int max_ply, bool verify, bool low_bag) {
  lab.census_options.max_ply = max_ply;
  lab.census_options.low_bag = low_bag;
  lab.verify = verify;
}

void crl_set_lazy_carry(bool with_carry) {
  lab.census_options.with_carry = with_carry;
}

void crl_set_build_threads(int build_threads) {
  lab.census_options.build_threads = build_threads;
}

const CrlLazyStats *crl_get_lazy_stats(void) {
  if (lab.census != NULL) {
    const CensusRolloutBuildStats *build_stats =
        census_rollout_get_build_stats(lab.census);
    lab.lazy_stats.candidate_builds = build_stats->candidate_builds;
    lab.lazy_stats.root_entries = build_stats->root_entries;
    lab.lazy_stats.candidate_entries = build_stats->candidate_entries;
  }
  crl_workers_acquire();
  lab.lazy_stats.root_gather_hits = crl_retired_root_hits;
  lab.lazy_stats.root_gather_lookups = crl_retired_root_lookups;
  for (int worker_idx = 0; worker_idx < crl_number_of_workers; worker_idx++) {
    const CensusRolloutWorker *census_worker =
        crl_workers[worker_idx]->census_worker;
    lab.lazy_stats.root_gather_hits +=
        census_rollout_worker_get_root_hits(census_worker);
    lab.lazy_stats.root_gather_lookups +=
        census_rollout_worker_get_root_lookups(census_worker);
  }
  crl_workers_release();
  return &lab.lazy_stats;
}

static bool crl_census_options_equal(const CensusRolloutOptions *a,
                                     const CensusRolloutOptions *b) {
  return a->max_ply == b->max_ply && a->low_bag == b->low_bag &&
         a->with_carry == b->with_carry && a->build_threads == b->build_threads;
}

static void crl_prepare(void *context, const Game *game,
                        const MoveList *candidates) {
  (void)context;
  if (lab.mode != CRL_MODE_LAZY) {
    return;
  }
  // A census keeps the options it was made with.
  if (lab.census != NULL &&
      !crl_census_options_equal(&lab.census_made_with, &lab.census_options)) {
    census_rollout_destroy(lab.census);
    lab.census = NULL;
  }
  if (lab.census == NULL) {
    lab.census = census_rollout_create(&lab.census_options);
    lab.census_made_with = lab.census_options;
  }
  const int64_t start = ctimer_monotonic_ns();
  census_rollout_prepare(lab.census, game, candidates);
  lab.lazy_stats.build_ns += ctimer_monotonic_ns() - start;
}

// The best of every play the generator finds, by compare_moves: the move a
// best-move generation must return if its pruning is sound.
static uint64_t crl_exhaustive_best_signature(Game *game, MoveList *all_moves) {
  const MoveGenArgs args = {.game = game,
                            .move_list = all_moves,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE};
  generate_moves(&args);
  const int count = move_list_get_count(all_moves);
  const Move *best = NULL;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const Move *move = move_list_get_move(all_moves, move_idx);
    if (best == NULL || compare_moves(move, best, true) == 1) {
      best = move;
    }
  }
  return best != NULL ? crl_move_signature(best) : 0;
}

static const Move *crl_lazy_query(CrlWorker *worker, Game *game,
                                  MoveList *move_list, BestMoveCarry *carry,
                                  int ply, CrlPlyStats *stats) {
  const int64_t start = crl_query_ns();
  const Move *best = census_rollout_get_best_move(
      worker->census_worker, lab.census, game, move_list, carry, ply);
  stats->generation_ns += crl_query_ns() - start;
  if (census_rollout_worker_last_served(worker->census_worker)) {
    stats->eligible++;
  }
  const bool steered =
      census_rollout_worker_last_steered(worker->census_worker);
  if (steered) {
    stats->replay_hits++;
  }
  if (!lab.verify) {
    return best;
  }
  if (worker->all_moves == NULL) {
    worker->all_moves = move_list_create(CRL_ALL_MOVES_CAPACITY);
  }
  move_copy(&worker->census_move, best);
  if (steered) {
    // Every known anchor's census value against every play now: at least
    // its best play of two or more tiles, at most its best play (a play of
    // one tile is in both of its lanes for the census, in one for the
    // generator).
    crl_board_values_of(game, worker->all_moves, &worker->every_play, false);
    crl_board_values_of(game, worker->all_moves, &worker->multi_tile, true);
    for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
      for (uint32_t remaining = census_rollout_worker_get_known_anchors(
               worker->census_worker, lane);
           remaining != 0; remaining &= remaining - 1) {
        const int col = __builtin_ctz(remaining);
        const Equity value = census_rollout_worker_get_known_value(
            worker->census_worker, lane, col);
        stats->anchors_checked++;
        if (value < worker->multi_tile.anchors[lane][col] ||
            value > worker->every_play.anchors[lane][col]) {
          stats->anchor_value_violations++;
        }
      }
    }
  }
  const uint64_t census_signature = crl_move_signature(&worker->census_move);
  const uint64_t exhaustive_signature =
      crl_exhaustive_best_signature(game, worker->all_moves);
  const Move *plain = get_top_equity_move_with_carry(game, move_list, NULL);
  const uint64_t plain_signature = crl_move_signature(plain);
  if (plain_signature != census_signature) {
    stats->mismatches++;
  }
  if (plain_signature != exhaustive_signature) {
    stats->plain_vs_exhaustive++;
  }
  if (census_signature != exhaustive_signature) {
    stats->census_vs_exhaustive++;
  }
  return plain;
}

// ---- The hooks ----

static const Move *crl_best_move(void *worker_pointer, Game *game,
                                 MoveList *move_list, BestMoveCarry *carry,
                                 int ply) {
  CrlWorker *worker = worker_pointer;
  CrlPlyStats *stats =
      &worker->stats[ply < CRL_MAX_PLIES ? ply : CRL_MAX_PLIES - 1];
  stats->queries++;
  if (lab.mode == CRL_MODE_OFF) {
    // The hooks are set only for the carry's buckets.
    return get_top_equity_move_with_carry(game, move_list, carry);
  }
  if (lab.mode == CRL_MODE_RECORD) {
    return crl_record_query(game, move_list, stats);
  }
  if (lab.mode == CRL_MODE_LAZY) {
    return crl_lazy_query(worker, game, move_list, carry, ply, stats);
  }
  // Records are walked in order, so only a single-threaded sim reads them.
  const CrlRecord *record = NULL;
  if (lab.number_of_records > 0) {
    if (lab.next_record < lab.number_of_records) {
      record = &lab.records[lab.next_record];
    }
    lab.next_record++;
  }
  const Move *best = NULL;
  if (lab.mode == CRL_MODE_REPLAY) {
    if (record == NULL) {
      log_fatal("census rollout lab: more generations than were recorded");
      return NULL;
    }
    const CrlVariantRecord *variant = NULL;
    if (record->eligible) {
      stats->eligible++;
      if (record->variants[lab.variant].hit) {
        variant = &record->variants[lab.variant];
        stats->replay_hits++;
      }
    }
    best = crl_timed_generation(
        game, move_list, lab.replay_with_carry ? carry : NULL, variant, stats);
  } else {
    if (record != NULL && record->eligible) {
      stats->eligible++;
    }
    best = crl_timed_generation(game, move_list,
                                lab.mode == CRL_MODE_TIME_CARRY ? carry : NULL,
                                NULL, stats);
  }
  if (record != NULL && crl_move_signature(best) != record->move_signature) {
    stats->mismatches++;
    best = get_top_equity_move_with_carry(game, move_list, NULL);
  }
  return best;
}

static SimRolloutHooks crl_hooks = {
    .context = NULL,
    .create_worker = crl_worker_create,
    .destroy_worker = crl_worker_destroy,
    .prepare = crl_prepare,
    .before_candidate = crl_before_candidate,
    .after_candidate = crl_after_candidate,
    .best_move = crl_best_move,
    .carry_buckets = 0,
};

void crl_set_mode(crl_mode_t mode, int variant, bool replay_with_carry) {
  lab.mode = mode;
  lab.variant = variant;
  lab.replay_with_carry = replay_with_carry;
  lab.replay_parts = CRL_REPLAY_SKIPS | CRL_REPLAY_FLOOR;
  if (mode == CRL_MODE_RECORD && lab.all_moves == NULL) {
    lab.all_moves = move_list_create(CRL_ALL_MOVES_CAPACITY);
  }
  crl_hooks.carry_buckets = crl_carry_buckets;
  rv_sim_set_rollout_hooks(
      mode != CRL_MODE_OFF || crl_carry_buckets > 0 ? &crl_hooks : NULL);
}
