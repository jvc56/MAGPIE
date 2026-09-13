#include "pat_overlap_pilot_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/cross_set_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Diagnostic pilot for "overlapping reply opportunities" -- the NARCEIN-at-8D
// pattern (one existing tile reachable by a near-full-rack play from two
// different triple word squares), generalized. This is deliberately NOT a
// new PAT feature or penalty: per Astra (gpt-6-astra)'s review, the sign of
// any correction is unknown up front (summing mutually exclusive reply
// opportunities can overcount; taking the largest can undercount route
// diversity across racks), PAT's worst-unit + gamma * sum combination is
// already sublinear, and the fitted weights may already absorb some
// overlap. So the first deliverable is a detector, a frequency report over
// a stated sampling distribution, and a prespecified +/- reranking
// sensitivity sweep -- enough to apply the stopping rule (does the pattern
// occur in competitively relevant candidate moves, and can a modest
// correction change decisions at all) before any scoring primitive exists.
//
// A "route" is one way the opponent could reach an open triple word square
// along one of its two lanes: either a hook (an empty square on the lane
// whose perpendicular cross set is a real constraint, i.e. a word already
// sits beside it) or a floater (a run of existing tiles on the lane that a
// word reaching the triple would play through). Each route records the
// premium, its lane and direction, the contact square, how many fresh
// tiles a word covering the span from the contact through the premium
// must lay down (the same distance bin pat_scan_unit uses), the unseen
// flexibility of its letter constraint, and the set of existing tiles it
// uses (every run it plays through, plus the perpendicular word beside a
// hook square). Routes are grouped into components by shared existing
// tiles; a component with routes from two or more distinct premiums is a
// "shared anchor" overlap, the mechanism behind NARCEIN. "Shared premium"
// (one premium with live routes from more than one lane/direction) is
// reported separately: current PAT already models that as separate units.
//
// Sampling distribution, stated explicitly: PAT_OVERLAP_NUM_GAMES full
// self-play games from the empty board, both players the frozen champion
// (equity player, pat_dls_champion_v2), every decision point while the bag
// is nonempty (PAT is gated off entirely once the bag is empty, so those
// decisions are out of scope). Positions from the same game are clustered,
// not independent; counts below are positions, with game count reported
// alongside.

#define PAT_OVERLAP_NUM_GAMES 300
#define PAT_OVERLAP_MOVE_LIST_CAPACITY 1000
// Candidates within this many equity points of the best move are examined
// (creation/removal of overlaps, equity gap, reranking); anything further
// back is assumed unaffected, which holds for the sweep below as long as
// |correction| * |overlap delta| stays under this margin.
#define PAT_OVERLAP_EQUITY_MARGIN 10.0
#define PAT_OVERLAP_MAX_CANDIDATES 40
// A shared-anchor overlap is "long" when its easiest route still needs at
// least this many fresh tiles: the near-full-rack shape of the NARCEIN
// example, as opposed to a two-tile hook that any rack can hit.
#define PAT_OVERLAP_LONG_TILES 5
#define PAT_OVERLAP_MAX_ROUTES 2048
#define PAT_OVERLAP_MASK_WORDS ((BOARD_DIM * BOARD_DIM + 63) / 64)
#define PAT_OVERLAP_MAX_PREMIUMS 64
#define PAT_OVERLAP_NUM_STAGES 4
#define PAT_OVERLAP_NUM_CORRECTIONS 8
#define PAT_OVERLAP_MAX_EXAMPLES 8

// Prespecified diagnostic corrections, in equity points per unit of
// shared-anchor overlap a candidate creates (negative values credit
// creating one). Applied by reranking an exhaustively generated list; a
// sensitivity analysis only, not a trained feature.
static const double pat_overlap_corrections[PAT_OVERLAP_NUM_CORRECTIONS] = {
    -5.0, -3.0, -2.0, -1.0, 1.0, 2.0, 3.0, 5.0};

typedef struct OverlapRoute {
  int premium_row;
  int premium_col;
  int dir;
  // -1 / +1 along the lane; 0 for the one-tile hook on the premium itself.
  int side;
  int contact_row;
  int contact_col;
  bool is_floater;
  int tiles_required;
  int flex;
  uint64_t mask[PAT_OVERLAP_MASK_WORDS];
  int component;
} OverlapRoute;

typedef struct OverlapScan {
  OverlapRoute routes[PAT_OVERLAP_MAX_ROUTES];
  int num_routes;
  int num_hooks;
  int num_floaters;
  // Components (routes joined by shared existing tiles) with routes from
  // two or more distinct premiums, and the subset whose easiest route
  // needs at least PAT_OVERLAP_LONG_TILES fresh tiles.
  int shared_anchor_any;
  int shared_anchor_long;
  // Premium squares with live routes from more than one (lane, direction).
  int shared_premium;
  // Over shared-anchor components: the easiest route's tile count, and the
  // easiest route from a second, different premium.
  int anchor_min_tiles_hist[RACK_SIZE + 1];
  int anchor_second_tiles_hist[RACK_SIZE + 1];
  bool component_is_shared[PAT_OVERLAP_MAX_ROUTES];
  // Every floater run the walk reached (recorded as a route or not): how
  // many are dead under the real extension set, and the real vs
  // every-unseen-tile flexibility totals.
  int floater_runs_seen;
  int floater_runs_dead;
  long floater_real_flex_sum;
  long floater_trivial_flex_sum;
} OverlapScan;

