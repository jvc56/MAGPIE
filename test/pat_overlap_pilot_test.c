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
#include "../src/ent/bonus_square.h"
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
// The narrow measure's minimum fresh tiles per qualifying route: the
// near-full-rack, opposing-reach shape of the NARCEIN example.
#define PAT_OVERLAP_NARROW_MIN_TILES 5
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
  // Floater routes only: the run's lane index span, and whether it is the
  // first run the walk from the premium reached (a word using a later run
  // must play through this one too, so only the first is "direct").
  int run_lo;
  int run_hi;
  bool is_direct;
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
  // The narrow, prespecified measure (see overlap_summarize): runs that
  // are the direct contact of lexicon-feasible floater routes needing at
  // least PAT_OVERLAP_NARROW_MIN_TILES fresh tiles from two distinct
  // premiums along the run's own lane, counted as max(0, k - 1) distinct
  // premiums per run.
  int narrow;
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
  route->run_lo = -1;
  route->run_hi = -1;
  route->is_direct = false;
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
    int runs_passed = 0;
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
        int run_far_idx = idx;
        while (idx >= 0 && idx < BOARD_DIM &&
               !square_get_is_brick(&lane[idx]) &&
               square_get_letter(&lane[idx]) != ALPHABET_EMPTY_SQUARE_MARKER) {
          overlap_mask_set(span_mask, overlap_row(dir, lane_index, idx),
                           overlap_col(dir, lane_index, idx));
          run_far_idx = idx;
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
            route->run_lo =
                (facing_idx < run_far_idx) ? facing_idx : run_far_idx;
            route->run_hi =
                (facing_idx < run_far_idx) ? run_far_idx : facing_idx;
            route->is_direct = (runs_passed == 0);
          }
        }
        runs_passed++;
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
  // Narrow measure: no transitive grouping. Two routes count together
  // only when their direct contact run is the same run in the same lane
  // (so they approach it from opposite ends of that lane), each needs
  // PAT_OVERLAP_NARROW_MIN_TILES+ fresh tiles, and each is lexicon-
  // feasible (every recorded route already has flex > 0). Distinct
  // premiums per run, in excess of one.
  scan->narrow = 0;
  for (int i = 0; i < scan->num_routes; i++) {
    const OverlapRoute *anchor = &scan->routes[i];
    if (!anchor->is_floater || !anchor->is_direct ||
        anchor->tiles_required < PAT_OVERLAP_NARROW_MIN_TILES) {
      continue;
    }
    // Count each run once, from its lowest-index qualifying route.
    bool first = true;
    int distinct_premiums = 0;
    int seen_premium_ids[PAT_OVERLAP_MAX_PREMIUMS];
    for (int j = 0; j < scan->num_routes; j++) {
      const OverlapRoute *route = &scan->routes[j];
      if (!route->is_floater || !route->is_direct ||
          route->tiles_required < PAT_OVERLAP_NARROW_MIN_TILES ||
          route->dir != anchor->dir || route->run_lo != anchor->run_lo ||
          route->run_hi != anchor->run_hi) {
        continue;
      }
      const int route_lane = (route->dir == BOARD_HORIZONTAL_DIRECTION)
                                 ? route->contact_row
                                 : route->contact_col;
      const int anchor_lane = (anchor->dir == BOARD_HORIZONTAL_DIRECTION)
                                  ? anchor->contact_row
                                  : anchor->contact_col;
      if (route_lane != anchor_lane) {
        continue;
      }
      if (j < i) {
        first = false;
        break;
      }
      const int premium_id =
          route->premium_row * BOARD_DIM + route->premium_col;
      bool seen = false;
      for (int p = 0; p < distinct_premiums; p++) {
        if (seen_premium_ids[p] == premium_id) {
          seen = true;
          break;
        }
      }
      if (!seen && distinct_premiums < PAT_OVERLAP_MAX_PREMIUMS) {
        seen_premium_ids[distinct_premiums++] = premium_id;
      }
    }
    if (first && distinct_premiums > 1) {
      scan->narrow += distinct_premiums - 1;
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
  // Narrow: the vertical pair shares the run {H8} in column H, the
  // horizontal pair shares it in row 8: two qualifying runs.
  assert(scan->narrow == 2);
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
  assert(scan->narrow == 0);

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
  // Narrow: only the vertical pair through the E qualifies (the A8 route
  // is dead, so the NARCEIN run has one feasible direct route, not two).
  assert(scan->narrow == 1);

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
  int pos_with_narrow;
  int pos_with_narrow_by_stage[PAT_OVERLAP_NUM_STAGES];
  long narrow_sum;
  int pos_with_anchor_any_by_stage[PAT_OVERLAP_NUM_STAGES];
  int pos_with_anchor_long_by_stage[PAT_OVERLAP_NUM_STAGES];
  long anchor_min_tiles_hist[RACK_SIZE + 1];
  long anchor_second_tiles_hist[RACK_SIZE + 1];
  long candidates_examined;
  long candidates_changing_any;
  long candidates_changing_long;
  long candidates_changing_narrow;
  long candidates_creating_narrow;
  long candidates_removing_narrow;
  int pos_with_candidate_changing_narrow;
  int pos_best_changes_narrow;
  int pos_close_candidate_changing_narrow[3];
  int rerank_changes_narrow[PAT_OVERLAP_NUM_CORRECTIONS];
  int narrow_examples_printed;
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
  if (pre_scan->narrow > 0) {
    stats->pos_with_narrow++;
    stats->pos_with_narrow_by_stage[stage]++;
  }
  stats->narrow_sum += pre_scan->narrow;
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
  int delta_narrow[PAT_OVERLAP_MAX_CANDIDATES];
  int num_candidates = 0;
  bool any_changes = false;
  bool long_changes = false;
  bool narrow_changes = false;
  bool close_any[3] = {false, false, false};
  bool close_long[3] = {false, false, false};
  bool close_narrow[3] = {false, false, false};
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
    int d_narrow = 0;
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      play_move_without_drawing_tiles(move, game);
      overlap_scan_game(game, mover_index, post_scan);
      game_unplay_last_move(game);
      d_any = post_scan->shared_anchor_any - pre_scan->shared_anchor_any;
      d_long = post_scan->shared_anchor_long - pre_scan->shared_anchor_long;
      d_narrow = post_scan->narrow - pre_scan->narrow;
    }
    equities[num_candidates] = equity;
    delta_any[num_candidates] = d_any;
    delta_long[num_candidates] = d_long;
    delta_narrow[num_candidates] = d_narrow;
    num_candidates++;
    stats->candidates_examined++;
    if (d_narrow != 0) {
      stats->candidates_changing_narrow++;
      narrow_changes = true;
      if (d_narrow > 0) {
        stats->candidates_creating_narrow++;
      } else {
        stats->candidates_removing_narrow++;
      }
      if (i == 0) {
        stats->pos_best_changes_narrow++;
      }
      for (int g = 0; g < 3; g++) {
        if (i > 0 && gap <= pat_overlap_close_gaps[g]) {
          close_narrow[g] = true;
        }
      }
      // Examples for manual inspection of the narrow definition: a
      // candidate within 3 equity of best that creates one.
      if (d_narrow > 0 && gap <= 3.0 &&
          stats->narrow_examples_printed < PAT_OVERLAP_MAX_EXAMPLES) {
        stats->narrow_examples_printed++;
        char *cgp = game_get_cgp(game, true);
        printf("\nnarrow example %d (bag %d): %s\n  best ",
               stats->narrow_examples_printed,
               bag_get_letters(game_get_bag(game)), cgp);
        free(cgp);
        overlap_print_move(game, move_list_get_move(move_list, 0));
        printf(" eq %.2f\n  candidate ", best_equity);
        overlap_print_move(game, move);
        printf(" eq %.2f (gap %.2f) narrow %d -> %d; direct floater routes "
               "needing >= %d tiles on the resulting board:\n",
               equity, gap, pre_scan->narrow, post_scan->narrow,
               PAT_OVERLAP_NARROW_MIN_TILES);
        for (int r = 0; r < post_scan->num_routes; r++) {
          const OverlapRoute *route = &post_scan->routes[r];
          if (!route->is_floater || !route->is_direct ||
              route->tiles_required < PAT_OVERLAP_NARROW_MIN_TILES) {
            continue;
          }
          printf("    from TWS ");
          overlap_print_square(route->premium_row, route->premium_col);
          printf(" %s%s run ",
                 route->dir == BOARD_HORIZONTAL_DIRECTION ? "row" : "col",
                 route->side < 0 ? "-" : "+");
          overlap_print_square(
              overlap_row(route->dir,
                          route->dir == BOARD_HORIZONTAL_DIRECTION
                              ? route->contact_row
                              : route->contact_col,
                          route->run_lo),
              overlap_col(route->dir,
                          route->dir == BOARD_HORIZONTAL_DIRECTION
                              ? route->contact_row
                              : route->contact_col,
                          route->run_lo));
          printf("-");
          overlap_print_square(
              overlap_row(route->dir,
                          route->dir == BOARD_HORIZONTAL_DIRECTION
                              ? route->contact_row
                              : route->contact_col,
                          route->run_hi),
              overlap_col(route->dir,
                          route->dir == BOARD_HORIZONTAL_DIRECTION
                              ? route->contact_row
                              : route->contact_col,
                          route->run_hi));
          printf(" tiles %d flex %d\n", route->tiles_required, route->flex);
        }
      }
    }
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
  if (narrow_changes) {
    stats->pos_with_candidate_changing_narrow++;
  }
  for (int g = 0; g < 3; g++) {
    if (close_any[g]) {
      stats->pos_close_candidate_changing_any[g]++;
    }
    if (close_long[g]) {
      stats->pos_close_candidate_changing_long[g]++;
    }
    if (close_narrow[g]) {
      stats->pos_close_candidate_changing_narrow[g]++;
    }
  }
  // Reranking sensitivity: does any prespecified correction change the
  // argmax over the examined candidates? Ties keep the original best.
  for (int c = 0; c < PAT_OVERLAP_NUM_CORRECTIONS; c++) {
    const double correction = pat_overlap_corrections[c];
    for (int metric = 0; metric < 3; metric++) {
      const int *deltas = (metric == 0)   ? delta_any
                          : (metric == 1) ? delta_long
                                          : delta_narrow;
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
        } else if (metric == 1) {
          stats->rerank_changes_long[c]++;
        } else {
          stats->rerank_changes_narrow[c]++;
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

// The narrow overlap measure of the game's current board from the mover's
// point of view, for the move-choice harness's diagnostic reranking. Uses
// a private scratch scan (single-threaded tests only).
int pat_overlap_narrow_measure(const Game *game, int mover_index) {
  static OverlapScan *scratch = NULL;
  if (scratch == NULL) {
    scratch = malloc_or_die(sizeof(OverlapScan));
  }
  overlap_scan_game(game, mover_index, scratch);
  return scratch->narrow;
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
  overlap_print_pct("positions with a NARROW overlap (same direct run, "
                    "opposite ends, each route >= 5 tiles, lexicon-feasible)",
                    stats->pos_with_narrow, stats->positions);
  printf("  narrow measure mean per position: %.3f\n",
         (double)stats->narrow_sum / stats->positions);
  static const char *const stage_names[PAT_OVERLAP_NUM_STAGES] = {
      "bag >= 60", "bag 40-59", "bag 20-39", "bag 1-19"};
  for (int s = 0; s < PAT_OVERLAP_NUM_STAGES; s++) {
    printf("  %s: %d positions, shared-anchor any %.2f%%, long %.2f%%, "
           "narrow %.2f%%\n",
           stage_names[s], stats->positions_by_stage[s],
           stats->positions_by_stage[s] > 0
               ? 100.0 * stats->pos_with_anchor_any_by_stage[s] /
                     stats->positions_by_stage[s]
               : 0.0,
           stats->positions_by_stage[s] > 0
               ? 100.0 * stats->pos_with_anchor_long_by_stage[s] /
                     stats->positions_by_stage[s]
               : 0.0,
           stats->positions_by_stage[s] > 0
               ? 100.0 * stats->pos_with_narrow_by_stage[s] /
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
  overlap_print_pct("candidates changing the narrow measure",
                    stats->candidates_changing_narrow,
                    stats->candidates_examined);
  printf("    increasing %ld, decreasing %ld\n",
         stats->candidates_creating_narrow, stats->candidates_removing_narrow);
  overlap_print_pct("positions with any examined candidate changing (narrow)",
                    stats->pos_with_candidate_changing_narrow,
                    stats->positions);
  overlap_print_pct("positions where the best move itself changes (narrow)",
                    stats->pos_best_changes_narrow, stats->positions);
  for (int g = 0; g < 3; g++) {
    char label[128];
    snprintf(label, sizeof(label),
             "positions with a non-best candidate within %.0f eq changing "
             "(narrow)",
             pat_overlap_close_gaps[g]);
    overlap_print_pct(label, stats->pos_close_candidate_changing_narrow[g],
                      stats->positions);
  }
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
    printf("  correction %+.0f: any %d (%.2f%%), long %d (%.2f%%), narrow "
           "%d (%.2f%%)\n",
           pat_overlap_corrections[c], stats->rerank_changes_any[c],
           100.0 * stats->rerank_changes_any[c] / stats->positions,
           stats->rerank_changes_long[c],
           100.0 * stats->rerank_changes_long[c] / stats->positions,
           stats->rerank_changes_narrow[c],
           100.0 * stats->rerank_changes_narrow[c] / stats->positions);
  }

  assert(stats->positions > 0);
  free(stats);
  free(pre_scan);
  free(post_scan);
  move_list_destroy(move_list);
  config_destroy(config);
}

// Premium-combination pilot (Astra): for every route to an open word-
// multiplier square (TWS, DWS), the largest letter multiplier among the
// EMPTY squares the reaching word must cover (the required span: the
// premium, the empties between it and the route's contact, and a hook
// contact itself), separately from letter multipliers beyond the contact
// that only some replies would reach (the extension, within the remaining
// rack budget). An occupied letter multiplier is spent and is not
// counted. The per-position aggregate is sum over routes of
// word_multiplier x (max letter multiplier in the required span - 1), and
// candidates are scored by how they change it, with the same +/-
// reranking sweep the overlap pilot used.

typedef struct LMRoute {
  int premium_row;
  int premium_col;
  int word_multiplier;
  int dir;
  int side;
  int tiles_required;
  bool is_floater;
  int span_lm;        // max letter multiplier in the required span (1 = none)
  int span_lm_offset; // squares from the premium to that letter multiplier
  int ext_lm;         // max letter multiplier in the optional extension
} LMRoute;

#define PAT_LM_MAX_ROUTES 1024

typedef struct LMScan {
  LMRoute routes[PAT_LM_MAX_ROUTES];
  int num_routes;
  int aggregate;
  int aggregate_by_pair[2]
                       [2]; // [word class: 0 DWS, 1 TWS][letter: 0 DLS, 1 TLS]
  int num_span_by_pair[2][2];
  int num_ext_only;
} LMScan;

// Empty squares beyond a point on the lane, within the rack budget.
static int lm_extension_max(const Square *lane, int start, int side,
                            int budget) {
  int ext_lm = 1;
  for (int e = start;
       budget > 0 && e >= 0 && e < BOARD_DIM &&
       !square_get_is_brick(&lane[e]) &&
       square_get_letter(&lane[e]) == ALPHABET_EMPTY_SQUARE_MARKER;
       e += side, budget--) {
    const int lm =
        bonus_square_get_letter_multiplier(square_get_bonus_square(&lane[e]));
    if (lm > ext_lm) {
      ext_lm = lm;
    }
  }
  return ext_lm;
}

static bool lm_route_same_key(const LMRoute *a, const LMRoute *b) {
  return a->premium_row == b->premium_row && a->premium_col == b->premium_col &&
         a->dir == b->dir && a->side == b->side &&
         a->tiles_required == b->tiles_required &&
         a->is_floater == b->is_floater;
}

static const LMRoute *lm_find_route(const LMScan *scan, const LMRoute *key) {
  for (int i = 0; i < scan->num_routes; i++) {
    if (lm_route_same_key(&scan->routes[i], key)) {
      return &scan->routes[i];
    }
  }
  return NULL;
}

// Whether a tile-placement move puts one of its own tiles on a square.
static bool overlap_move_covers(const Move *move, int row, int col) {
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int dir = move_get_dir(move);
  const int length = move_get_tiles_length(move);
  for (int i = 0; i < length; i++) {
    const int r = dir == BOARD_HORIZONTAL_DIRECTION ? row_start : row_start + i;
    const int c = dir == BOARD_HORIZONTAL_DIRECTION ? col_start + i : col_start;
    if (r == row && c == col) {
      return move_get_tile(move, i) != PLAYED_THROUGH_MARKER;
    }
  }
  return false;
}

static void lm_scan_premium_lane(const Square *lanes,
                                 const uint8_t *unseen_counts, int premium_row,
                                 int premium_col, int word_multiplier, int dir,
                                 LMScan *scan) {
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
  for (int side = -1; side <= 1; side += 2) {
    int empties_used = 1;
    int prev_empty_idx = premium_idx;
    int span_lm = 1;
    int span_lm_offset = 0;
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
          idx += side;
        }
        const uint64_t extension_set =
            (side > 0)
                ? square_get_left_extension_set(&lane[prev_empty_idx])
                : square_get_right_extension_set(&lane[prev_empty_idx - 1]);
        if (overlap_set_flex(unseen_counts, extension_set) > 0 &&
            scan->num_routes < PAT_LM_MAX_ROUTES) {
          // Extension: empty squares beyond the run or beyond the
          // premium, within the rack.
          int ext_lm =
              lm_extension_max(lane, idx, side, RACK_SIZE - distance_bin);
          const int far_lm = lm_extension_max(lane, premium_idx - side, -side,
                                              RACK_SIZE - distance_bin);
          if (far_lm > ext_lm) {
            ext_lm = far_lm;
          }
          LMRoute *route = &scan->routes[scan->num_routes++];
          route->premium_row = premium_row;
          route->premium_col = premium_col;
          route->word_multiplier = word_multiplier;
          route->dir = dir;
          route->side = side;
          route->tiles_required = distance_bin;
          route->is_floater = true;
          route->span_lm = span_lm;
          route->span_lm_offset = span_lm_offset;
          route->ext_lm = ext_lm;
          (void)facing_idx;
        }
        // A word must play through the run; squares beyond it belong to
        // later routes on this lane, which pat_scan_unit also continues
        // to, but for this pilot the first run ends the route search.
        break;
      }
      const uint64_t cross_set = square_get_cross_set(&lane[idx]);
      if (cross_set == 0) {
        break;
      }
      empties_used++;
      if (empties_used > RACK_SIZE) {
        break;
      }
      const int lm = bonus_square_get_letter_multiplier(
          square_get_bonus_square(&lane[idx]));
      if (lm > span_lm) {
        span_lm = lm;
        span_lm_offset = empties_used - 1;
      }
      if (bonus_square_get_word_multiplier(
              square_get_bonus_square(&lane[idx])) >= 2) {
        // A second word multiplier within reach: the word x word cases
        // (triple-triple, double-double) are their own features.
        break;
      }
      if (cross_set != TRIVIAL_CROSS_SET &&
          overlap_set_flex(unseen_counts, cross_set) > 0 &&
          scan->num_routes < PAT_LM_MAX_ROUTES) {
        int ext_lm =
            lm_extension_max(lane, idx + side, side, RACK_SIZE - empties_used);
        const int far_lm = lm_extension_max(lane, premium_idx - side, -side,
                                            RACK_SIZE - empties_used);
        if (far_lm > ext_lm) {
          ext_lm = far_lm;
        }
        LMRoute *route = &scan->routes[scan->num_routes++];
        route->premium_row = premium_row;
        route->premium_col = premium_col;
        route->word_multiplier = word_multiplier;
        route->dir = dir;
        route->side = side;
        route->tiles_required = empties_used;
        route->is_floater = false;
        route->span_lm = span_lm;
        route->span_lm_offset = span_lm_offset;
        route->ext_lm = ext_lm;
      }
      prev_empty_idx = idx;
      idx += side;
    }
  }
}

static void lm_scan_game(const Game *game, int mover_index, LMScan *scan) {
  scan->num_routes = 0;
  scan->aggregate = 0;
  memset(scan->aggregate_by_pair, 0, sizeof(scan->aggregate_by_pair));
  memset(scan->num_span_by_pair, 0, sizeof(scan->num_span_by_pair));
  scan->num_ext_only = 0;
  const Board *board = game_get_board(game);
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
      const int wm =
          bonus_square_get_word_multiplier(square_get_bonus_square(square));
      if (square_get_is_brick(square) ||
          square_get_letter(square) != ALPHABET_EMPTY_SQUARE_MARKER || wm < 2) {
        continue;
      }
      lm_scan_premium_lane(lanes, unseen_counts, row, col, wm,
                           BOARD_HORIZONTAL_DIRECTION, scan);
      lm_scan_premium_lane(lanes, unseen_counts, row, col, wm,
                           BOARD_VERTICAL_DIRECTION, scan);
    }
  }
  for (int i = 0; i < scan->num_routes; i++) {
    const LMRoute *route = &scan->routes[i];
    scan->aggregate += route->word_multiplier * (route->span_lm - 1);
    if (route->span_lm > 1) {
      const int w = route->word_multiplier == 3 ? 1 : 0;
      const int l = route->span_lm == 3 ? 1 : 0;
      scan->num_span_by_pair[w][l]++;
      scan->aggregate_by_pair[w][l] +=
          route->word_multiplier * (route->span_lm - 1);
    } else if (route->ext_lm > 1) {
      scan->num_ext_only++;
    }
  }
}

