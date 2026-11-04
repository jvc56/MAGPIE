#include "census_rollout_lab.h"

#include "../compat/ctime.h"
#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/static_eval_defs.h"
#include "../ent/bag.h"
#include "../ent/best_move_carry.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/static_eval.h"
#include "../util/io_util.h"
#include "census.h"
#include "census_play_scores.h"
#include "gameplay.h"
#include "move_gen.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  CRL_NUM_LANES = 2 * BOARD_DIM,
  // Room for every play of a rack, so that a lane's best is never cut off.
  CRL_ALL_MOVES_CAPACITY = 1 << 17,
};

// No play: below every equity a move has.
#define CRL_NO_PLAY EQUITY_MIN_VALUE

const int crl_topk_values[CRL_NUM_TOPK] = {1, 2, 4, 8, CRL_NUM_LANES};

bool crl_active = false;

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
} CensusRolloutLab;

static CensusRolloutLab lab;

void crl_set_mode(crl_mode_t mode, int variant, bool replay_with_carry) {
  lab.mode = mode;
  lab.variant = variant;
  lab.replay_with_carry = replay_with_carry;
  lab.replay_parts = CRL_REPLAY_SKIPS | CRL_REPLAY_FLOOR;
  crl_active = mode != CRL_MODE_OFF;
  if (mode == CRL_MODE_RECORD && lab.all_moves == NULL) {
    lab.all_moves = move_list_create(CRL_ALL_MOVES_CAPACITY);
  }
}

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

static void crl_lazy_reset_stats(void);
static void crl_workers_reset_stats(void);
static void crl_workers_sum_ply(int ply, CrlPlyStats *sum);

void crl_reset_stats(void) {
  crl_workers_reset_stats();
  crl_lazy_reset_stats();
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

static void crl_lazy_set_current(CrlWorker *worker, const Move *candidate);

// Recording and replaying are single-threaded: they walk one list of
// generations in order.
void crl_before_candidate(CrlWorker *worker, const Game *game) {
  (void)worker;
  if (lab.mode == CRL_MODE_RECORD && !lab.root_is_set) {
    crl_copy_board(CRL_BOARD_ROOT, game);
    lab.root_is_set = true;
  }
}

void crl_after_candidate(CrlWorker *worker, const Game *game,
                         const Move *candidate) {
  if (lab.mode == CRL_MODE_RECORD) {
    crl_copy_board(CRL_BOARD_CAND, game);
  }
  if (lab.mode == CRL_MODE_LAZY) {
    crl_lazy_set_current(worker, candidate);
  }
}

static uint64_t crl_move_signature(const Move *move) {
  uint64_t hash = UINT64_C(1469598103934665603);
  const uint64_t prime = UINT64_C(1099511628211);
  hash = (hash ^ (uint64_t)move_get_type(move)) * prime;
  hash = (hash ^ (uint64_t)move_get_row_start(move)) * prime;
  hash = (hash ^ (uint64_t)move_get_col_start(move)) * prime;
  hash = (hash ^ (uint64_t)move_get_dir(move)) * prime;
  const int length = move_get_tiles_length(move);
  hash = (hash ^ (uint64_t)length) * prime;
  for (int tile_idx = 0; tile_idx < length; tile_idx++) {
    hash = (hash ^ (uint64_t)move_get_tile(move, tile_idx)) * prime;
  }
  hash = (hash ^ (uint64_t)(uint32_t)move_get_score(move)) * prime;
  hash = (hash ^ (uint64_t)(uint32_t)move_get_equity(move)) * prime;
  return hash;
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
      lanes |= (uint64_t)1 << (vertical ? BOARD_DIM * BOARD_HORIZONTAL_DIRECTION + row
                                        : BOARD_DIM * BOARD_VERTICAL_DIRECTION + col);
    }
  }
  return lanes;
}

// A lane's squares as the generator reads them (lane BOARD_DIM * dir + row).
static const Square *crl_lane_squares(const Game *game, int lane) {
  const int cross_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), 0);
  return board_get_row_cache(
      board_get_readonly_lanes(game_get_board(game), cross_index),
      lane % BOARD_DIM, lane / BOARD_DIM);
}

static bool crl_square_is_empty(const Square *square) {
  return square_get_letter(square) == ALPHABET_EMPTY_SQUARE_MARKER;
}

static uint16_t crl_anchor_cols(const Square *squares) {
  uint16_t anchor_cols = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    if (square_get_anchor(&squares[col])) {
      anchor_cols |= (uint16_t)(1U << col);
    }
  }
  return anchor_cols;
}

// The squares of a lane whose letter, cross set, cross score, cross word flag
// or anchor differ between the two boards: what decides which plays a lane
// holds and what they score, square by square.
static uint16_t crl_changed_cols(const Square *now, const Square *then) {
  uint16_t changed = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    if (square_get_letter(&now[col]) != square_get_letter(&then[col]) ||
        square_get_cross_set(&now[col]) != square_get_cross_set(&then[col]) ||
        square_get_cross_score(&now[col]) !=
            square_get_cross_score(&then[col]) ||
        square_get_is_cross_word(&now[col]) !=
            square_get_is_cross_word(&then[col]) ||
        square_get_anchor(&now[col]) != square_get_anchor(&then[col])) {
      changed |= (uint16_t)(1U << col);
    }
  }
  return changed;
}