static inline void overlap_mask_set(uint64_t *mask, int row, int col) {
  const int bit = row * BOARD_DIM + col;
  mask[bit / 64] |= (uint64_t)1 << (bit % 64);
}

static inline bool overlap_masks_intersect(const uint64_t *a,
                                           const uint64_t *b) {
  for (int i = 0; i < PAT_OVERLAP_MASK_WORDS; i++) {
    if (a[i] & b[i]) {
      return true;
    }
  }
  return false;
}

static inline int overlap_row(int dir, int lane_index, int idx) {
  return (dir == BOARD_HORIZONTAL_DIRECTION) ? lane_index : idx;
}

static inline int overlap_col(int dir, int lane_index, int idx) {
  return (dir == BOARD_HORIZONTAL_DIRECTION) ? idx : lane_index;
}

// Mirrors pat_compute_unseen_counts: tiles neither on the board nor in the
// evaluating player's rack (the opponent's hand plus the bag).
static void overlap_compute_unseen(const Square *lanes,
                                   const LetterDistribution *ld,
                                   const Rack *player_rack,
                                   uint8_t *unseen_counts) {
  memset(unseen_counts, 0, sizeof(uint8_t) * MAX_ALPHABET_SIZE);
  const int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    unseen_counts[ml] = (uint8_t)ld_get_dist(ld, ml);
  }
  for (int row = 0; row < BOARD_DIM; row++) {
    const Square *lane =
        board_get_row_cache(lanes, row, BOARD_HORIZONTAL_DIRECTION);
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter letter = square_get_letter(&lane[col]);
      if (letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      const MachineLetter counted =
          get_is_blanked(letter) ? BLANK_MACHINE_LETTER : letter;
      if (counted < MAX_ALPHABET_SIZE && unseen_counts[counted] > 0) {
        unseen_counts[counted]--;
      }
    }
  }
  for (int ml = 0; ml < ld_size; ml++) {
    const int held = rack_get_letter(player_rack, ml);
    unseen_counts[ml] =
        (uint8_t)((unseen_counts[ml] > held) ? unseen_counts[ml] - held : 0);
  }
}

// Same convention as pat_set_flex: the blank marker at bit 0 is not a
// letter; flexibility counts unseen real tiles that fit the set.
static int overlap_set_flex(const uint8_t *unseen_counts, uint64_t letter_set) {
  int flex = 0;
  for (int ml = 1; ml < MAX_ALPHABET_SIZE; ml++) {
    if (letter_set & ((uint64_t)1 << ml)) {
      flex += unseen_counts[ml];
    }
  }
  return flex;
}

