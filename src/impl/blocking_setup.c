#include "blocking_setup.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"
#include "gameplay.h"
#include "move_gen.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

BlockingSetupSamples *blocking_setup_samples_create(int max_racks,
                                                    int pool_capacity) {
  assert(max_racks >= BLOCKING_SETUP_MIN_RACKS &&
         max_racks <= BLOCKING_SETUP_MAX_RACKS);
  assert(pool_capacity > 0);
  BlockingSetupSamples *samples = malloc_or_die(sizeof(BlockingSetupSamples));
  samples->num_racks = 0;
  samples->pool_size = 0;
  samples->rack_capacity = max_racks;
  samples->pool_capacity = pool_capacity;
  samples->opponent_racks = malloc_or_die(sizeof(Rack) * (size_t)max_racks);
  samples->draw_orders = malloc_or_die(
      sizeof(MachineLetter) * (size_t)max_racks * (size_t)pool_capacity);
  samples->draw_sizes = malloc_or_die(sizeof(int) * (size_t)max_racks);
  return samples;
}

void blocking_setup_samples_destroy(BlockingSetupSamples *samples) {
  if (samples == NULL) {
    return;
  }
  free(samples->opponent_racks);
  free(samples->draw_orders);
  free(samples->draw_sizes);
  free(samples);
}

int blocking_setup_unseen_pool(const Game *game, MachineLetter *pool) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  int unseen[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = ld_get_dist(ld, ml);
  }
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter letter = board_get_letter(board, row, col);
      if (letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      const MachineLetter counted =
          get_is_blanked(letter) ? BLANK_MACHINE_LETTER : letter;
      assert(unseen[counted] > 0);
      unseen[counted]--;
    }
  }
  const Rack *rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  int pool_size = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    const int count = unseen[ml] - rack_get_letter(rack, (MachineLetter)ml);
    assert(count >= 0);
    for (int tile_idx = 0; tile_idx < count; tile_idx++) {
      pool[pool_size++] = (MachineLetter)ml;
    }
  }
  return pool_size;
}

static void shuffle_tiles(MachineLetter *tiles, int count, XoshiroPRNG *prng) {
  for (int tile_idx = 0; tile_idx < count - 1; tile_idx++) {
    const int pick = tile_idx + (int)prng_get_random_number(
                                    prng, (uint64_t)(count - tile_idx));
    const MachineLetter tile = tiles[pick];
    tiles[pick] = tiles[tile_idx];
    tiles[tile_idx] = tile;
  }
}

void blocking_setup_samples_deal(BlockingSetupSamples *samples,
                                 const Game *game, int num_racks,
                                 bool partition, bool condition_draws,
                                 uint64_t seed) {
  assert(num_racks >= BLOCKING_SETUP_MIN_RACKS &&
         num_racks <= samples->rack_capacity);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  assert(ld_get_total_tiles(ld) <= samples->pool_capacity);
  MachineLetter *pool =
      malloc_or_die(sizeof(MachineLetter) * (size_t)samples->pool_capacity);
  MachineLetter *shuffled =
      malloc_or_die(sizeof(MachineLetter) * (size_t)samples->pool_capacity);
  const int pool_size = blocking_setup_unseen_pool(game, pool);
  const int rack_size = pool_size < RACK_SIZE ? pool_size : RACK_SIZE;
  XoshiroPRNG *prng = prng_create(seed);
  int rack_idx = 0;
  while (rack_idx < num_racks) {
    for (int tile_idx = 0; tile_idx < pool_size; tile_idx++) {
      shuffled[tile_idx] = pool[tile_idx];
    }
    shuffle_tiles(shuffled, pool_size, prng);
    // With partition, one shuffle deals as many disjoint racks as fit; an
    // independent draw uses a fresh shuffle for every rack.
    for (int start = 0; start + rack_size <= pool_size && rack_idx < num_racks;
         start += rack_size) {
      Rack *rack = &samples->opponent_racks[rack_idx];
      rack_set_dist_size_and_reset(rack, ld_size);
      for (int tile_idx = 0; tile_idx < rack_size; tile_idx++) {
        rack_add_letter(rack, shuffled[start + tile_idx]);
      }
      rack_idx++;
      if (!partition || rack_size == 0) {
        break;
      }
    }
  }
  for (rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    Rack excluded;
    rack_copy(&excluded, &samples->opponent_racks[rack_idx]);
    MachineLetter *order = samples->draw_orders +
                           ((size_t)rack_idx * (size_t)samples->pool_capacity);
    int draw_size = 0;
    for (int tile_idx = 0; tile_idx < pool_size; tile_idx++) {
      const MachineLetter tile = pool[tile_idx];
      if (condition_draws && rack_get_letter(&excluded, tile) > 0) {
        rack_take_letter(&excluded, tile);
      } else {
        order[draw_size++] = tile;
      }
    }
    shuffle_tiles(order, draw_size, prng);
    samples->draw_sizes[rack_idx] = draw_size;
  }
  samples->num_racks = num_racks;
  samples->pool_size = pool_size;
  prng_destroy(prng);
  free(shuffled);
  free(pool);
}