static uint16_t crl_cols_between(int low, int high) {
  return (uint16_t)((((uint32_t)1 << (high + 1)) - 1) &
                    ~(((uint32_t)1 << low) - 1));
}

// For each anchor of a lane, the squares its plays can cover or border: from
// one left of the farthest start to one right of the farthest end that
// rack_tiles tiles reach on either side, the anchor before it bounding it on
// the left as it bounds the generator. A play's legality and score read only
// the squares it covers and borders, so a changed square outside an anchor's
// zone leaves its plays as they were.
static void crl_anchor_zones(const Square *squares, uint16_t anchor_cols,
                             int rack_tiles, uint16_t *zones) {
  int last_anchor_col = BOARD_DIM;
  for (uint32_t remaining = anchor_cols; remaining != 0;
       remaining &= remaining - 1) {
    const int col = __builtin_ctz(remaining);
    const bool on_tile = !crl_square_is_empty(&squares[col]);
    const int left_limit = last_anchor_col == BOARD_DIM ? 0 : last_anchor_col + 1;
    int spare = rack_tiles - (on_tile ? 0 : 1);
    if (spare < 0) {
      spare = 0;
    }
    int start = col;
    int tiles = spare;
    for (int other = col - 1; other >= left_limit; other--) {
      if (crl_square_is_empty(&squares[other])) {
        if (tiles == 0) {
          break;
        }
        tiles--;
      }
      start = other;
    }
    int end = col;
    tiles = spare;
    for (int other = col + 1; other < BOARD_DIM; other++) {
      if (crl_square_is_empty(&squares[other])) {
        if (tiles == 0) {
          break;
        }
        tiles--;
      }
      end = other;
    }
    zones[col] = crl_cols_between(start > 0 ? start - 1 : 0,
                                  end < BOARD_DIM - 1 ? end + 1 : BOARD_DIM - 1);
    last_anchor_col = on_tile ? col + 1 : col;
  }
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
    lab.records = realloc_or_die(lab.records,
                                 sizeof(CrlRecord) * lab.records_capacity);
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
  uint64_t anchored = 0;
  uint64_t clean[CRL_NUM_BOARDS] = {0};
  uint16_t changed[CRL_NUM_BOARDS][CRL_NUM_LANES] = {{0}};
  uint16_t known[CRL_NUM_BOARDS][CRL_NUM_LANES] = {{0}};
  if (eligible) {
    const int64_t hash_start = ctimer_monotonic_ns();
    gen_hash_game_lanes(game, 0, current_hashes);
    stats->hash_ns += ctimer_monotonic_ns() - hash_start;
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
        stats->variant_searched_lanes[variant] += (uint64_t)__builtin_popcountll(
            anchored & ~variant_record->skip_lanes);
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
    const uint16_t touch_zone = crl_cols_between(
        start > 0 ? start - 1 : 0, end < BOARD_DIM - 1 ? end + 1 : BOARD_DIM - 1);
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
  const int64_t start = ctimer_monotonic_ns();
  generate_moves(&args);
  stats->generation_ns += ctimer_monotonic_ns() - start;
  return move_list_get_move(move_list, 0);
}

// ---- The lazy census ----

enum {
  CRL_MAX_CANDIDATES = 64,
  CRL_MAX_UNITS = CRL_NUM_LANES * BOARD_DIM,
  CRL_NO_UNIT = -1,
};

// A census board's per-anchor play scores: for every placed multiset P, the
// anchors it is played from and its best score from each.
typedef struct CrlUnitTable {
  int number_of_units;
  int16_t unit_of[CRL_NUM_LANES][BOARD_DIM];
  uint32_t *offsets;
  uint16_t *units;
  Equity *scores;
  uint32_t number_of_entries;
  uint32_t entries_capacity;
} CrlUnitTable;

typedef struct CrlCensusBoard {
  Game *game;
  uint64_t signature;
  uint64_t hashes[CRL_NUM_LANES];
  uint16_t anchor_cols[CRL_NUM_LANES];
  // For a full rack, the zone of each anchor (see crl_anchor_zones).
  uint16_t zones[CRL_NUM_LANES][BOARD_DIM];
  // The anchors whose bests are the root table's: every anchor of the root,
  // and those of a candidate's board that the candidate leaves as they were.
  uint16_t from_root[CRL_NUM_LANES];
  // The other anchors' scores.
  CrlUnitTable table;
  // Its place among the candidates, which index each worker's lane memo.
  int index;
} CrlCensusBoard;

typedef struct CrlLazy {
  CensusLattice *lattice;
  Equity *leave;
  const KLV *leave_klv;
  // Lattice-sized, CENSUS_UNPLAYABLE between builds.
  Equity *scratch;
  CensusTouched touched;
  CrlCensusBoard root;
  CrlCensusBoard candidates[CRL_MAX_CANDIDATES];
  int number_of_candidates;
  // The board a candidate is played on, while building.
  Game *candidate_game;
  int max_ply;
  bool verify;
  // Whether generations with fewer than PEG_ADJUST_VALUES_LENGTH tiles in
  // the bag are served too, their plays' pre-endgame adjustments added by
  // tiles played.
  bool low_bag;
  // Whether the census steers the best-move carry's generations, or plain
  // ones.
  bool with_carry;
  CrlLazyStats stats;
} CrlLazy;

static CrlLazy lazy = {.max_ply = 3, .with_carry = true};

enum {
  // Racks whose root-table bests a worker remembers (a power of two).
  CRL_ROOT_GATHER_SLOTS = 1 << 13,
  CRL_MAX_WORKERS = 512,
};

struct CrlWorker {
  CrlPlyStats stats[CRL_MAX_PLIES];
  // The census board of the rollout this worker is playing.
  const CrlCensusBoard *current;
  // Each rack's bests in the root table's anchors, which hold for the whole
  // sim whatever the candidate: every candidate's rollouts deal the opponent
  // the same racks. Keyed by two independent hashes of the rack and the bag's
  // adjustment class.
  uint64_t cache_keys[CRL_ROOT_GATHER_SLOTS];
  uint64_t cache_checks[CRL_ROOT_GATHER_SLOTS];
  Equity *cache_bests;
  int cache_units;
  uint64_t cache_hits;
  uint64_t cache_lookups;
  Equity root_bests[CRL_MAX_UNITS];
  Equity candidate_bests[CRL_MAX_UNITS];
  // By candidate and lane, the last changed version of the lane seen in a
  // rollout (its hash and the rack size its zones were for) and what it
  // gave: the anchors it has, and those whose plays are as they were on the
  // candidate's board. A lane that hashes the same holds the same squares.
  uint64_t memo_hashes[CRL_MAX_CANDIDATES][CRL_NUM_LANES];
  int memo_rack_tiles[CRL_MAX_CANDIDATES][CRL_NUM_LANES];
  uint16_t memo_anchors[CRL_MAX_CANDIDATES][CRL_NUM_LANES];
  uint16_t memo_known[CRL_MAX_CANDIDATES][CRL_NUM_LANES];
  // Verifying: the last skips' known anchors and values, and every play.
  uint16_t last_known[CRL_NUM_LANES];
  Equity last_values[CRL_NUM_LANES][BOARD_DIM];
  MoveList *all_moves;
  Move census_move;
  CrlBoardValues every_play;
  CrlBoardValues multi_tile;
};

// Every worker, for summing and resetting their stats. Workers are created
// and destroyed by the threads that start and end sims, which may be several
// (autoplay's sims), so the list has a lock.
static CrlWorker *crl_workers[CRL_MAX_WORKERS];
static int crl_number_of_workers;
// The stats of workers already destroyed: a sim's workers may end with it.
static CrlPlyStats crl_retired_stats[CRL_MAX_PLIES];
static uint64_t crl_retired_cache_hits;
static uint64_t crl_retired_cache_lookups;
static atomic_flag crl_workers_lock = ATOMIC_FLAG_INIT;

static void crl_workers_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&crl_workers_lock,
                                           memory_order_acquire)) {
  }
}