// Adds the run of existing tiles through (row, col) along the direction
// perpendicular to dir -- the word a hook square on a dir lane hooks onto.
static void overlap_mask_add_perpendicular_word(const Square *lanes, int dir,
                                                int row, int col,
                                                uint64_t *mask) {
  const int perp_dir = (dir == BOARD_HORIZONTAL_DIRECTION)
                           ? BOARD_VERTICAL_DIRECTION
                           : BOARD_HORIZONTAL_DIRECTION;
  const int perp_lane_index =
      (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? row : col;
  const int perp_idx = (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? col : row;
  const Square *perp_lane =
      board_get_row_cache(lanes, perp_lane_index, perp_dir);
  for (int side = -1; side <= 1; side += 2) {
    int idx = perp_idx + side;
    while (idx >= 0 && idx < BOARD_DIM &&
           !square_get_is_brick(&perp_lane[idx]) &&
           square_get_letter(&perp_lane[idx]) != ALPHABET_EMPTY_SQUARE_MARKER) {
      overlap_mask_set(mask, overlap_row(perp_dir, perp_lane_index, idx),
                       overlap_col(perp_dir, perp_lane_index, idx));
      idx += side;
    }
  }
}

static OverlapRoute *overlap_new_route(OverlapScan *scan, int premium_row,
                                       int premium_col, int dir, int side,
                                       int contact_row, int contact_col,
                                       bool is_floater, int tiles_required,
                                       int flex) {
  if (scan->num_routes >= PAT_OVERLAP_MAX_ROUTES) {
    return NULL;
  }
  OverlapRoute *route = &scan->routes[scan->num_routes++];
  route->premium_row = premium_row;
  route->premium_col = premium_col;
  route->dir = dir;
  route->side = side;
  route->contact_row = contact_row;
  route->contact_col = contact_col;
  route->is_floater = is_floater;
  route->tiles_required = tiles_required;
  route->flex = flex;
  memset(route->mask, 0, sizeof(route->mask));
  route->component = -1;
  if (is_floater) {
    scan->num_floaters++;
  } else {
    scan->num_hooks++;
  }
  return route;
}

// Walks one (premium, lane) the way pat_scan_unit does with no move overlay,
// recording live routes instead of accumulating features. Reach is capped
// at RACK_SIZE (the bag is nonempty at every in-scope position, so the
// opponent always holds a full rack). A second triple word square within
// reach ends the walk: the triple-triple span is already its own explicit
// PAT feature, not an overlap of independent routes.
static void overlap_scan_premium_lane(const Square *lanes,
                                      const uint8_t *unseen_counts,
                                      int premium_row, int premium_col, int dir,
                                      OverlapScan *scan) {
  const int lane_index =
      (dir == BOARD_HORIZONTAL_DIRECTION) ? premium_row : premium_col;
  const int premium_idx =
      (dir == BOARD_HORIZONTAL_DIRECTION) ? premium_col : premium_row;
  const Square *lane = board_get_row_cache(lanes, lane_index, dir);
  if (square_get_letter(&lane[premium_idx]) != ALPHABET_EMPTY_SQUARE_MARKER) {
    return;
  }
  const uint64_t premium_cross_set = square_get_cross_set(&lane[premium_idx]);
  if (premium_cross_set == 0) {
    return;
  }
  if (premium_cross_set != TRIVIAL_CROSS_SET) {
    const int flex = overlap_set_flex(unseen_counts, premium_cross_set);
    if (flex > 0) {
      OverlapRoute *route =
          overlap_new_route(scan, premium_row, premium_col, dir, 0, premium_row,
                            premium_col, false, 1, flex);
      if (route) {
        overlap_mask_set(route->mask, premium_row, premium_col);
        overlap_mask_add_perpendicular_word(lanes, dir, premium_row,
                                            premium_col, route->mask);
      }
    }
  }
  for (int side = -1; side <= 1; side += 2) {
    int empties_used = 1;
    int prev_empty_idx = premium_idx;
    uint64_t span_mask[PAT_OVERLAP_MASK_WORDS];
    memset(span_mask, 0, sizeof(span_mask));
    int idx = premium_idx + side;
    while (idx >= 0 && idx < BOARD_DIM) {
      if (square_get_is_brick(&lane[idx])) {
        break;
      }
      const MachineLetter letter = square_get_letter(&lane[idx]);
      if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
        const int distance_bin = empties_used;
        const int facing_idx = idx;
        while (idx >= 0 && idx < BOARD_DIM &&
               !square_get_is_brick(&lane[idx]) &&
               square_get_letter(&lane[idx]) != ALPHABET_EMPTY_SQUARE_MARKER) {
          overlap_mask_set(span_mask, overlap_row(dir, lane_index, idx),
                           overlap_col(dir, lane_index, idx));
          idx += side;
        }
        // The letters that could extend this run toward the premium. Per
        // game_gen_cross_set (and move_gen's own reading of these fields),
        // an empty square's left_extension_set holds the letters that can
        // go immediately left of the run to its RIGHT, and a run's
        // rightmost tile holds the run's own left/right extension sets;
        // an empty square's right_extension_set is never written. So a
        // run on the far side of prev_empty (side > 0) is read from
        // prev_empty's left set, and a run on the near side (side < 0)
        // from its rightmost tile's right set, i.e. the tile at
        // prev_empty_idx - 1. (pat_scan_unit reads
        // right_extension_set(prev_empty) / left_extension_set(prev_empty)
        // respectively, which are the unwritten field and the wrong run's
        // field: the shipped floater flex is effectively "every unseen
        // tile" for every floater. trivial_flex below is that value, so
        // the report can quantify the gap.)
        const uint64_t extension_set =
            (side > 0)
                ? square_get_left_extension_set(&lane[prev_empty_idx])
                : square_get_right_extension_set(&lane[prev_empty_idx - 1]);
        const int flex = overlap_set_flex(unseen_counts, extension_set);
        scan->floater_runs_seen++;
        scan->floater_trivial_flex_sum +=
            overlap_set_flex(unseen_counts, TRIVIAL_CROSS_SET);
        scan->floater_real_flex_sum += flex;
        if (flex == 0) {
          scan->floater_runs_dead++;
        }
        if (flex > 0) {
          OverlapRoute *route =
              overlap_new_route(scan, premium_row, premium_col, dir, side,
                                overlap_row(dir, lane_index, facing_idx),
                                overlap_col(dir, lane_index, facing_idx), true,
                                distance_bin, flex);
          if (route) {
            memcpy(route->mask, span_mask, sizeof(span_mask));
          }
        }
        continue;
      }
      const uint64_t cross_set = square_get_cross_set(&lane[idx]);
      if (cross_set == 0) {
        break;
      }
      empties_used++;
      if (empties_used > RACK_SIZE) {
        break;
      }
      if (bonus_square_get_word_multiplier(
              square_get_bonus_square(&lane[idx])) == 3) {
        break;
      }
      if (cross_set != TRIVIAL_CROSS_SET) {
        const int flex = overlap_set_flex(unseen_counts, cross_set);
        if (flex > 0) {
          const int row = overlap_row(dir, lane_index, idx);
          const int col = overlap_col(dir, lane_index, idx);
          OverlapRoute *route =
              overlap_new_route(scan, premium_row, premium_col, dir, side, row,
                                col, false, empties_used, flex);
          if (route) {
            memcpy(route->mask, span_mask, sizeof(span_mask));
            overlap_mask_set(route->mask, row, col);
            overlap_mask_add_perpendicular_word(lanes, dir, row, col,
                                                route->mask);
          }
        }
      }
      prev_empty_idx = idx;
      idx += side;
    }
  }
}

static int overlap_find_root(int *parent, int i) {
  while (parent[i] != i) {
    parent[i] = parent[parent[i]];
    i = parent[i];
  }
  return i;
}

static int overlap_compare_ints(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

// Groups routes into components by shared existing tiles and fills in the
// per-position summary counts.
static void overlap_summarize(OverlapScan *scan) {
  int parent[PAT_OVERLAP_MAX_ROUTES];
  for (int i = 0; i < scan->num_routes; i++) {
    parent[i] = i;
  }
  for (int i = 0; i < scan->num_routes; i++) {
    for (int j = i + 1; j < scan->num_routes; j++) {
      if (overlap_masks_intersect(scan->routes[i].mask, scan->routes[j].mask)) {
        const int ri = overlap_find_root(parent, i);
        const int rj = overlap_find_root(parent, j);
        if (ri != rj) {
          parent[ri] = rj;
        }
      }
    }
  }
  for (int i = 0; i < scan->num_routes; i++) {
    scan->routes[i].component = overlap_find_root(parent, i);
    scan->component_is_shared[i] = false;
  }
  scan->shared_anchor_any = 0;
  scan->shared_anchor_long = 0;
  memset(scan->anchor_min_tiles_hist, 0, sizeof(scan->anchor_min_tiles_hist));
  memset(scan->anchor_second_tiles_hist, 0,
         sizeof(scan->anchor_second_tiles_hist));
  for (int root = 0; root < scan->num_routes; root++) {
    if (scan->routes[root].component != root) {
      continue;
    }
    // Per distinct premium in this component, its easiest route.
    int premium_ids[PAT_OVERLAP_MAX_PREMIUMS];
    int premium_min_tiles[PAT_OVERLAP_MAX_PREMIUMS];
    int num_premiums = 0;
    for (int i = 0; i < scan->num_routes; i++) {
      const OverlapRoute *route = &scan->routes[i];
      if (route->component != root) {
        continue;
      }
      const int premium_id =
          route->premium_row * BOARD_DIM + route->premium_col;
      int slot = -1;
      for (int p = 0; p < num_premiums; p++) {
        if (premium_ids[p] == premium_id) {
          slot = p;
          break;
        }
      }
      if (slot < 0) {
        if (num_premiums == PAT_OVERLAP_MAX_PREMIUMS) {
          continue;
        }
        slot = num_premiums++;
        premium_ids[slot] = premium_id;
        premium_min_tiles[slot] = route->tiles_required;
      } else if (route->tiles_required < premium_min_tiles[slot]) {
        premium_min_tiles[slot] = route->tiles_required;
      }
    }
    if (num_premiums < 2) {
      continue;
    }
    qsort(premium_min_tiles, num_premiums, sizeof(int), overlap_compare_ints);
    scan->shared_anchor_any++;
    scan->component_is_shared[root] = true;
    scan->anchor_min_tiles_hist[premium_min_tiles[0]]++;
    scan->anchor_second_tiles_hist[premium_min_tiles[1]]++;
    if (premium_min_tiles[0] >= PAT_OVERLAP_LONG_TILES) {
      scan->shared_anchor_long++;
    }
  }
  // Shared premium: distinct (lane, direction) combinations per premium.
  scan->shared_premium = 0;
  int premium_ids[PAT_OVERLAP_MAX_PREMIUMS];
  int premium_combo_masks[PAT_OVERLAP_MAX_PREMIUMS];
  int num_premiums = 0;
  for (int i = 0; i < scan->num_routes; i++) {
    const OverlapRoute *route = &scan->routes[i];
    const int premium_id = route->premium_row * BOARD_DIM + route->premium_col;
    const int combo_bit = 1 << (route->dir * 3 + (route->side + 1));
    int slot = -1;
    for (int p = 0; p < num_premiums; p++) {
      if (premium_ids[p] == premium_id) {
        slot = p;
        break;
      }
    }
    if (slot < 0) {
      if (num_premiums == PAT_OVERLAP_MAX_PREMIUMS) {
        continue;
      }
      slot = num_premiums++;
      premium_ids[slot] = premium_id;
      premium_combo_masks[slot] = 0;
    }
    premium_combo_masks[slot] |= combo_bit;
  }
  for (int p = 0; p < num_premiums; p++) {
    if (__builtin_popcount((unsigned)premium_combo_masks[p]) >= 2) {
      scan->shared_premium++;
    }
  }
}

// Scans the game's current board from mover_index's point of view (their
// rack is what unseen counts exclude). Only triple word squares are
// scanned: the NARCEIN pattern is a triple pattern, and PAT's richer
// channels are triple-only too.
static void overlap_scan_game(const Game *game, int mover_index,
                              OverlapScan *scan) {
  scan->num_routes = 0;
  scan->num_hooks = 0;
  scan->num_floaters = 0;
  scan->floater_runs_seen = 0;
  scan->floater_runs_dead = 0;
  scan->floater_real_flex_sum = 0;
  scan->floater_trivial_flex_sum = 0;
  const Board *board = game_get_board(game);
  assert(!board_get_transposed(board));
  assert(board_get_cross_sets_valid(board));
  const int cross_set_index = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), mover_index);
  const Square *lanes = board_get_readonly_lanes(board, cross_set_index);
  uint8_t unseen_counts[MAX_ALPHABET_SIZE];
  overlap_compute_unseen(lanes, game_get_ld(game),
                         player_get_rack(game_get_player(game, mover_index)),
                         unseen_counts);
  for (int row = 0; row < BOARD_DIM; row++) {
    const Square *lane =
        board_get_row_cache(lanes, row, BOARD_HORIZONTAL_DIRECTION);
    for (int col = 0; col < BOARD_DIM; col++) {
      const Square *square = &lane[col];
      if (square_get_is_brick(square) ||
          square_get_letter(square) != ALPHABET_EMPTY_SQUARE_MARKER ||
          bonus_square_get_word_multiplier(square_get_bonus_square(square)) !=
              3) {
        continue;
      }
      overlap_scan_premium_lane(lanes, unseen_counts, row, col,
                                BOARD_HORIZONTAL_DIRECTION, scan);
      overlap_scan_premium_lane(lanes, unseen_counts, row, col,
                                BOARD_VERTICAL_DIRECTION, scan);
    }
  }
  overlap_summarize(scan);
}