void blocking_setup_candidate_leave(const Game *game, const Move *move,
                                    Rack *leave) {
  rack_copy(leave, player_get_rack(game_get_player(
                       game, game_get_player_on_turn_index(game))));
  const bool is_exchange = move_get_type(move) == GAME_EVENT_EXCHANGE;
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter tile = move_get_tile(move, tile_idx);
    // An exchanged blank is BLANK_MACHINE_LETTER, which shares its value
    // with PLAYED_THROUGH_MARKER; only placements play through tiles.
    if (!is_exchange && tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    rack_take_letter(leave, get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile);
  }
}

enum {
  // Replies kept per rack after our pass, and the points below the best
  // within which the list is complete (see pass_reply_lists).
  BLOCKING_SETUP_PASS_REPLIES = 16,
  BLOCKING_SETUP_PASS_REPLY_MARGIN = 20,
};

// A set of board squares, one bit per column for each row.
typedef struct SquareSet {
  uint32_t rows[BOARD_DIM];
} SquareSet;

static_assert(BOARD_DIM <= 32, "SquareSet rows must hold a board row");
static_assert(2 * BOARD_DIM <= 64, "lane masks must hold every lane");

// A best placement found on a board, with the squares its words depend on
// (see checker_footprint), so it can be reused on a board that differs by
// a candidate that leaves those squares alone.
typedef struct BestPlay {
  // Whether the entry has been filled (for lazily filled caches).
  bool known;
  bool exists;
  Move move;
  SquareSet footprint;
} BestPlay;

struct BlockingSetupChecker {
  const LetterDistribution *ld;
  const BlockingSetupSamples *samples;
  int followup_draws;
  int on_turn_index;
  int bag_tiles;
  Game *base;
  // The base board plus the candidate being measured.
  Game *candidate_board;
  Game *after;
  // One board per rack: our pass and that rack's best reply to it.
  Game **pass_replied;
  int pass_replied_capacity;
  BestPlay *pass_replies;
  // Per rack, the best replies after our pass in compare_moves order (the
  // first is pass_replies' entry), complete for every reply scoring at
  // least pass_reply_floor: a candidate that disturbs the best can then
  // start from the best one it leaves intact.
  BestPlay *pass_reply_lists;
  int *pass_reply_counts;
  Equity *pass_reply_floors;
  MoveList *reply_candidates;
  // Pass-branch follow-ups depend only on the rack and the leave (the refill
  // is the leave's complement from the rack's draw order), so they are kept
  // per distinct leave seen since the last load: num_leaves leaves, each
  // with num_racks * followup_draws entries.
  Rack *leaves;
  BestPlay *leave_pass_followups;
  int num_leaves;
  int leave_capacity;
  MoveList *reply_list;
  Move *pass_move;
};

BlockingSetupChecker *blocking_setup_checker_create(void) {
  BlockingSetupChecker *checker =
      calloc_or_die(1, sizeof(BlockingSetupChecker));
  checker->reply_list = move_list_create(1);
  checker->reply_candidates = move_list_create(BLOCKING_SETUP_PASS_REPLIES);
  checker->pass_move = move_create();
  move_set_as_pass(checker->pass_move);
  return checker;
}

static void checker_destroy_games(BlockingSetupChecker *checker) {
  game_destroy(checker->base);
  game_destroy(checker->after);
  game_destroy(checker->candidate_board);
  checker->candidate_board = NULL;
  for (int rack_idx = 0; rack_idx < checker->pass_replied_capacity;
       rack_idx++) {
    game_destroy(checker->pass_replied[rack_idx]);
  }
  free(checker->pass_replied);
  free(checker->pass_replies);
  free(checker->pass_reply_lists);
  free(checker->pass_reply_counts);
  free(checker->pass_reply_floors);
  checker->pass_reply_lists = NULL;
  checker->pass_reply_counts = NULL;
  checker->pass_reply_floors = NULL;
  checker->base = NULL;
  checker->after = NULL;
  checker->pass_replied = NULL;
  checker->pass_replies = NULL;
  checker->pass_replied_capacity = 0;
}

void blocking_setup_checker_destroy(BlockingSetupChecker *checker) {
  if (checker == NULL) {
    return;
  }
  checker_destroy_games(checker);
  free(checker->leaves);
  free(checker->leave_pass_followups);
  move_list_destroy(checker->reply_list);
  move_list_destroy(checker->reply_candidates);
  move_destroy(checker->pass_move);
  free(checker);
}

// Copies src into *dst, creating it when missing. Copies share the letter
// distribution, so a checker reloaded with a different one recreates its
// games first (see blocking_setup_checker_load).
static void checker_copy_game(Game **dst, const Game *src) {
  if (*dst == NULL) {
    *dst = game_duplicate(src);
  } else {
    game_copy(*dst, src);
  }
}

static bool square_set_has(const SquareSet *set, int row, int col) {
  return ((set->rows[row] >> col) & 1) != 0;
}

static void square_set_add(SquareSet *set, int row, int col) {
  set->rows[row] |= (uint32_t)1 << col;
}

static bool square_is_occupied(const Board *board, int row, int col) {
  return board_get_letter(board, row, col) != ALPHABET_EMPTY_SQUARE_MARKER;
}

// Adds the empty squares just past each end of the run of occupied squares
// through (row, col) along the given direction, (row, col) itself counting
// as occupied.
static void add_run_ends(const Board *board, int row, int col, bool vertical,
                         SquareSet *set) {
  const int row_step = vertical ? 1 : 0;
  const int col_step = vertical ? 0 : 1;
  for (int sign = -1; sign <= 1; sign += 2) {
    int end_row = row + (sign * row_step);
    int end_col = col + (sign * col_step);
    while (end_row >= 0 && end_row < BOARD_DIM && end_col >= 0 &&
           end_col < BOARD_DIM && square_is_occupied(board, end_row, end_col)) {
      end_row += sign * row_step;
      end_col += sign * col_step;
    }
    if (end_row >= 0 && end_row < BOARD_DIM && end_col >= 0 &&
        end_col < BOARD_DIM) {
      square_set_add(set, end_row, end_col);
    }
  }
}