static void crl_workers_release(void) {
  atomic_flag_clear_explicit(&crl_workers_lock, memory_order_release);
}

CrlWorker *crl_worker_create(void) {
  CrlWorker *worker = calloc_or_die(1, sizeof(CrlWorker));
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
  for (size_t word = 0; word < sizeof(CrlPlyStats) / sizeof(uint64_t);
       word++) {
    sum_words[word] += stats_words[word];
  }
}

void crl_worker_destroy(CrlWorker *worker) {
  if (worker == NULL) {
    return;
  }
  crl_workers_acquire();
  for (int ply = 0; ply < CRL_MAX_PLIES; ply++) {
    crl_add_ply_stats(&crl_retired_stats[ply], &worker->stats[ply]);
  }
  crl_retired_cache_hits += worker->cache_hits;
  crl_retired_cache_lookups += worker->cache_lookups;
  for (int worker_idx = 0; worker_idx < crl_number_of_workers; worker_idx++) {
    if (crl_workers[worker_idx] == worker) {
      crl_workers[worker_idx] = crl_workers[--crl_number_of_workers];
      break;
    }
  }
  crl_workers_release();
  move_list_destroy(worker->all_moves);
  free(worker->cache_bests);
  free(worker);
}

static void crl_workers_reset_stats(void) {
  crl_workers_acquire();
  memset(crl_retired_stats, 0, sizeof(crl_retired_stats));
  crl_retired_cache_hits = 0;
  crl_retired_cache_lookups = 0;
  for (int worker_idx = 0; worker_idx < crl_number_of_workers; worker_idx++) {
    memset(crl_workers[worker_idx]->stats, 0,
           sizeof(crl_workers[worker_idx]->stats));
    crl_workers[worker_idx]->cache_hits = 0;
    crl_workers[worker_idx]->cache_lookups = 0;
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

// A table with no entries, for a gather that skips the root's.
static CrlUnitTable crl_empty_table;
static uint32_t *crl_empty_offsets;

void crl_set_lazy_options(int max_ply, bool verify, bool low_bag) {
  lazy.max_ply = max_ply;
  lazy.verify = verify;
  lazy.low_bag = low_bag;
}

void crl_set_lazy_carry(bool with_carry) { lazy.with_carry = with_carry; }

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

const CrlLazyStats *crl_get_lazy_stats(void) {
  crl_workers_acquire();
  lazy.stats.root_gather_hits = crl_retired_cache_hits;
  lazy.stats.root_gather_lookups = crl_retired_cache_lookups;
  for (int worker_idx = 0; worker_idx < crl_number_of_workers; worker_idx++) {
    lazy.stats.root_gather_hits += crl_workers[worker_idx]->cache_hits;
    lazy.stats.root_gather_lookups += crl_workers[worker_idx]->cache_lookups;
  }
  crl_workers_release();
  return &lazy.stats;
}

static void crl_lazy_reset_stats(void) {
  memset(&lazy.stats, 0, sizeof(lazy.stats));
}

// The lazy census serves any generation made while the bag holds tiles: a
// play's equity is then its score, its leave's value, and the pre-endgame
// adjustment, which for a given bag depends only on how many tiles the play
// places (Quackle's leftInBagPlusSeven index).
static bool crl_lazy_is_eligible(const Game *game) {
  return bag_get_letters(game_get_bag(game)) > 0 &&
         board_get_tiles_played(game_get_board(game)) > 0;
}

// By tiles played, the pre-endgame adjustment of a play from this bag, as
// static_eval_get_nonopening_move_equity adds it.
static void crl_peg_by_tiles_played(int number_of_tiles_in_bag,
                                    Equity *by_tiles_played) {
  by_tiles_played[0] = 0;
  for (int tiles_played = 1; tiles_played <= RACK_SIZE; tiles_played++) {
    const int index = number_of_tiles_in_bag - tiles_played + RACK_SIZE;
    by_tiles_played[tiles_played] =
        index < PEG_ADJUST_VALUES_LENGTH ? peg_adjust_values[index] : 0;
  }
}

static void crl_lazy_ensure_lattice(const Game *game) {
  if (lazy.lattice != NULL) {
    return;
  }
  ErrorStack *error_stack = error_stack_create();
  lazy.lattice = census_lattice_create(ld_get_size(game_get_ld(game)),
                                       RACK_SIZE, error_stack);
  error_stack_destroy(error_stack);
  if (lazy.lattice == NULL) {
    log_fatal("census rollout lab: no census lattice for this alphabet");
  }
  const uint32_t size = census_lattice_get_size(lazy.lattice);
  lazy.leave = malloc_or_die(sizeof(Equity) * size);
  lazy.scratch = malloc_or_die(sizeof(Equity) * size);
  for (uint32_t index = 0; index < size; index++) {
    lazy.scratch[index] = CENSUS_UNPLAYABLE;
  }
}

static void crl_table_append(CrlUnitTable *table, uint32_t rank, int unit,
                             Equity score, uint32_t **ranks) {
  if (table->number_of_entries == table->entries_capacity) {
    table->entries_capacity =
        table->entries_capacity ? table->entries_capacity * 2 : 1 << 16;
    table->units = realloc_or_die(table->units,
                                  sizeof(uint16_t) * table->entries_capacity);
    table->scores = realloc_or_die(table->scores,
                                   sizeof(Equity) * table->entries_capacity);
    *ranks =
        realloc_or_die(*ranks, sizeof(uint32_t) * table->entries_capacity);
  }
  (*ranks)[table->number_of_entries] = rank;
  table->units[table->number_of_entries] = (uint16_t)unit;
  table->scores[table->number_of_entries] = score;
  table->number_of_entries++;
}

// Builds the play scores of the anchors in build_cols, by lane, of the board,
// over its pool, sparsely: each anchor's build resets only what it wrote.
static void crl_table_build(CrlUnitTable *table, const Game *game,
                            const uint16_t *build_cols) {
  const uint32_t size = census_lattice_get_size(lazy.lattice);
  uint8_t pool[CENSUS_MAX_LETTERS];
  census_get_unseen_tally(game, pool);
  table->number_of_units = 0;
  table->number_of_entries = 0;
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      table->unit_of[lane][col] = CRL_NO_UNIT;
    }
  }
  uint32_t *ranks = NULL;
  const uint32_t saved_capacity = table->entries_capacity;
  // crl_table_append grows ranks with the table; start it at the table's size.
  if (saved_capacity > 0) {
    ranks = malloc_or_die(sizeof(uint32_t) * saved_capacity);
  }
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    for (uint32_t remaining = build_cols[lane]; remaining != 0;
         remaining &= remaining - 1) {
      const int col = __builtin_ctz(remaining);
      const int unit = table->number_of_units++;
      table->unit_of[lane][col] = (int16_t)unit;
      lazy.touched.count = 0;
      census_set_touched(&lazy.touched);
      census_play_scores_build_anchor(game, lazy.lattice, pool, lazy.scratch,
                                      lane, col);
      census_set_touched(NULL);
      for (uint32_t touched_idx = 0; touched_idx < lazy.touched.count;
           touched_idx++) {
        const uint32_t rank = lazy.touched.ranks[touched_idx];
        const Equity score = lazy.scratch[rank];
        if (score > 0) {
          crl_table_append(table, rank, unit, score, &ranks);
        }
        lazy.scratch[rank] = CENSUS_UNPLAYABLE;
      }
    }
  }
  if (table->offsets == NULL) {
    table->offsets = malloc_or_die(sizeof(uint32_t) * ((size_t)size + 1));
  }
  memset(table->offsets, 0, sizeof(uint32_t) * ((size_t)size + 1));
  for (uint32_t entry = 0; entry < table->number_of_entries; entry++) {
    table->offsets[ranks[entry] + 1]++;
  }
  for (uint32_t index = 0; index < size; index++) {
    table->offsets[index + 1] += table->offsets[index];
  }
  // Order the entries by rank, through a copy.
  uint16_t *units = malloc_or_die(sizeof(uint16_t) * (table->number_of_entries + 1));
  Equity *scores = malloc_or_die(sizeof(Equity) * (table->number_of_entries + 1));
  uint32_t *fill = malloc_or_die(sizeof(uint32_t) * ((size_t)size + 1));
  memcpy(fill, table->offsets, sizeof(uint32_t) * ((size_t)size + 1));
  for (uint32_t entry = 0; entry < table->number_of_entries; entry++) {
    const uint32_t slot = fill[ranks[entry]]++;
    units[slot] = table->units[entry];
    scores[slot] = table->scores[entry];
  }
  memcpy(table->units, units, sizeof(uint16_t) * table->number_of_entries);
  memcpy(table->scores, scores, sizeof(Equity) * table->number_of_entries);
  free(fill);
  free(scores);
  free(units);
  free(ranks);
}