static void overlap_print_square(int row, int col) {
  printf("%c%d", 'A' + col, row + 1);
}

static void overlap_print_shared_routes(const OverlapScan *scan) {
  for (int i = 0; i < scan->num_routes; i++) {
    const OverlapRoute *route = &scan->routes[i];
    if (!scan->component_is_shared[route->component]) {
      continue;
    }
    printf("    component %d: %s from TWS ", route->component,
           route->is_floater ? "floater" : "hook   ");
    overlap_print_square(route->premium_row, route->premium_col);
    printf(" %s%s contact ",
           route->dir == BOARD_HORIZONTAL_DIRECTION ? "row" : "col",
           route->side < 0 ? "-" : (route->side > 0 ? "+" : " "));
    overlap_print_square(route->contact_row, route->contact_col);
    printf(" tiles %d flex %d\n", route->tiles_required, route->flex);
  }
}

// Synthetic checks of the detector on hand-built boards. These verify the
// mechanism, not anything about natural prevalence.
static void test_pat_overlap_detector(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-pat pat_dls_champion_v2");
  // A lone E on the center square: reachable by a 7-tile play from all
  // four center-lane triples, every route through the same tile.
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/7E7/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  OverlapScan *scan = malloc_or_die(sizeof(OverlapScan));
  overlap_scan_game(game, 0, scan);
  printf("lone E at H8: %d routes (%d hooks, %d floaters), shared anchor "
         "any %d long %d, shared premium %d\n",
         scan->num_routes, scan->num_hooks, scan->num_floaters,
         scan->shared_anchor_any, scan->shared_anchor_long,
         scan->shared_premium);
  overlap_print_shared_routes(scan);
  assert(scan->num_routes == 4);
  assert(scan->num_floaters == 4);
  assert(scan->shared_anchor_any == 1);
  assert(scan->shared_anchor_long == 1);
  assert(scan->anchor_min_tiles_hist[7] == 1);
  assert(scan->anchor_second_tiles_hist[7] == 1);
  assert(scan->shared_premium == 0);
  for (int i = 0; i < scan->num_routes; i++) {
    assert(scan->routes[i].contact_row == 7);
    assert(scan->routes[i].contact_col == 7);
    assert(scan->routes[i].tiles_required == 7);
  }

  // A lone E at C8: two tiles from the A8 triple along its row, out of
  // reach of every other triple. One route, one premium, no overlap.
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/2E12/15/15/15/15/15/15/15 / 0/0 0");
  overlap_scan_game(game, 0, scan);
  printf("lone E at C8: %d routes, shared anchor any %d, shared premium "
         "%d\n",
         scan->num_routes, scan->shared_anchor_any, scan->shared_premium);
  assert(scan->num_routes == 1);
  assert(scan->routes[0].tiles_required == 2);
  assert(scan->routes[0].premium_row == 7 && scan->routes[0].premium_col == 0);
  assert(scan->shared_anchor_any == 0);
  assert(scan->shared_premium == 0);

  // The motivating example. Nothing precedes NARCEIN in CSW21, so the
  // route from A8 is dead; NARCEINE/NARCEINS exist, so the route from O8
  // is live with a two-letter extension set; both vertical routes through
  // the E are 7-tile floaters. All three share the NARCEIN tiles.
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/3NARCEIN5/15/15/15/15/15/15/15 / 0/0 0");
  overlap_scan_game(game, 0, scan);
  printf("NARCEIN at 8D: %d routes (%d hooks, %d floaters), shared anchor "
         "any %d long %d, shared premium %d\n",
         scan->num_routes, scan->num_hooks, scan->num_floaters,
         scan->shared_anchor_any, scan->shared_anchor_long,
         scan->shared_premium);
  overlap_print_shared_routes(scan);
  int vertical_routes_through_e = 0;
  for (int i = 0; i < scan->num_routes; i++) {
    const OverlapRoute *route = &scan->routes[i];
    if (route->dir == BOARD_VERTICAL_DIRECTION && route->contact_row == 7 &&
        route->contact_col == 7 && route->tiles_required == 7) {
      vertical_routes_through_e++;
    }
  }
  assert(vertical_routes_through_e == 2);
  assert(scan->num_routes == 3);
  assert(scan->floater_runs_seen == 4);
  assert(scan->floater_runs_dead == 1);
  assert(scan->shared_anchor_any == 1);
  assert(scan->shared_anchor_long == 1);
  assert(scan->anchor_min_tiles_hist[5] == 1);
  assert(scan->anchor_second_tiles_hist[7] == 1);

  free(scan);
  config_destroy(config);
}