// The squares a placement's words depend on, on the board it is legal on:
// the squares it fills, the squares just past both ends of its main word,
// and those past both ends of each cross word through a tile it places. A
// change to the board that fills none of them leaves the placement legal
// with the same words, so with the same score.
static void checker_footprint(const Board *board, const Move *move,
                              SquareSet *footprint) {
  memset(footprint, 0, sizeof(SquareSet));
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int length = move_get_tiles_length(move);
  const int row = move_get_row_start(move);
  const int col = move_get_col_start(move);
  // The main word spans the move's tiles, played-through ones included.
  if (vertical) {
    add_run_ends(board, row, col, true, footprint);
    add_run_ends(board, row + length - 1, col, true, footprint);
  } else {
    add_run_ends(board, row, col, false, footprint);
    add_run_ends(board, row, col + length - 1, false, footprint);
  }
  for (int tile_idx = 0; tile_idx < length; tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int tile_row = row + (vertical ? tile_idx : 0);
    const int tile_col = col + (vertical ? 0 : tile_idx);
    square_set_add(footprint, tile_row, tile_col);
    add_run_ends(board, tile_row, tile_col, !vertical, footprint);
  }
}

// Whether the candidate fills none of the squares in footprint.
static bool candidate_leaves_clear(const Move *candidate,
                                   const SquareSet *footprint) {
  const bool vertical = board_is_dir_vertical(move_get_dir(candidate));
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(candidate);
       tile_idx++) {
    if (move_get_tile(candidate, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = move_get_row_start(candidate) + (vertical ? tile_idx : 0);
    const int col = move_get_col_start(candidate) + (vertical ? 0 : tile_idx);
    if (square_set_has(footprint, row, col)) {
      return false;
    }
  }
  return true;
}

// The lanes a placement made on board can matter to: every row and column
// through a square it filled or through an empty square just past a run
// through one. A play in no such lane neither fills nor scores through a
// square the placement changed, so it was available, with the same score,
// before the placement. board already holds the placement.
uint64_t candidate_lanes(const Board *board, const Move *candidate);
uint64_t candidate_lanes(const Board *board, const Move *candidate) {
  SquareSet changed;
  memset(&changed, 0, sizeof(changed));
  const bool vertical = board_is_dir_vertical(move_get_dir(candidate));
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(candidate);
       tile_idx++) {
    if (move_get_tile(candidate, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = move_get_row_start(candidate) + (vertical ? tile_idx : 0);
    const int col = move_get_col_start(candidate) + (vertical ? 0 : tile_idx);
    square_set_add(&changed, row, col);
    add_run_ends(board, row, col, false, &changed);
    add_run_ends(board, row, col, true, &changed);
  }
  uint64_t lanes = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    if (changed.rows[row] == 0) {
      continue;
    }
    lanes |= (uint64_t)1 << (BOARD_DIM * BOARD_HORIZONTAL_DIRECTION + row);
    for (int col = 0; col < BOARD_DIM; col++) {
      if (square_set_has(&changed, row, col)) {
        lanes |= (uint64_t)1 << (BOARD_DIM * BOARD_VERTICAL_DIRECTION + col);
      }
    }
  }
  return lanes;
}

// The best tile placement by score for the player on turn's rack, searching
// only lane_mask's lanes (0 for all), or NULL when there is none.
static const Move *checker_best_placement(BlockingSetupChecker *checker,
                                          const Game *game, uint64_t lane_mask,
                                          const Move *initial_best) {
  move_list_reset(checker->reply_list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = checker->reply_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .lane_mask = lane_mask,
      .initial_best_move = initial_best,
      .skip_exchanges = true,
      .disable_pat = true,
  };
  generate_moves(&args);
  if (move_list_get_count(checker->reply_list) == 0) {
    return NULL;
  }
  const Move *best = move_list_get_move(checker->reply_list, 0);
  if (move_get_type(best) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return NULL;
  }
  return best;
}

// The best placement on game, given that the candidate was just placed and
// that known was the best placement before it (for the same rack). When
// the candidate leaves known's words alone, only the candidate's lanes can
// hold anything better (see candidate_lanes), and move generation's order
// on equal scores is total (compare_moves), so the result is exactly what a
// full search would find. Otherwise it searches the whole board.
static const Move *checker_best_after_candidate(BlockingSetupChecker *checker,
                                                const Game *game,
                                                const Move *candidate,
                                                const BestPlay *known) {
  if (known->exists && !candidate_leaves_clear(candidate, &known->footprint)) {
    return checker_best_placement(checker, game, 0, NULL);
  }
  // Starting from the known play prunes every lane anchor that cannot
  // reach it, and ties resolve by compare_moves as in a full search.
  return checker_best_placement(
      checker, game, candidate_lanes(game_get_board(game), candidate),
      known->exists ? &known->move : NULL);
}

// The opponent's best reply on game, which holds the candidate: the best
// reply after our pass that the candidate leaves intact is the best of the
// replies the candidate cannot affect (any reply it can affect lies in its
// lanes), so a lane search starting from it is exact. When every reply the
// list is sure of is disturbed, the whole board is searched.
static const Move *checker_reply_after_candidate(BlockingSetupChecker *checker,
                                                 const Game *game,
                                                 const Move *candidate,
                                                 int rack_idx) {
  const BestPlay *entries = checker->pass_reply_lists +
                            ((size_t)rack_idx * BLOCKING_SETUP_PASS_REPLIES);
  const int count = checker->pass_reply_counts[rack_idx];
  if (count == 0) {
    // No reply at all after the pass: only the candidate's lanes can hold one.
    return checker_best_placement(
        checker, game, candidate_lanes(game_get_board(game), candidate), NULL);
  }
  for (int entry_idx = 0; entry_idx < count; entry_idx++) {
    const BestPlay *entry = &entries[entry_idx];
    if (move_get_score(&entry->move) < checker->pass_reply_floors[rack_idx]) {
      break;
    }
    if (candidate_leaves_clear(candidate, &entry->footprint)) {
      return checker_best_placement(
          checker, game, candidate_lanes(game_get_board(game), candidate),
          &entry->move);
    }
  }
  return checker_best_placement(checker, game, 0, NULL);
}

// Records game's best placement for the player on turn's current rack, with
// its footprint on game's board.
static void checker_record_best(BlockingSetupChecker *checker, const Game *game,
                                BestPlay *best_play) {
  const Move *best = checker_best_placement(checker, game, 0, NULL);
  best_play->exists = best != NULL;
  if (best != NULL) {
    move_copy(&best_play->move, best);
    checker_footprint(game_get_board(game), best, &best_play->footprint);
  }
}

static double best_play_score(const BestPlay *best_play) {
  return best_play->exists ? equity_to_double(move_get_score(&best_play->move))
                           : 0.0;
}

// Fills rack_idx's list of best replies after our pass (see
// BlockingSetupChecker.pass_reply_lists).
static void checker_record_reply_list(BlockingSetupChecker *checker,
                                      const Game *passed, int rack_idx) {
  MoveList *list = checker->reply_candidates;
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = passed,
      .move_list = list,
      .move_record_type = MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = int_to_equity(BLOCKING_SETUP_PASS_REPLY_MARGIN),
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .skip_exchanges = true,
      .disable_pat = true,
  };
  generate_moves(&args);
  const bool full = move_list_get_count(list) == move_list_get_capacity(list);
  move_list_sort_moves(list);
  BestPlay *entries = checker->pass_reply_lists +
                      ((size_t)rack_idx * BLOCKING_SETUP_PASS_REPLIES);
  int count = 0;
  Equity lowest = EQUITY_MAX_VALUE;
  for (int move_idx = 0; move_idx < move_list_get_count(list); move_idx++) {
    const Move *move = move_list_get_move(list, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    entries[count].exists = true;
    move_copy(&entries[count].move, move);
    checker_footprint(game_get_board(passed), move, &entries[count].footprint);
    if (move_get_score(move) < lowest) {
      lowest = move_get_score(move);
    }
    count++;
  }
  checker->pass_reply_counts[rack_idx] = count;
  // Every reply within the margin of the best is listed unless the list
  // filled up, when only those above its lowest score are sure to be.
  Equity floor = EQUITY_MAX_VALUE;
  if (count > 0) {
    floor = move_get_score(&entries[0].move) -
            int_to_equity(BLOCKING_SETUP_PASS_REPLY_MARGIN);
    if (full && lowest + 1 > floor) {
      floor = lowest + 1;
    }
  }
  checker->pass_reply_floors[rack_idx] = floor;
}

void blocking_setup_checker_load(BlockingSetupChecker *checker,
                                 const Game *game,
                                 const BlockingSetupSamples *samples,
                                 int followup_draws) {
  assert(!game_over(game));
  assert(bag_get_letters(game_get_bag(game)) > 0);
  assert(samples->num_racks >= BLOCKING_SETUP_MIN_RACKS);
  assert(followup_draws >= 1 &&
         followup_draws <= BLOCKING_SETUP_MAX_FOLLOWUP_DRAWS);
  if (checker->ld != game_get_ld(game) ||
      checker->pass_replied_capacity < samples->num_racks) {
    checker_destroy_games(checker);
    checker->ld = game_get_ld(game);
    checker->pass_replied_capacity = samples->num_racks;
    checker->pass_replied =
        calloc_or_die((size_t)samples->num_racks, sizeof(Game *));
    checker->pass_replies =
        malloc_or_die(sizeof(BestPlay) * (size_t)samples->num_racks);
    checker->pass_reply_lists =
        malloc_or_die(sizeof(BestPlay) * (size_t)samples->num_racks *
                      BLOCKING_SETUP_PASS_REPLIES);
    checker->pass_reply_counts =
        malloc_or_die(sizeof(int) * (size_t)samples->num_racks);
    checker->pass_reply_floors =
        malloc_or_die(sizeof(Equity) * (size_t)samples->num_racks);
  }
  checker->samples = samples;
  checker->followup_draws = followup_draws;
  checker->num_leaves = 0;
  checker->on_turn_index = game_get_player_on_turn_index(game);
  // The tiles left to draw once the opponent holds a full rack. This is the
  // bag when the game knows the opponent's rack, and stays right when it
  // does not (the bag then also holds the opponent's tiles).
  checker->bag_tiles =
      samples->pool_size -
      (samples->pool_size < RACK_SIZE ? samples->pool_size : RACK_SIZE);
  checker_copy_game(&checker->base, game);
  game_set_backup_mode(checker->base, BACKUP_MODE_OFF);
  for (int rack_idx = 0; rack_idx < samples->num_racks; rack_idx++) {
    checker_copy_game(&checker->pass_replied[rack_idx], checker->base);
    Game *passed = checker->pass_replied[rack_idx];
    play_move(checker->pass_move, passed, NULL);
    rack_copy(
        player_get_rack(game_get_player(passed, 1 - checker->on_turn_index)),
        &samples->opponent_racks[rack_idx]);
    BestPlay *reply = &checker->pass_replies[rack_idx];
    checker_record_best(checker, passed, reply);
    checker_record_reply_list(checker, passed, rack_idx);
    play_move(reply->exists ? &reply->move : checker->pass_move, passed, NULL);
  }
}

static bool checker_racks_equal(const Rack *a, const Rack *b) {
  if (rack_get_total_letters(a) != rack_get_total_letters(b)) {
    return false;
  }
  for (int ml = 0; ml < rack_get_dist_size(a); ml++) {
    if (rack_get_letter(a, (MachineLetter)ml) !=
        rack_get_letter(b, (MachineLetter)ml)) {
      return false;
    }
  }
  return true;
}

static size_t checker_followups_per_leave(const BlockingSetupChecker *checker) {
  return (size_t)checker->samples->num_racks * (size_t)checker->followup_draws;
}

// The index in the leave cache of leave's pass-branch follow-ups, one per
// rack and draw, filled lazily (see BestPlay.known).
static int checker_leave_index(BlockingSetupChecker *checker,
                               const Rack *leave) {
  const size_t per_leave = checker_followups_per_leave(checker);
  for (int leave_idx = 0; leave_idx < checker->num_leaves; leave_idx++) {
    if (checker_racks_equal(&checker->leaves[leave_idx], leave)) {
      return leave_idx;
    }
  }
  if (checker->num_leaves == checker->leave_capacity) {
    checker->leave_capacity =
        checker->leave_capacity == 0 ? 64 : 2 * checker->leave_capacity;
    checker->leaves = realloc_or_die(
        checker->leaves, sizeof(Rack) * (size_t)checker->leave_capacity);
  }
  // Sized for the current per-leave count, which can change between loads,
  // so resized whenever a leave is added.
  checker->leave_pass_followups = realloc_or_die(
      checker->leave_pass_followups,
      sizeof(BestPlay) * per_leave * (size_t)checker->leave_capacity);
  rack_copy(&checker->leaves[checker->num_leaves], leave);
  BestPlay *entries =
      checker->leave_pass_followups + ((size_t)checker->num_leaves * per_leave);
  for (size_t entry_idx = 0; entry_idx < per_leave; entry_idx++) {
    entries[entry_idx].known = false;
  }
  return checker->num_leaves++;
}

// What one candidate needs across racks; valid until the next load.
typedef struct CandidateState {
  const Move *move;
  bool is_placement;
  Rack leave;
  // Refill tiles the leave needs, capped by the tiles left to draw.
  int missing;
  // The leave's entry in the leave cache.
  int leave_index;
} CandidateState;

static void checker_prepare_candidate(BlockingSetupChecker *checker,
                                      const Move *candidate,
                                      CandidateState *state) {
  assert(checker->samples != NULL);
  assert(move_get_type(candidate) == GAME_EVENT_TILE_PLACEMENT_MOVE ||
         move_get_type(candidate) == GAME_EVENT_EXCHANGE);
  state->move = candidate;
  state->is_placement =
      move_get_type(candidate) == GAME_EVENT_TILE_PLACEMENT_MOVE;
  blocking_setup_candidate_leave(checker->base, candidate, &state->leave);
  state->missing = RACK_SIZE - rack_get_total_letters(&state->leave);
  if (state->missing > checker->bag_tiles) {
    state->missing = checker->bag_tiles;
  }
  state->leave_index = checker_leave_index(checker, &state->leave);
}

// Plays the candidate on the checker's candidate board, which each rack's
// branch then starts from.
static void checker_play_candidate(BlockingSetupChecker *checker,
                                   const CandidateState *state) {
  checker_copy_game(&checker->candidate_board, checker->base);
  if (state->is_placement) {
    play_move(state->move, checker->candidate_board, NULL);
  }
}

// The follow-up rack of rack_idx and draw_idx for the candidate's leave:
// draw draw_idx takes the tiles at positions draw_idx * n + i (mod the
// order's length) of the rack's draw order, where n is the number of tiles
// the leave needs.
static void checker_followup_rack(const BlockingSetupChecker *checker,
                                  int rack_idx, const CandidateState *state,
                                  int draw_idx, Rack *followup) {
  const BlockingSetupSamples *samples = checker->samples;
  const MachineLetter *order =
      samples->draw_orders +
      ((size_t)rack_idx * (size_t)samples->pool_capacity);
  const int draw_size = samples->draw_sizes[rack_idx];
  const int draw_count =
      state->missing < draw_size ? state->missing : draw_size;
  rack_copy(followup, &state->leave);
  for (int tile_idx = 0; tile_idx < draw_count; tile_idx++) {
    rack_add_letter(followup,
                    order[((draw_idx * draw_count) + tile_idx) % draw_size]);
  }
}

// The pass-branch follow-up of rack_idx and draw_idx for the candidate's
// leave, filled when not yet known.
static const BestPlay *checker_pass_followup(BlockingSetupChecker *checker,
                                             const CandidateState *state,
                                             int rack_idx, int draw_idx,
                                             const Rack *followup) {
  BestPlay *pass_followup =
      checker->leave_pass_followups +
      ((size_t)state->leave_index * checker_followups_per_leave(checker)) +
      ((size_t)rack_idx * (size_t)checker->followup_draws) + (size_t)draw_idx;
  if (!pass_followup->known) {
    const Game *passed = checker->pass_replied[rack_idx];
    if (game_over(passed)) {
      pass_followup->exists = false;
    } else {
      rack_copy(
          player_get_rack(game_get_player(passed, checker->on_turn_index)),
          followup);
      checker_record_best(checker, passed, pass_followup);
    }
    pass_followup->known = true;
  }
  return pass_followup;
}

// One rack's values, in points; follow-ups are summed over the draws.
typedef struct RackValues {
  double pass_reply;
  double candidate_reply;
  double pass_followup;
  double candidate_followup;
  int terminal;
} RackValues;

// Measures one rack for the candidate on the checker's candidate board (see
// checker_play_candidate).
static void checker_measure_rack(BlockingSetupChecker *checker,
                                 const CandidateState *state, int rack_idx,
                                 RackValues *values) {
  const BestPlay *pass_reply = &checker->pass_replies[rack_idx];
  *values = (RackValues){.pass_reply = best_play_score(pass_reply)};
  if (!state->is_placement) {
    // An exchange leaves the board as a pass does, and the opponent's reply
    // and our follow-up on it depend only on the board, the racks and the
    // bag's size, which the exchange leaves as a pass does: the candidate
    // branch is the pass branch with this leave.
    values->candidate_reply = values->pass_reply;
    values->terminal = game_over(checker->pass_replied[rack_idx]);
    for (int draw_idx = 0; draw_idx < checker->followup_draws; draw_idx++) {
      Rack followup;
      checker_followup_rack(checker, rack_idx, state, draw_idx, &followup);
      const double score = best_play_score(
          checker_pass_followup(checker, state, rack_idx, draw_idx, &followup));
      values->pass_followup += score;
      values->candidate_followup += score;
    }
    return;
  }
  checker_copy_game(&checker->after, checker->candidate_board);
  Game *after = checker->after;
  // Whether the candidate's board is the pass branch's plus the candidate:
  // the same reply, and one the candidate left intact.
  bool same_reply = false;
  if (!game_over(after)) {
    rack_copy(
        player_get_rack(game_get_player(after, 1 - checker->on_turn_index)),
        &checker->samples->opponent_racks[rack_idx]);
    const Move *reply =
        checker_reply_after_candidate(checker, after, state->move, rack_idx);
    same_reply =
        pass_reply->exists && reply != NULL &&
        compare_moves_without_equity(reply, &pass_reply->move, true) == -1;
    if (reply != NULL) {
      values->candidate_reply = equity_to_double(move_get_score(reply));
    }
    play_move(reply == NULL ? checker->pass_move : reply, after, NULL);
    values->terminal = game_over(after);
  }
  const Game *passed = checker->pass_replied[rack_idx];
  for (int draw_idx = 0; draw_idx < checker->followup_draws; draw_idx++) {
    Rack followup;
    checker_followup_rack(checker, rack_idx, state, draw_idx, &followup);
    const BestPlay *pass_followup =
        checker_pass_followup(checker, state, rack_idx, draw_idx, &followup);
    values->pass_followup += best_play_score(pass_followup);
    if (game_over(after)) {
      continue;
    }
    rack_copy(player_get_rack(game_get_player(after, checker->on_turn_index)),
              &followup);
    const Move *best = same_reply && !game_over(passed)
                           ? checker_best_after_candidate(
                                 checker, after, state->move, pass_followup)
                           : checker_best_placement(checker, after, 0, NULL);
    if (best != NULL) {
      values->candidate_followup += equity_to_double(move_get_score(best));
    }
  }
}

void blocking_setup_checker_measure(BlockingSetupChecker *checker,
                                    const Move *candidate,
                                    BlockingSetupResult *result) {
  CandidateState state;
  checker_prepare_candidate(checker, candidate, &state);
  checker_play_candidate(checker, &state);
  double pass_reply_sum = 0.0;
  double candidate_reply_sum = 0.0;
  double pass_followup_sum = 0.0;
  double candidate_followup_sum = 0.0;
  int terminal_replies = 0;
  const int num_racks = checker->samples->num_racks;
  for (int rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    RackValues values;
    checker_measure_rack(checker, &state, rack_idx, &values);
    pass_reply_sum += values.pass_reply;
    candidate_reply_sum += values.candidate_reply;
    pass_followup_sum += values.pass_followup;
    candidate_followup_sum += values.candidate_followup;
    terminal_replies += values.terminal;
  }
  const double racks = (double)num_racks;
  const double num_followups = racks * checker->followup_draws;
  result->pass_reply_mean = pass_reply_sum / racks;
  result->candidate_reply_mean = candidate_reply_sum / racks;
  result->blocking_delta = (pass_reply_sum - candidate_reply_sum) / racks;
  result->pass_followup_mean = pass_followup_sum / num_followups;
  result->candidate_followup_mean = candidate_followup_sum / num_followups;
  result->setup_delta =
      (candidate_followup_sum - pass_followup_sum) / num_followups;
  result->terminal_replies = terminal_replies;
}

// One candidate's per-rack adjusted values in a race: rack_values[rack] is
// blocking_weight * (pass reply - candidate reply) + setup_weight *
// (candidate follow-up - pass follow-up) / draws, in points.
typedef struct RaceEntry {
  CandidateState state;
  bool alive;
  double base_points;
  double pass_reply_sum;
  double candidate_reply_sum;
  double pass_followup_sum;
  double candidate_followup_sum;
} RaceEntry;

// The candidate's adjusted value after racks racks, as an Equity, computed
// exactly as from a full measurement (same sums in the same order).
static Equity race_value(const RaceEntry *entry, int racks, int draws,
                         double blocking_weight, double setup_weight) {
  const double blocking_delta =
      (entry->pass_reply_sum - entry->candidate_reply_sum) / racks;
  const double setup_delta =
      (entry->candidate_followup_sum - entry->pass_followup_sum) /
      ((double)racks * draws);
  return double_to_equity(entry->base_points) +
         double_to_equity((blocking_weight * blocking_delta) +
                          (setup_weight * setup_delta));
}

int blocking_setup_checker_choose(BlockingSetupChecker *checker,
                                  const Move *const *candidates,
                                  const Equity *base_equities,
                                  int num_candidates, double blocking_weight,
                                  double setup_weight,
                                  const BlockingSetupRaceSettings *settings,
                                  BlockingSetupRaceStats *stats) {
  assert(num_candidates >= 1);
  const int num_racks = checker->samples->num_racks;
  const int draws = checker->followup_draws;
  RaceEntry *entries =
      malloc_or_die(sizeof(RaceEntry) * (size_t)num_candidates);
  double *rack_values = malloc_or_die(sizeof(double) * (size_t)num_candidates *
                                      (size_t)num_racks);
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    RaceEntry *entry = &entries[cand_idx];
    checker_prepare_candidate(checker, candidates[cand_idx], &entry->state);
    entry->alive = true;
    entry->base_points = equity_to_double(base_equities[cand_idx]);
    entry->pass_reply_sum = 0.0;
    entry->candidate_reply_sum = 0.0;
    entry->pass_followup_sum = 0.0;
    entry->candidate_followup_sum = 0.0;
  }
  const int batch = settings->batch_racks > 0 ? settings->batch_racks
                                              : BLOCKING_SETUP_RACE_BATCH;
  int alive = num_candidates;
  int racks_done = 0;
  int candidate_racks = 0;
  while (racks_done < num_racks && alive > 1) {
    const int end =
        racks_done + batch < num_racks ? racks_done + batch : num_racks;
    for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
      RaceEntry *entry = &entries[cand_idx];
      if (!entry->alive) {
        continue;
      }
      checker_play_candidate(checker, &entry->state);
      for (int rack_idx = racks_done; rack_idx < end; rack_idx++) {
        RackValues values;
        checker_measure_rack(checker, &entry->state, rack_idx, &values);
        entry->pass_reply_sum += values.pass_reply;
        entry->candidate_reply_sum += values.candidate_reply;
        entry->pass_followup_sum += values.pass_followup;
        entry->candidate_followup_sum += values.candidate_followup;
        rack_values[((size_t)cand_idx * (size_t)num_racks) + rack_idx] =
            (blocking_weight * (values.pass_reply - values.candidate_reply)) +
            (setup_weight * (values.candidate_followup - values.pass_followup) /
             draws);
        candidate_racks++;
      }
    }
    racks_done = end;
    if (settings->z <= 0.0 || racks_done < settings->min_racks ||
        racks_done == num_racks) {
      continue;
    }
    // The leader by mean value so far; ties go to the earlier candidate.
    int leader = -1;
    double leader_mean = 0.0;
    for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
      if (!entries[cand_idx].alive) {
        continue;
      }
      const double *values =
          rack_values + ((size_t)cand_idx * (size_t)num_racks);
      double sum = 0.0;
      for (int rack_idx = 0; rack_idx < racks_done; rack_idx++) {
        sum += values[rack_idx];
      }
      const double mean = entries[cand_idx].base_points + (sum / racks_done);
      if (leader < 0 || mean > leader_mean) {
        leader = cand_idx;
        leader_mean = mean;
      }
    }
    // Drop candidates trailing the leader by more than z paired standard
    // errors of the per-rack difference.
    const double *leader_values =
        rack_values + ((size_t)leader * (size_t)num_racks);
    for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
      if (cand_idx == leader || !entries[cand_idx].alive) {
        continue;
      }
      const double *values =
          rack_values + ((size_t)cand_idx * (size_t)num_racks);
      double sum = 0.0;
      double sum_squares = 0.0;
      for (int rack_idx = 0; rack_idx < racks_done; rack_idx++) {
        const double difference = values[rack_idx] - leader_values[rack_idx];
        sum += difference;
        sum_squares += difference * difference;
      }
      const double mean_difference = sum / racks_done;
      const double variance =
          (sum_squares - (sum * mean_difference)) / (racks_done - 1);
      const double standard_error =
          sqrt(variance > 0.0 ? variance : 0.0) / sqrt((double)racks_done);
      const double gap = entries[cand_idx].base_points -
                         entries[leader].base_points + mean_difference;
      if (gap + (settings->z * standard_error) < 0.0) {
        entries[cand_idx].alive = false;
        alive--;
      }
    }
  }
  // The best survivor by the value a full measurement of its racks gives.
  int best = -1;
  Equity best_value = 0;
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    if (!entries[cand_idx].alive) {
      continue;
    }
    const Equity value = race_value(&entries[cand_idx], racks_done, draws,
                                    blocking_weight, setup_weight);
    if (best < 0 || value > best_value) {
      best = cand_idx;
      best_value = value;
    }
  }
  if (stats != NULL) {
    stats->candidate_racks = candidate_racks;
    stats->racks = racks_done;
    stats->survivors = alive;
  }
  free(rack_values);
  free(entries);
  return best;
}