// The aggregate for the harness's diagnostic reranking.
int pat_lm_aggregate(const Game *game, int mover_index) {
  static LMScan *scratch = NULL;
  if (scratch == NULL) {
    scratch = malloc_or_die(sizeof(LMScan));
  }
  lm_scan_game(game, mover_index, scratch);
  return scratch->aggregate;
}

void test_pat_premium_combo_pilot(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
      "-pat pat_dls_champion_v4");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create(PAT_OVERLAP_MOVE_LIST_CAPACITY);
  LMScan *pre = malloc_or_die(sizeof(LMScan));
  LMScan *post = malloc_or_die(sizeof(LMScan));
  int positions = 0;
  int pos_with_span = 0;
  int pos_by_pair[2][2] = {{0, 0}, {0, 0}};
  long routes_total = 0;
  long routes_with_span = 0;
  long routes_ext_only = 0;
  long offset_hist[RACK_SIZE + 1] = {0};
  long candidates = 0;
  long candidates_changing = 0;
  long candidates_up = 0;
  long candidates_down = 0;
  int pos_candidate_changing = 0;
  int pos_close_changing[3] = {0, 0, 0};
  int pos_best_changes = 0;
  static const double lm_corrections[PAT_OVERLAP_NUM_CORRECTIONS] = {
      -2.0, -1.0, -0.5, -0.25, 0.25, 0.5, 1.0, 2.0};
  int rerank[PAT_OVERLAP_NUM_CORRECTIONS] = {0};
  long delta_hist[5] = {0}; // |delta| 1-2, 3-5, 6-10, 11-20, >20 (within 2 eq)
  // What a near-best (within 2 eq) candidate does to routes with a letter
  // multiplier in their span: creates one, consumes the letter multiplier
  // or the premium with its own tiles, removes one otherwise (a block or
  // a changed contact), or leaves them alone.
  int pos_near_create = 0;
  int pos_near_consume = 0;
  int pos_near_remove = 0;
  long near_candidates = 0;
  long near_create = 0;
  long near_consume = 0;
  long near_remove = 0;
  int examples = 0;
  for (int game_index = 0; game_index < PAT_OVERLAP_NUM_GAMES; game_index++) {
    game_reset(game);
    game_seed(game, 910000000ULL + (uint64_t)game_index);
    draw_starting_racks(game);
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
      const int mover_index = game_get_player_on_turn_index(game);
      lm_scan_game(game, mover_index, pre);
      positions++;
      routes_total += pre->num_routes;
      bool any_span = false;
      for (int i = 0; i < pre->num_routes; i++) {
        if (pre->routes[i].span_lm > 1) {
          routes_with_span++;
          any_span = true;
          offset_hist[pre->routes[i].span_lm_offset]++;
        } else if (pre->routes[i].ext_lm > 1) {
          routes_ext_only++;
        }
      }
      if (any_span) {
        pos_with_span++;
      }
      for (int w = 0; w < 2; w++) {
        for (int l = 0; l < 2; l++) {
          if (pre->num_span_by_pair[w][l] > 0) {
            pos_by_pair[w][l]++;
          }
        }
      }
      const int num_moves = move_list_get_count(move_list);
      const double best_equity =
          equity_to_double(move_get_equity(move_list_get_move(move_list, 0)));
      double equities[PAT_OVERLAP_MAX_CANDIDATES];
      int deltas[PAT_OVERLAP_MAX_CANDIDATES];
      int num_candidates = 0;
      bool changing = false;
      bool close[3] = {false, false, false};
      bool near_create_here = false;
      bool near_consume_here = false;
      bool near_remove_here = false;
      game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
      for (int i = 0;
           i < num_moves && num_candidates < PAT_OVERLAP_MAX_CANDIDATES; i++) {
        const Move *move = move_list_get_move(move_list, i);
        if (move_get_type(move) == GAME_EVENT_PASS) {
          continue;
        }
        const double equity = equity_to_double(move_get_equity(move));
        const double gap = best_equity - equity;
        if (gap > PAT_OVERLAP_EQUITY_MARGIN) {
          break;
        }
        int delta = 0;
        int created = 0;
        int consumed = 0;
        int removed = 0;
        if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
          play_move_without_drawing_tiles(move, game);
          lm_scan_game(game, mover_index, post);
          game_unplay_last_move(game);
          delta = post->aggregate - pre->aggregate;
          if (gap <= 2.0) {
            for (int r = 0; r < post->num_routes; r++) {
              const LMRoute *route = &post->routes[r];
              if (route->span_lm > 1 && lm_find_route(pre, route) == NULL) {
                created++;
              }
            }
            for (int r = 0; r < pre->num_routes; r++) {
              const LMRoute *route = &pre->routes[r];
              if (route->span_lm <= 1) {
                continue;
              }
              const LMRoute *after = lm_find_route(post, route);
              if (after != NULL && after->span_lm >= route->span_lm) {
                continue;
              }
              // Does the move's own word cover the letter multiplier
              // square or the premium?
              int lm_row = route->premium_row;
              int lm_col = route->premium_col;
              if (route->dir == BOARD_HORIZONTAL_DIRECTION) {
                lm_col += route->side * route->span_lm_offset;
              } else {
                lm_row += route->side * route->span_lm_offset;
              }
              const bool covers = overlap_move_covers(move, lm_row, lm_col) ||
                                  overlap_move_covers(move, route->premium_row,
                                                      route->premium_col);
              if (covers) {
                consumed++;
              } else {
                removed++;
              }
            }
          }
        }
        if (gap <= 2.0 && i > 0) {
          near_candidates++;
          if (created > 0) {
            near_create++;
            near_create_here = true;
          }
          if (consumed > 0) {
            near_consume++;
            near_consume_here = true;
          }
          if (removed > 0) {
            near_remove++;
            near_remove_here = true;
          }
          if (delta != 0) {
            const int a = delta < 0 ? -delta : delta;
            delta_hist[a <= 2    ? 0
                       : a <= 5  ? 1
                       : a <= 10 ? 2
                       : a <= 20 ? 3
                                 : 4]++;
          }
        }
        equities[num_candidates] = equity;
        deltas[num_candidates] = delta;
        num_candidates++;
        candidates++;
        if (delta != 0) {
          candidates_changing++;
          changing = true;
          if (delta > 0) {
            candidates_up++;
          } else {
            candidates_down++;
          }
          if (i == 0) {
            pos_best_changes++;
          }
          for (int g = 0; g < 3; g++) {
            if (i > 0 && gap <= pat_overlap_close_gaps[g]) {
              close[g] = true;
            }
          }
          if (i > 0 && delta > 0 && gap <= 3.0 &&
              examples < PAT_OVERLAP_MAX_EXAMPLES) {
            examples++;
            char *cgp = game_get_cgp(game, true);
            printf("\npremium-combo example %d (bag %d): %s\n  best ", examples,
                   bag_get_letters(game_get_bag(game)), cgp);
            free(cgp);
            overlap_print_move(game, move_list_get_move(move_list, 0));
            printf(" eq %.2f\n  candidate ", best_equity);
            overlap_print_move(game, move);
            printf(" eq %.2f (gap %.2f) aggregate %d -> %d\n", equity, gap,
                   pre->aggregate, post->aggregate);
            for (int r = 0; r < post->num_routes; r++) {
              const LMRoute *route = &post->routes[r];
              if (route->span_lm <= 1) {
                continue;
              }
              printf("    %s from %s ",
                     route->is_floater ? "floater" : "hook   ",
                     route->word_multiplier == 3 ? "TWS" : "DWS");
              overlap_print_square(route->premium_row, route->premium_col);
              printf(" %s%s tiles %d: letter x%d at offset %d%s\n",
                     route->dir == BOARD_HORIZONTAL_DIRECTION ? "row" : "col",
                     route->side < 0 ? "-" : "+", route->tiles_required,
                     route->span_lm, route->span_lm_offset,
                     route->ext_lm > 1 ? " (+ext)" : "");
            }
          }
        }
      }
      game_set_backup_mode(game, BACKUP_MODE_OFF);
      if (changing) {
        pos_candidate_changing++;
      }
      pos_near_create += near_create_here;
      pos_near_consume += near_consume_here;
      pos_near_remove += near_remove_here;
      for (int g = 0; g < 3; g++) {
        if (close[g]) {
          pos_close_changing[g]++;
        }
      }
      for (int c = 0; c < PAT_OVERLAP_NUM_CORRECTIONS; c++) {
        const double correction = lm_corrections[c];
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
          rerank[c]++;
        }
      }
      Move best;
      move_copy(&best, move_list_get_move(move_list, 0));
      play_move(&best, game, NULL);
    }
  }
  printf(
      "\npremium-combination pilot (v4 self-play, %d games, %d positions):\n",
      PAT_OVERLAP_NUM_GAMES, positions);
  printf("  routes per position %.2f; with an empty letter multiplier in the "
         "required span %.1f%%; letter multiplier only in the extension "
         "%.1f%%\n",
         (double)routes_total / positions,
         100.0 * routes_with_span / routes_total,
         100.0 * routes_ext_only / routes_total);
  overlap_print_pct("positions with any such route", pos_with_span, positions);
  printf("  by pair: TWS+DLS %.1f%%, TWS+TLS %.1f%%, DWS+DLS %.1f%%, DWS+TLS "
         "%.1f%% of positions\n",
         100.0 * pos_by_pair[1][0] / positions,
         100.0 * pos_by_pair[1][1] / positions,
         100.0 * pos_by_pair[0][0] / positions,
         100.0 * pos_by_pair[0][1] / positions);
  printf("  letter multiplier's offset from the premium (routes):");
  for (int o = 0; o <= RACK_SIZE; o++) {
    printf(" %d:%ld", o, offset_hist[o]);
  }
  printf("\ncandidates within %.0f equity of best:\n",
         PAT_OVERLAP_EQUITY_MARGIN);
  overlap_print_pct("candidates changing the aggregate", candidates_changing,
                    candidates);
  printf("    increasing %ld, decreasing %ld\n", candidates_up,
         candidates_down);
  overlap_print_pct("positions with any examined candidate changing",
                    pos_candidate_changing, positions);
  overlap_print_pct("positions where the best move itself changes",
                    pos_best_changes, positions);
  for (int g = 0; g < 3; g++) {
    char label[96];
    snprintf(label, sizeof(label),
             "positions with a non-best candidate within %.0f eq changing",
             pat_overlap_close_gaps[g]);
    overlap_print_pct(label, pos_close_changing[g], positions);
  }
  printf("near-best (within 2 eq, non-best) candidates: %ld\n",
         near_candidates);
  printf("  |delta| 1-2: %ld, 3-5: %ld, 6-10: %ld, 11-20: %ld, >20: %ld\n",
         delta_hist[0], delta_hist[1], delta_hist[2], delta_hist[3],
         delta_hist[4]);
  printf("  creating a route with a letter multiplier in its span: %ld "
         "(%.1f%%); positions %d (%.1f%%)\n",
         near_create, 100.0 * near_create / near_candidates, pos_near_create,
         100.0 * pos_near_create / positions);
  printf("  consuming the letter multiplier or the premium: %ld (%.1f%%); "
         "positions %d (%.1f%%)\n",
         near_consume, 100.0 * near_consume / near_candidates, pos_near_consume,
         100.0 * pos_near_consume / positions);
  printf("  removing such a route otherwise (block, changed contact): %ld "
         "(%.1f%%); positions %d (%.1f%%)\n",
         near_remove, 100.0 * near_remove / near_candidates, pos_near_remove,
         100.0 * pos_near_remove / positions);
  printf("reranking sensitivity (equity per unit of aggregate = per point of "
         "word multiplier x (letter multiplier - 1); positive = penalize "
         "creating):\n");
  for (int c = 0; c < PAT_OVERLAP_NUM_CORRECTIONS; c++) {
    printf("  correction %+.2f: %d (%.2f%%)\n", lm_corrections[c], rerank[c],
           100.0 * rerank[c] / positions);
  }
  free(pre);
  free(post);
  move_list_destroy(move_list);
  config_destroy(config);
}