typedef struct OverlapStats {
  int games;
  int positions;
  int positions_by_stage[PAT_OVERLAP_NUM_STAGES];
  long total_routes;
  long total_hooks;
  long total_floaters;
  long floater_runs_seen;
  long floater_runs_dead;
  long floater_real_flex_sum;
  long floater_trivial_flex_sum;
  int pos_with_anchor_any;
  int pos_with_anchor_long;
  int pos_with_shared_premium;
  int pos_with_anchor_any_by_stage[PAT_OVERLAP_NUM_STAGES];
  int pos_with_anchor_long_by_stage[PAT_OVERLAP_NUM_STAGES];
  long anchor_min_tiles_hist[RACK_SIZE + 1];
  long anchor_second_tiles_hist[RACK_SIZE + 1];
  long candidates_examined;
  long candidates_changing_any;
  long candidates_changing_long;
  long candidates_creating_any;
  long candidates_removing_any;
  long candidates_creating_long;
  long candidates_removing_long;
  int pos_with_candidate_changing_any;
  int pos_with_candidate_changing_long;
  int pos_best_changes_any;
  int pos_best_changes_long;
  // A non-best candidate within 1, 2, 5 equity of the best move changes
  // the overlap count relative to the pre-move board.
  int pos_close_candidate_changing_any[3];
  int pos_close_candidate_changing_long[3];
  int rerank_changes_any[PAT_OVERLAP_NUM_CORRECTIONS];
  int rerank_changes_long[PAT_OVERLAP_NUM_CORRECTIONS];
  int examples_printed;
} OverlapStats;