static void crl_census_board_load(CrlCensusBoard *board, const Game *game) {
  if (board->game == NULL) {
    board->game = game_duplicate(game);
  } else {
    game_copy(board->game, game);
  }
  gen_hash_game_lanes(game, 0, board->hashes);
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    const Square *squares = crl_lane_squares(game, lane);
    board->anchor_cols[lane] = crl_anchor_cols(squares);
    crl_anchor_zones(squares, board->anchor_cols[lane], RACK_SIZE,
                     board->zones[lane]);
  }
}

static void crl_lazy_set_root(const Game *game) {
  crl_lazy_ensure_lattice(game);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  if (klv != lazy.leave_klv) {
    census_leaves_from_klv(lazy.lattice, klv, lazy.leave);
    lazy.leave_klv = klv;
  }
  const int64_t start = ctimer_monotonic_ns();
  crl_census_board_load(&lazy.root, game);
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    lazy.root.from_root[lane] = 0;
  }
  crl_table_build(&lazy.root.table, game, lazy.root.anchor_cols);
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    lazy.root.from_root[lane] = lazy.root.anchor_cols[lane];
  }

  lazy.stats.root_build_ns += ctimer_monotonic_ns() - start;
  lazy.stats.root_entries += lazy.root.table.number_of_entries;
  if (crl_empty_offsets == NULL) {
    crl_empty_offsets =
        calloc_or_die((size_t)census_lattice_get_size(lazy.lattice) + 1,
                      sizeof(uint32_t));
    crl_empty_table.offsets = crl_empty_offsets;
  }
  lazy.number_of_candidates = 0;
}