enum {
  // Every move of a position, so exchanges far down the static order are
  // still seen by the exchange quota.
  BLOCKING_SETUP_POLICY_GEN_CAPACITY = 200000,
  BLOCKING_SETUP_POLICY_POOL_CAPACITY = 1024,
  BLOCKING_SETUP_POLICY_MAX_UNIVERSE = 512,
};

struct BlockingSetupPolicy {
  BlockingSetupPolicySettings settings;
  int num_racks;
  MoveList *list;
  BlockingSetupSamples *samples;
  BlockingSetupChecker *checker;
  const Move *universe[BLOCKING_SETUP_POLICY_MAX_UNIVERSE];
  Equity base[BLOCKING_SETUP_POLICY_MAX_UNIVERSE];
};

BlockingSetupPolicy *
blocking_setup_policy_create(const BlockingSetupPolicySettings *settings) {
  assert(settings->params != NULL);
  assert(settings->universe >= 1 &&
         settings->universe + settings->exchange_quota <=
             BLOCKING_SETUP_POLICY_MAX_UNIVERSE);
  BlockingSetupPolicy *policy = malloc_or_die(sizeof(BlockingSetupPolicy));
  policy->settings = *settings;
  policy->num_racks =
      settings->num_racks > 0
          ? settings->num_racks
          : blocking_setup_params_get_teacher_racks(settings->params);
  policy->list = move_list_create(BLOCKING_SETUP_POLICY_GEN_CAPACITY);
  policy->samples = blocking_setup_samples_create(
      policy->num_racks, BLOCKING_SETUP_POLICY_POOL_CAPACITY);
  policy->checker = blocking_setup_checker_create();
  return policy;
}