static const double pat_overlap_close_gaps[3] = {1.0, 2.0, 5.0};

static int overlap_stage_of(int bag_letters) {
  if (bag_letters >= 60) {
    return 0;
  }
  if (bag_letters >= 40) {
    return 1;
  }
  if (bag_letters >= 20) {
    return 2;
  }
  return 3;
}

static void overlap_print_move(const Game *game, const Move *move) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          true);
  printf("%s", string_builder_peek(sb));
  string_builder_destroy(sb);
}

static void overlap_analyze_position(Game *game, MoveList *move_list,
                                     OverlapScan *pre_scan,
                                     OverlapScan *post_scan,
                                     OverlapStats *stats) {
  const int mover_index = game_get_player_on_turn_index(game);
  const int stage = overlap_stage_of(bag_get_letters(game_get_bag(game)));
  overlap_scan_game(game, mover_index, pre_scan);
  stats->positions++;
  stats->positions_by_stage[stage]++;
  stats->total_routes += pre_scan->num_routes;
  stats->total_hooks += pre_scan->num_hooks;
  stats->total_floaters += pre_scan->num_floaters;
  stats->floater_runs_seen += pre_scan->floater_runs_seen;
  stats->floater_runs_dead += pre_scan->floater_runs_dead;
  stats->floater_real_flex_sum += pre_scan->floater_real_flex_sum;
  stats->floater_trivial_flex_sum += pre_scan->floater_trivial_flex_sum;
  if (pre_scan->shared_anchor_any > 0) {
    stats->pos_with_anchor_any++;
    stats->pos_with_anchor_any_by_stage[stage]++;
  }
  if (pre_scan->shared_anchor_long > 0) {
    stats->pos_with_anchor_long++;
    stats->pos_with_anchor_long_by_stage[stage]++;
  }
  if (pre_scan->shared_premium > 0) {
    stats->pos_with_shared_premium++;
  }
  for (int t = 0; t <= RACK_SIZE; t++) {
    stats->anchor_min_tiles_hist[t] += pre_scan->anchor_min_tiles_hist[t];
    stats->anchor_second_tiles_hist[t] += pre_scan->anchor_second_tiles_hist[t];
  }

  const int num_moves = move_list_get_count(move_list);
  const double best_equity =
      equity_to_double(move_get_equity(move_list_get_move(move_list, 0)));
  double equities[PAT_OVERLAP_MAX_CANDIDATES];
  int delta_any[PAT_OVERLAP_MAX_CANDIDATES];
  int delta_long[PAT_OVERLAP_MAX_CANDIDATES];
  int num_candidates = 0;
  bool any_changes = false;
  bool long_changes = false;
  bool close_any[3] = {false, false, false};
  bool close_long[3] = {false, false, false};
  game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
  for (int i = 0; i < num_moves && num_candidates < PAT_OVERLAP_MAX_CANDIDATES;
       i++) {
    const Move *move = move_list_get_move(move_list, i);
    const double equity = equity_to_double(move_get_equity(move));
    const double gap = best_equity - equity;
    if (gap > PAT_OVERLAP_EQUITY_MARGIN) {
      break;
    }
    int d_any = 0;
    int d_long = 0;
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      play_move_without_drawing_tiles(move, game);
      overlap_scan_game(game, mover_index, post_scan);
      game_unplay_last_move(game);
      d_any = post_scan->shared_anchor_any - pre_scan->shared_anchor_any;
      d_long = post_scan->shared_anchor_long - pre_scan->shared_anchor_long;
    }
    equities[num_candidates] = equity;
    delta_any[num_candidates] = d_any;
    delta_long[num_candidates] = d_long;
    num_candidates++;
    stats->candidates_examined++;
    if (d_any != 0) {
      stats->candidates_changing_any++;
      any_changes = true;
      if (d_any > 0) {
        stats->candidates_creating_any++;
      } else {
        stats->candidates_removing_any++;
      }
      if (i == 0) {
        stats->pos_best_changes_any++;
      }
      for (int g = 0; g < 3; g++) {
        if (i > 0 && gap <= pat_overlap_close_gaps[g]) {
          close_any[g] = true;
        }
      }
    }
    if (d_long != 0) {
      stats->candidates_changing_long++;
      long_changes = true;
      if (d_long > 0) {
        stats->candidates_creating_long++;
      } else {
        stats->candidates_removing_long++;
      }
      if (i == 0) {
        stats->pos_best_changes_long++;
      }
      for (int g = 0; g < 3; g++) {
        if (i > 0 && gap <= pat_overlap_close_gaps[g]) {
          close_long[g] = true;
        }
      }
      if (d_long > 0 && gap <= 3.0 &&
          stats->examples_printed < PAT_OVERLAP_MAX_EXAMPLES) {
        stats->examples_printed++;
        char *cgp = game_get_cgp(game, true);
        printf("\nexample %d (bag %d): %s\n  best ", stats->examples_printed,
               bag_get_letters(game_get_bag(game)), cgp);
        free(cgp);
        overlap_print_move(game, move_list_get_move(move_list, 0));
        printf(" eq %.2f\n  candidate ", best_equity);
        overlap_print_move(game, move);
        printf(" eq %.2f (gap %.2f) creates a long shared-anchor overlap "
               "(pre %d -> post %d):\n",
               equity, gap, pre_scan->shared_anchor_long,
               post_scan->shared_anchor_long);
        overlap_print_shared_routes(post_scan);
      }
    }
  }
  game_set_backup_mode(game, BACKUP_MODE_OFF);
  if (any_changes) {
    stats->pos_with_candidate_changing_any++;
  }
  if (long_changes) {
    stats->pos_with_candidate_changing_long++;
  }
  for (int g = 0; g < 3; g++) {
    if (close_any[g]) {
      stats->pos_close_candidate_changing_any[g]++;
    }
    if (close_long[g]) {
      stats->pos_close_candidate_changing_long[g]++;
    }
  }
  // Reranking sensitivity: does any prespecified correction change the
  // argmax over the examined candidates? Ties keep the original best.
  for (int c = 0; c < PAT_OVERLAP_NUM_CORRECTIONS; c++) {
    const double correction = pat_overlap_corrections[c];
    for (int metric = 0; metric < 2; metric++) {
      const int *deltas = (metric == 0) ? delta_any : delta_long;
      int best_index = 0;
      double best_adjusted = equities[0] - correction * deltas[0];
      for (int i = 1; i < num_candidates; i++) {
        const double adjusted = equities[i] - correction * deltas[i];
        if (adjusted > best_adjusted) {
          best_adjusted = adjusted;
          best_index = i;
        }
      }
      if (best_index != 0) {
        if (metric == 0) {
          stats->rerank_changes_any[c]++;
        } else {
          stats->rerank_changes_long[c]++;
        }
      }
    }
  }
}