// The board of the candidate whose signature this is, or NULL.
static void crl_lazy_set_current(CrlWorker *worker, const Move *candidate) {
  const uint64_t signature = crl_move_signature(candidate);
  worker->current = NULL;
  for (int candidate_idx = 0; candidate_idx < lazy.number_of_candidates;
       candidate_idx++) {
    if (lazy.candidates[candidate_idx].signature == signature) {
      worker->current = &lazy.candidates[candidate_idx];
      return;
    }
  }
}

// Builds the board after candidate, played on game, and the play scores of
// its anchors that the candidate changes.
static void crl_lazy_add_candidate(const Game *game, const Move *candidate) {
  if (lazy.number_of_candidates == CRL_MAX_CANDIDATES) {
    return;
  }
  const int64_t start = ctimer_monotonic_ns();
  if (lazy.candidate_game == NULL) {
    lazy.candidate_game = game_duplicate(game);
  } else {
    game_copy(lazy.candidate_game, game);
  }
  play_move(candidate, lazy.candidate_game, NULL);
  const Game *board_game = lazy.candidate_game;
  const uint64_t signature = crl_move_signature(candidate);
  CrlCensusBoard *board = &lazy.candidates[lazy.number_of_candidates];
  board->index = lazy.number_of_candidates;
  lazy.number_of_candidates++;
  crl_census_board_load(board, board_game);
  board->signature = signature;
  uint16_t build_cols[CRL_NUM_LANES];
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    board->from_root[lane] = 0;
    if (board->hashes[lane] == lazy.root.hashes[lane]) {
      board->from_root[lane] = board->anchor_cols[lane];
    } else {
      const uint16_t changed =
          crl_changed_cols(crl_lane_squares(board_game, lane),
                           crl_lane_squares(lazy.root.game, lane));
      for (uint32_t remaining =
               board->anchor_cols[lane] & lazy.root.anchor_cols[lane];
           remaining != 0; remaining &= remaining - 1) {
        const int col = __builtin_ctz(remaining);
        if ((board->zones[lane][col] & changed) == 0 &&
            (lazy.root.zones[lane][col] & changed) == 0) {
          board->from_root[lane] |= (uint16_t)(1U << col);
        }
      }
    }
    build_cols[lane] = board->anchor_cols[lane] & (uint16_t)~board->from_root[lane];
  }
  crl_table_build(&board->table, board_game, build_cols);
  lazy.stats.candidate_build_ns += ctimer_monotonic_ns() - start;
  lazy.stats.candidate_builds++;
  lazy.stats.candidate_entries += board->table.number_of_entries;
}