void blocking_setup_policy_destroy(BlockingSetupPolicy *policy) {
  if (policy == NULL) {
    return;
  }
  move_list_destroy(policy->list);
  blocking_setup_samples_destroy(policy->samples);
  blocking_setup_checker_destroy(policy->checker);
  free(policy);
}

const Move *blocking_setup_policy_choose(BlockingSetupPolicy *policy,
                                         const Game *game, uint64_t seed) {
  const BlockingSetupPolicySettings *settings = &policy->settings;
  MoveList *list = policy->list;
  move_list_reset(list);
  const MoveGenArgs args = {
      .game = game,
      .move_list = list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .disable_pat = true,
  };
  generate_moves(&args);
  if (move_list_get_count(list) == 0) {
    return NULL;
  }
  move_list_sort_moves(list);
  const Move *top = move_list_get_move(list, 0);
  if (move_list_get_count(list) == 1 || game_over(game) ||
      bag_get_letters(game_get_bag(game)) == 0) {
    return top;
  }
  const int on_turn = game_get_player_on_turn_index(game);
  const int lead =
      equity_to_int(player_get_score(game_get_player(game, on_turn)) -
                    player_get_score(game_get_player(game, 1 - on_turn)));
  double blocking_weight = 0.0;
  double setup_weight = 0.0;
  blocking_setup_params_get_weights(settings->params,
                                    bag_get_letters(game_get_bag(game)), lead,
                                    &blocking_weight, &setup_weight);
  if (blocking_weight == 0.0 && setup_weight == 0.0) {
    return top;
  }
  int count = 0;
  int placements = 0;
  int exchanges = 0;
  const Equity top_equity = move_get_equity(top);
  for (int move_idx = 0; move_idx < move_list_get_count(list) &&
                         (placements < settings->universe ||
                          exchanges < settings->exchange_quota);
       move_idx++) {
    const Move *move = move_list_get_move(list, move_idx);
    const game_event_t type = move_get_type(move);
    if (type == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        placements < settings->universe) {
      placements++;
    } else if (type == GAME_EVENT_EXCHANGE &&
               exchanges < settings->exchange_quota &&
               move_get_equity(move) >=
                   top_equity - settings->exchange_margin) {
      exchanges++;
    } else {
      continue;
    }
    policy->universe[count] = move;
    policy->base[count] = move_get_equity(move);
    count++;
  }
  if (count < 2) {
    return top;
  }
  blocking_setup_samples_deal(
      policy->samples, game, policy->num_racks,
      blocking_setup_params_get_teacher_partition(settings->params),
      blocking_setup_params_get_teacher_condition_draws(settings->params),
      seed);
  blocking_setup_checker_load(
      policy->checker, game, policy->samples,
      blocking_setup_params_get_teacher_followup_draws(settings->params));
  const int pick = blocking_setup_checker_choose(
      policy->checker, policy->universe, policy->base, count, blocking_weight,
      setup_weight, &settings->race, NULL);
  return policy->universe[pick];
}