static void overlap_print_pct(const char *label, long numerator,
                              long denominator) {
  printf("  %s: %ld / %ld (%.2f%%)\n", label, numerator, denominator,
         denominator > 0 ? 100.0 * numerator / denominator : 0.0);
}

void test_pat_overlap_pilot(void) {
  test_pat_overlap_detector();

  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-pat pat_dls_champion_v2");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create(PAT_OVERLAP_MOVE_LIST_CAPACITY);
  OverlapScan *pre_scan = malloc_or_die(sizeof(OverlapScan));
  OverlapScan *post_scan = malloc_or_die(sizeof(OverlapScan));
  OverlapStats *stats = malloc_or_die(sizeof(OverlapStats));
  memset(stats, 0, sizeof(OverlapStats));

  for (int game_index = 0; game_index < PAT_OVERLAP_NUM_GAMES; game_index++) {
    game_reset(game);
    game_seed(game, 900000000ULL + (uint64_t)game_index);
    draw_starting_racks(game);
    stats->games++;
    while (game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
           bag_get_letters(game_get_bag(game)) > 0) {
      const MoveGenArgs args = {
          .game = game,
          .move_list = move_list,
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&args);
      move_list_sort_moves(move_list);
      if (move_list_get_count(move_list) == 0) {
        break;
      }
      overlap_analyze_position(game, move_list, pre_scan, post_scan, stats);
      Move best;
      move_copy(&best, move_list_get_move(move_list, 0));
      play_move(&best, game, NULL);
    }
  }

  printf("\noverlap pilot: %d games, %d in-scope positions (bag nonempty)\n",
         stats->games, stats->positions);
  printf("  routes per position: %.2f (%.2f hooks, %.2f floaters)\n",
         (double)stats->total_routes / stats->positions,
         (double)stats->total_hooks / stats->positions,
         (double)stats->total_floaters / stats->positions);
  overlap_print_pct("floater runs dead under the real extension set "
                    "(shipped pat.c treats every one as fully open)",
                    stats->floater_runs_dead, stats->floater_runs_seen);
  printf("  floater flex, real / every-unseen-tile: %ld / %ld (%.1f%%)\n",
         stats->floater_real_flex_sum, stats->floater_trivial_flex_sum,
         stats->floater_trivial_flex_sum > 0
             ? 100.0 * stats->floater_real_flex_sum /
                   stats->floater_trivial_flex_sum
             : 0.0);
  overlap_print_pct("positions with a shared-anchor overlap (any)",
                    stats->pos_with_anchor_any, stats->positions);
  overlap_print_pct("positions with a long shared-anchor overlap (easiest "
                    "route >= 5 tiles)",
                    stats->pos_with_anchor_long, stats->positions);
  overlap_print_pct("positions with a shared-premium overlap",
                    stats->pos_with_shared_premium, stats->positions);
  static const char *const stage_names[PAT_OVERLAP_NUM_STAGES] = {
      "bag >= 60", "bag 40-59", "bag 20-39", "bag 1-19"};
  for (int s = 0; s < PAT_OVERLAP_NUM_STAGES; s++) {
    printf("  %s: %d positions, shared-anchor any %.2f%%, long %.2f%%\n",
           stage_names[s], stats->positions_by_stage[s],
           stats->positions_by_stage[s] > 0
               ? 100.0 * stats->pos_with_anchor_any_by_stage[s] /
                     stats->positions_by_stage[s]
               : 0.0,
           stats->positions_by_stage[s] > 0
               ? 100.0 * stats->pos_with_anchor_long_by_stage[s] /
                     stats->positions_by_stage[s]
               : 0.0);
  }
  printf("  shared-anchor components by easiest route's tiles:");
  for (int t = 1; t <= RACK_SIZE; t++) {
    printf(" %d:%ld", t, stats->anchor_min_tiles_hist[t]);
  }
  printf("\n  ... by the second premium's easiest route's tiles:");
  for (int t = 1; t <= RACK_SIZE; t++) {
    printf(" %d:%ld", t, stats->anchor_second_tiles_hist[t]);
  }
  printf("\ncandidates within %.0f equity of best (cap %d per position):\n",
         PAT_OVERLAP_EQUITY_MARGIN, PAT_OVERLAP_MAX_CANDIDATES);
  overlap_print_pct("candidates changing shared-anchor count (any)",
                    stats->candidates_changing_any, stats->candidates_examined);
  printf("    creating %ld, removing %ld\n", stats->candidates_creating_any,
         stats->candidates_removing_any);
  overlap_print_pct("candidates changing long shared-anchor count",
                    stats->candidates_changing_long,
                    stats->candidates_examined);
  printf("    creating %ld, removing %ld\n", stats->candidates_creating_long,
         stats->candidates_removing_long);
  overlap_print_pct("positions with any examined candidate changing (any)",
                    stats->pos_with_candidate_changing_any, stats->positions);
  overlap_print_pct("positions with any examined candidate changing (long)",
                    stats->pos_with_candidate_changing_long, stats->positions);
  overlap_print_pct("positions where the best move itself changes (any)",
                    stats->pos_best_changes_any, stats->positions);
  overlap_print_pct("positions where the best move itself changes (long)",
                    stats->pos_best_changes_long, stats->positions);
  for (int g = 0; g < 3; g++) {
    char label[128];
    snprintf(label, sizeof(label),
             "positions with a non-best candidate within %.0f eq changing "
             "(any)",
             pat_overlap_close_gaps[g]);
    overlap_print_pct(label, stats->pos_close_candidate_changing_any[g],
                      stats->positions);
    snprintf(label, sizeof(label),
             "positions with a non-best candidate within %.0f eq changing "
             "(long)",
             pat_overlap_close_gaps[g]);
    overlap_print_pct(label, stats->pos_close_candidate_changing_long[g],
                      stats->positions);
  }
  printf("reranking sensitivity (positions whose argmax changes; "
         "correction is equity per created overlap, negative = credit):\n");
  for (int c = 0; c < PAT_OVERLAP_NUM_CORRECTIONS; c++) {
    printf("  correction %+.0f: any %d (%.2f%%), long %d (%.2f%%)\n",
           pat_overlap_corrections[c], stats->rerank_changes_any[c],
           100.0 * stats->rerank_changes_any[c] / stats->positions,
           stats->rerank_changes_long[c],
           100.0 * stats->rerank_changes_long[c] / stats->positions);
  }

  assert(stats->positions > 0);
  free(stats);
  free(pre_scan);
  free(post_scan);
  move_list_destroy(move_list);
  config_destroy(config);
}