void crl_prepare_sim(const Game *game, const MoveList *candidates) {
  if (lab.mode != CRL_MODE_LAZY) {
    return;
  }
  crl_lazy_set_root(game);
  const int count = move_list_get_count(candidates);
  for (int move_idx = 0; move_idx < count; move_idx++) {
    crl_lazy_add_candidate(game, move_list_get_move(candidates, move_idx));
  }
  const int root_units = lazy.root.table.number_of_units;
  crl_workers_acquire();
  for (int worker_idx = 0; worker_idx < crl_number_of_workers; worker_idx++) {
    CrlWorker *worker = crl_workers[worker_idx];
    worker->current = NULL;
    memset(worker->cache_keys, 0, sizeof(worker->cache_keys));
    memset(worker->memo_hashes, 0, sizeof(worker->memo_hashes));
    if (worker->cache_units < root_units) {
      free(worker->cache_bests);
      worker->cache_units = root_units;
      worker->cache_bests = malloc_or_die(sizeof(Equity) * (size_t)root_units *
                                          CRL_ROOT_GATHER_SLOTS);
    }
  }
  crl_workers_release();
}

// The rack's best in each unit of the two tables: root_bests by the root's
// units, candidate_bests by the candidate's. The splits come from an odometer
// whose first letter turns fastest; a letter's rank term reads only the
// counts at or after it, so each split re-ranks only the letters it moved.
static void crl_lazy_gather(const Rack *rack, const CrlUnitTable *root_table,
                            const CrlUnitTable *candidate_table,
                            const Equity *peg_by_tiles_played,
                            Equity *root_bests, Equity *candidate_bests) {
  for (int unit = 0; unit < root_table->number_of_units; unit++) {
    root_bests[unit] = CRL_NO_PLAY;
  }
  for (int unit = 0; unit < candidate_table->number_of_units; unit++) {
    candidate_bests[unit] = CRL_NO_PLAY;
  }
  const uint32_t *rank_terms = census_lattice_get_rank_terms(lazy.lattice);
  const int stride = RACK_SIZE + 1;
  int letters[RACK_SIZE];
  int counts[RACK_SIZE];
  int number_of_letters = 0;
  const int dist_size = rack_get_dist_size(rack);
  for (int ml = 0; ml < dist_size; ml++) {
    const int count = rack_get_letter(rack, ml);
    if (count > 0) {
      letters[number_of_letters] = ml;
      counts[number_of_letters] = count;
      number_of_letters++;
    }
  }
  uint32_t size_starts[RACK_SIZE + 1];
  for (int size = 0; size <= RACK_SIZE; size++) {
    size_starts[size] = census_lattice_get_size_start(lazy.lattice, size);
  }
  // By letter index: the played and kept counts, and the counts and rank
  // terms of the letters at or after it.
  int played[RACK_SIZE + 1] = {0};
  int kept[RACK_SIZE + 1];
  int played_after[RACK_SIZE + 1] = {0};
  uint32_t played_terms[RACK_SIZE + 1] = {0};
  int kept_after[RACK_SIZE + 1];
  uint32_t kept_terms[RACK_SIZE + 1];
  kept_after[number_of_letters] = 0;
  kept_terms[number_of_letters] = 0;
  for (int letter_idx = number_of_letters - 1; letter_idx >= 0; letter_idx--) {
    kept[letter_idx] = counts[letter_idx];
    kept_after[letter_idx] = kept_after[letter_idx + 1] + counts[letter_idx];
    kept_terms[letter_idx] =
        kept_terms[letter_idx + 1] +
        rank_terms[(letters[letter_idx] * stride + kept_after[letter_idx]) *
                       stride +
                   counts[letter_idx]];
  }
  while (true) {
    int moved = 0;
    while (moved < number_of_letters && played[moved] == counts[moved]) {
      played[moved] = 0;
      kept[moved] = counts[moved];
      moved++;
    }
    if (moved == number_of_letters) {
      break;
    }
    played[moved]++;
    kept[moved]--;
    // The played letters below the one that moved are absent, and an absent
    // letter's term is 0; the kept ones below it are whole again.
    played_after[moved] = played_after[moved + 1] + played[moved];
    played_terms[moved] =
        played_terms[moved + 1] +
        rank_terms[(letters[moved] * stride + played_after[moved]) * stride +
                   played[moved]];
    for (int letter_idx = moved; letter_idx >= 0; letter_idx--) {
      if (letter_idx < moved) {
        played_after[letter_idx] = played_after[moved];
        played_terms[letter_idx] = played_terms[moved];
      }
      kept_after[letter_idx] = kept_after[letter_idx + 1] + kept[letter_idx];
      kept_terms[letter_idx] =
          kept_terms[letter_idx + 1] +
          rank_terms[(letters[letter_idx] * stride + kept_after[letter_idx]) *
                         stride +
                     kept[letter_idx]];
    }
    const uint32_t played_index =
        size_starts[played_after[0]] + played_terms[0];
    const uint32_t root_first = root_table->offsets[played_index];
    const uint32_t root_last = root_table->offsets[played_index + 1];
    const uint32_t candidate_first = candidate_table->offsets[played_index];
    const uint32_t candidate_last = candidate_table->offsets[played_index + 1];
    if (root_first == root_last && candidate_first == candidate_last) {
      continue;
    }
    const Equity leave_value =
        lazy.leave[size_starts[kept_after[0]] + kept_terms[0]] +
        peg_by_tiles_played[played_after[0]];
    for (uint32_t entry = root_first; entry < root_last; entry++) {
      crl_raise(&root_bests[root_table->units[entry]],
                root_table->scores[entry] + leave_value);
    }
    for (uint32_t entry = candidate_first; entry < candidate_last; entry++) {
      crl_raise(&candidate_bests[candidate_table->units[entry]],
                candidate_table->scores[entry] + leave_value);
    }
  }
}

// The skips the lazy census gives a generation of game's player on turn on
// the current board, whose census board is board: every anchor known to be
// below the best known anchor. Returns false if it knows no anchor.
static bool crl_lazy_skips(CrlWorker *worker, const Game *game,
                           const CrlCensusBoard *board, const uint64_t *hashes,
                           uint64_t *skip_lanes, uint16_t *skip_anchor_cols,
                           int64_t *gather_ns) {
  const Rack *rack =
      player_get_rack(game_get_player(game, game_get_player_on_turn_index(game)));
  Equity *root_bests = worker->root_bests;
  Equity *candidate_bests = worker->candidate_bests;
  const int64_t gather_start = ctimer_monotonic_ns();
  const int bag = bag_get_letters(game_get_bag(game));
  Equity peg_by_tiles_played[RACK_SIZE + 1];
  crl_peg_by_tiles_played(bag, peg_by_tiles_played);
  // Bags that give no play an adjustment share the root bests.
  const int bag_class = bag >= PEG_ADJUST_VALUES_LENGTH ? 0 : bag;
  uint64_t key = UINT64_C(0xCBF29CE484222325) ^ (uint64_t)bag_class;
  uint64_t check = UINT64_C(0x84222325CBF29CE4) + (uint64_t)bag_class;
  for (int ml = 0; ml < rack_get_dist_size(rack); ml++) {
    key = (key ^ rack_get_letter(rack, ml)) * UINT64_C(0x100000001B3);
    check = (check + rack_get_letter(rack, ml) + 1) *
            UINT64_C(0x9E3779B97F4A7C15);
  }
  key |= 1;
  const int slot = (int)(key & (CRL_ROOT_GATHER_SLOTS - 1));
  Equity *cached =
      &worker->cache_bests[(size_t)slot * (size_t)worker->cache_units];
  const int root_units = lazy.root.table.number_of_units;
  worker->cache_lookups++;
  if (worker->cache_keys[slot] == key && worker->cache_checks[slot] == check) {
    worker->cache_hits++;
    crl_lazy_gather(rack, &crl_empty_table, &board->table, peg_by_tiles_played,
                    root_bests, candidate_bests);
    memcpy(root_bests, cached, sizeof(Equity) * (size_t)root_units);
  } else {
    crl_lazy_gather(rack, &lazy.root.table, &board->table,
                    peg_by_tiles_played, root_bests, candidate_bests);
    memcpy(cached, root_bests, sizeof(Equity) * (size_t)root_units);
    worker->cache_keys[slot] = key;
    worker->cache_checks[slot] = check;
  }
  *gather_ns += ctimer_monotonic_ns() - gather_start;
  Equity values[CRL_NUM_LANES][BOARD_DIM];
  uint16_t known[CRL_NUM_LANES];
  uint16_t anchors_now[CRL_NUM_LANES];
  Equity floor_equity = CRL_NO_PLAY;
  bool any_known = false;
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    known[lane] = 0;
    anchors_now[lane] = 0;
    if (hashes[lane] == 0) {
      continue;
    }
    if (hashes[lane] == board->hashes[lane]) {
      anchors_now[lane] = board->anchor_cols[lane];
      known[lane] = anchors_now[lane];
    } else if (worker->memo_hashes[board->index][lane] == hashes[lane] &&
               worker->memo_rack_tiles[board->index][lane] ==
                   rack_get_total_letters(rack)) {
      anchors_now[lane] = worker->memo_anchors[board->index][lane];
      known[lane] = worker->memo_known[board->index][lane];
    } else {
      const Square *squares = crl_lane_squares(game, lane);
      anchors_now[lane] = crl_anchor_cols(squares);
      const uint16_t changed =
          crl_changed_cols(squares, crl_lane_squares(board->game, lane));
      uint16_t zones_now[BOARD_DIM];
      crl_anchor_zones(squares, anchors_now[lane],
                       rack_get_total_letters(rack), zones_now);
      for (uint32_t remaining = anchors_now[lane] & board->anchor_cols[lane];
           remaining != 0; remaining &= remaining - 1) {
        const int col = __builtin_ctz(remaining);
        if ((zones_now[col] & changed) == 0 &&
            (board->zones[lane][col] & changed) == 0) {
          known[lane] |= (uint16_t)(1U << col);
        }
      }
      worker->memo_hashes[board->index][lane] = hashes[lane];
      worker->memo_rack_tiles[board->index][lane] = rack_get_total_letters(rack);
      worker->memo_anchors[board->index][lane] = anchors_now[lane];
      worker->memo_known[board->index][lane] = known[lane];
    }
    for (uint32_t remaining = known[lane]; remaining != 0;
         remaining &= remaining - 1) {
      const int col = __builtin_ctz(remaining);
      Equity value;
      if (board->from_root[lane] & (1U << col)) {
        value = root_bests[lazy.root.table.unit_of[lane][col]];
      } else {
        value = candidate_bests[board->table.unit_of[lane][col]];
      }
      values[lane][col] = value;
      any_known = true;
      crl_raise(&floor_equity, value);
    }
  }
  memcpy(worker->last_known, known, sizeof(worker->last_known));
  memcpy(worker->last_values, values, sizeof(worker->last_values));
  if (!any_known) {
    return false;
  }
  *skip_lanes = 0;
  for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
    uint16_t skip = 0;
    for (uint32_t remaining = known[lane]; remaining != 0;
         remaining &= remaining - 1) {
      const int col = __builtin_ctz(remaining);
      if (values[lane][col] == CRL_NO_PLAY || values[lane][col] < floor_equity) {
        skip |= (uint16_t)(1U << col);
      }
    }
    skip_anchor_cols[lane] = 0;
    if (skip != 0 && skip == anchors_now[lane]) {
      *skip_lanes |= (uint64_t)1 << lane;
    } else {
      skip_anchor_cols[lane] = skip;
    }
  }
  return true;
}

static const Move *crl_lazy_query(CrlWorker *worker, Game *game,
                                  MoveList *move_list, BestMoveCarry *carry,
                                  int ply, CrlPlyStats *stats) {
  MoveGenArgs args = {.game = game,
                      .move_list = move_list,
                      .move_record_type = MOVE_RECORD_BEST,
                      .move_sort_type = MOVE_SORT_EQUITY,
                      .override_kwg = NULL,
                      .eq_margin_movegen = 0,
                      .target_equity = EQUITY_MAX_VALUE,
                      .best_move_carry = lazy.with_carry ? carry : NULL};
  uint16_t skip_anchor_cols[CRL_NUM_LANES];
  uint64_t hashes[CRL_NUM_LANES];
  const int64_t start = ctimer_monotonic_ns();
  if (worker->current != NULL && ply <= lazy.max_ply &&
      (lazy.low_bag ? crl_lazy_is_eligible(game) : crl_is_eligible(game))) {
    stats->eligible++;
    gen_hash_game_lanes(game, 0, hashes);
    // The carry hashes the lanes the same way; it takes these instead.
    args.lane_hashes = hashes;
    uint64_t skip_lanes = 0;
    if (crl_lazy_skips(worker, game, worker->current, hashes, &skip_lanes,
                       skip_anchor_cols, &stats->gather_ns)) {
      args.skip_lanes = skip_lanes;
      args.skip_anchor_cols = skip_anchor_cols;
      stats->replay_hits++;
      if (lazy.verify) {
        // Every known anchor's census value against every play now: at
        // least its best play of two or more tiles, at most its best play
        // (a play of one tile is in both of its lanes for the census, in one
        // for the generator).
        if (worker->all_moves == NULL) {
          worker->all_moves = move_list_create(CRL_ALL_MOVES_CAPACITY);
        }
        crl_board_values_of(game, worker->all_moves, &worker->every_play,
                            false);
        crl_board_values_of(game, worker->all_moves, &worker->multi_tile, true);
        for (int lane = 0; lane < CRL_NUM_LANES; lane++) {
          for (uint32_t remaining = worker->last_known[lane]; remaining != 0;
               remaining &= remaining - 1) {
            const int col = __builtin_ctz(remaining);
            const Equity value = worker->last_values[lane][col];
            stats->anchors_checked++;
            if (value < worker->multi_tile.anchors[lane][col] ||
                value > worker->every_play.anchors[lane][col]) {
              stats->anchor_value_violations++;
            }
          }
        }
      }
    }
  }
  const int64_t generation_start = ctimer_monotonic_ns();
  stats->hash_ns += generation_start - start;
  generate_moves(&args);
  stats->generation_ns += ctimer_monotonic_ns() - generation_start;
  const Move *best = move_list_get_move(move_list, 0);
  if (lazy.verify) {
    if (worker->all_moves == NULL) {
      worker->all_moves = move_list_create(CRL_ALL_MOVES_CAPACITY);
    }
    move_copy(&worker->census_move, best);
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
    best = plain;
  }
  return best;
}

const Move *crl_get_best_move(CrlWorker *worker, Game *game,
                              MoveList *move_list, BestMoveCarry *carry,
                              int ply) {
  CrlPlyStats *stats =
      &worker->stats[ply < CRL_MAX_PLIES ? ply : CRL_MAX_PLIES - 1];
  stats->queries++;
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
    }
    const CrlVariantRecord *variant = NULL;
    if (record->eligible) {
      stats->eligible++;
      if (record->variants[lab.variant].hit) {
        variant = &record->variants[lab.variant];
        stats->replay_hits++;
      }
    }
    best = crl_timed_generation(game, move_list,
                                lab.replay_with_carry ? carry : NULL, variant,
                                stats);
  } else {
    if (record != NULL && record->eligible) {
      stats->eligible++;
    }
    best = crl_timed_generation(
        game, move_list, lab.mode == CRL_MODE_TIME_CARRY ? carry : NULL, NULL,
        stats);
  }
  if (record != NULL && crl_move_signature(best) != record->move_signature) {
    stats->mismatches++;
    best = get_top_equity_move_with_carry(game, move_list, NULL);
  }
  return best;
}
