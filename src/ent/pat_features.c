#include "pat_features.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/equity_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/pat_defs.h"
#include "../def/rack_defs.h"
#include "board.h"
#include "bonus_square.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "pat.h"
#include "pat_lexicon.h"
#include "rack.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// The flexibility of a hook or extension point: how many tiles the opponent
// could still hold that fit it. Counting unseen tiles rather than admitted
// letters makes a hook needing a J the near-nothing it usually is, and one
// only the evaluating player can fill no threat at all. Bit 0 of a cross or
// extension set is the blank marker, which is skipped.
static inline int pat_set_flex(const uint8_t *unseen_counts,
                               uint64_t letter_set) {
  int flex = 0;
  uint64_t remaining = letter_set & ~(uint64_t)1;
  while (remaining) {
    const int machine_letter = pat_ctz(remaining);
    remaining &= remaining - 1;
    flex += unseen_counts[machine_letter];
  }
  return flex;
}

// Fills unseen_counts (MAX_ALPHABET_SIZE entries) with the tiles that are
// neither on the board nor on the evaluating player's rack, which is
// exactly the pool the opponent draws from plus what they already hold.
// Blanks on the board are counted against the blank.
void pat_compute_unseen_counts(const Square *lanes,
                               const LetterDistribution *ld,
                               const Rack *player_rack,
                               uint8_t *unseen_counts) {
  memset(unseen_counts, 0, sizeof(uint8_t) * MAX_ALPHABET_SIZE);
  const int ld_size = ld_get_size(ld);
  for (int machine_letter = 0; machine_letter < ld_size; machine_letter++) {
    unseen_counts[machine_letter] = (uint8_t)ld_get_dist(ld, machine_letter);
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
  if (player_rack == NULL) {
    return;
  }
  for (int machine_letter = 0; machine_letter < ld_size; machine_letter++) {
    const int held = rack_get_letter(player_rack, machine_letter);
    unseen_counts[machine_letter] =
        (uint8_t)((unseen_counts[machine_letter] > held)
                      ? unseen_counts[machine_letter] - held
                      : 0);
  }
}

static inline bool pat_move_covers(const PATMoveOverlay *overlay, int row,
                                   int col, MachineLetter *fresh_letter_out) {
  if (!overlay) {
    return false;
  }
  if (row < overlay->row_start || row > overlay->row_end ||
      col < overlay->col_start || col > overlay->col_end) {
    return false;
  }
  const int tile_index =
      overlay->vertical ? row - overlay->row_start : col - overlay->col_start;
  const MachineLetter tile = move_get_tile(overlay->move, tile_index);
  if (tile == PLAYED_THROUGH_MARKER) {
    return false;
  }
  *fresh_letter_out = tile;
  return true;
}

// The cross set of the empty square (row, col) on a dir lane after the
// move, and the cross score to go with it: the perpendicular pattern
// through the square, existing and fresh tiles alike, resolved on the
// GADDAG the way game_gen_cross_set does for a played board. Tiles before
// the square are collected outward, which is the reversed prefix the
// GADDAG wants; a prefix-only pattern takes the separator and reads the
// accepted set (back hooks); a suffix-only pattern walks the reversed
// suffix and reads the accepted set (front hooks); with both, each letter
// arc after the separator is checked through the suffix. 0 means no
// letter fits (a dead square).
static uint64_t pat_fresh_cross_set(const KWG *kwg,
                                    const LetterDistribution *ld,
                                    const Square *lanes, int dir, int row,
                                    int col, const PATMoveOverlay *overlay,
                                    int *cross_score_out) {
  const int perp_dir = (dir == BOARD_HORIZONTAL_DIRECTION)
                           ? BOARD_VERTICAL_DIRECTION
                           : BOARD_HORIZONTAL_DIRECTION;
  const int perp_lane_index =
      (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? row : col;
  const int perp_idx = (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? col : row;
  const Square *perp_lane =
      board_get_row_cache(lanes, perp_lane_index, perp_dir);
  MachineLetter before[BOARD_DIM];
  MachineLetter after[BOARD_DIM];
  int num_before = 0;
  int num_after = 0;
  int cross_score = 0;
  for (int side = -1; side <= 1; side += 2) {
    int idx = perp_idx + side;
    while (idx >= 0 && idx < BOARD_DIM &&
           !square_get_is_brick(&perp_lane[idx])) {
      const int r =
          (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? perp_lane_index : idx;
      const int c =
          (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? idx : perp_lane_index;
      MachineLetter letter;
      if (!pat_move_covers(overlay, r, c, &letter)) {
        letter = square_get_letter(&perp_lane[idx]);
        if (letter == ALPHABET_EMPTY_SQUARE_MARKER) {
          break;
        }
      }
      if (!get_is_blanked(letter)) {
        cross_score += equity_to_int(ld_get_score(ld, letter));
      }
      const MachineLetter unblanked = get_unblanked_machine_letter(letter);
      if (side < 0) {
        before[num_before++] = unblanked;
      } else {
        after[num_after++] = unblanked;
      }
      idx += side;
    }
  }
  *cross_score_out = cross_score;
  if (num_before == 0 && num_after == 0) {
    return TRIVIAL_CROSS_SET;
  }
  const uint32_t root = kwg_get_root_node_index(kwg);
  uint64_t extension_set = 0;
  if (num_before == 0) {
    uint32_t node = root;
    for (int after_idx = num_after - 1; after_idx >= 0; after_idx--) {
      node = kwg_get_next_node_index(kwg, node, after[after_idx]);
      if (node == 0) {
        return 0;
      }
    }
    return kwg_get_letter_sets(kwg, node, &extension_set) & ~(uint64_t)1;
  }
  uint32_t node = root;
  for (int before_idx = 0; before_idx < num_before; before_idx++) {
    node = kwg_get_next_node_index(kwg, node, before[before_idx]);
    if (node == 0) {
      return 0;
    }
  }
  const uint32_t separated =
      kwg_get_next_node_index(kwg, node, SEPARATION_MACHINE_LETTER);
  if (separated == 0) {
    return 0;
  }
  if (num_after == 0) {
    return kwg_get_letter_sets(kwg, separated, &extension_set) & ~(uint64_t)1;
  }
  uint64_t cross_set = 0;
  for (uint32_t node_idx = separated;; node_idx++) {
    const uint32_t arc = kwg_node(kwg, node_idx);
    const MachineLetter ml = (MachineLetter)kwg_node_tile(arc);
    if (ml != SEPARATION_MACHINE_LETTER) {
      uint32_t cur = kwg_node_arc_index_prefetch(arc, kwg);
      bool ok = cur != 0;
      for (int after_idx = 0; ok && after_idx < num_after - 1; after_idx++) {
        cur = kwg_get_next_node_index(kwg, cur, after[after_idx]);
        ok = cur != 0;
      }
      if (ok && kwg_in_letter_set(kwg, after[num_after - 1], cur)) {
        cross_set |= (uint64_t)1 << ml;
      }
    }
    if (kwg_node_is_end(arc)) {
      break;
    }
  }
  return cross_set;
}

// Returns true and sets *fresh_letter_out if the move places a fresh tile
// on (row, col). Played-through positions fall through to the board.
static inline int pat_unit_row(int dir, int lane_index, int idx) {
  return (dir == BOARD_HORIZONTAL_DIRECTION) ? lane_index : idx;
}

static inline int pat_unit_col(int dir, int lane_index, int idx) {
  return (dir == BOARD_HORIZONTAL_DIRECTION) ? idx : lane_index;
}

static inline MachineLetter pat_effective_letter(const Square *lane, int idx,
                                                 const PATMoveOverlay *overlay,
                                                 int row, int col) {
  MachineLetter fresh_letter;
  if (pat_move_covers(overlay, row, col, &fresh_letter)) {
    return fresh_letter;
  }
  return square_get_letter(&lane[idx]);
}

typedef struct PATCrossInfo {
  bool dead;
  bool hooky;
  int flex;
  // Sum over admissible letters of unseen count times the letter's
  // incremental immediate score at this square (see
  // pat_effective_cross_info), divided by PAT_HOOK_SCORE_SCALE.
  int score_exposure;
  // The real cross-set this hook or floater route needs, valid only when
  // hooky; 0 otherwise. Blank stays at bit 0, same convention as every
  // other cross/extension set (see pat_set_flex).
  uint64_t letter_set;
} PATCrossInfo;

// The hook square's own multipliers and the premium's word multiplier are
// what a letter placed there actually earns: the hooked word's existing
// score times the square's word multiplier, plus the letter's value under
// the square's letter multiplier counted in the hook word (times the
// square's word multiplier) and again in the lane word the premium
// multiplies. When the square IS the premium the two word multipliers are
// the same one, which the formula already handles.
static inline int pat_letter_score_exposure(int cross_score, int tile_score,
                                            int letter_multiplier,
                                            int word_multiplier,
                                            int premium_word_multiplier) {
  return word_multiplier * cross_score +
         letter_multiplier * tile_score *
             (word_multiplier + premium_word_multiplier);
}

// The perpendicular constraint at an empty square: dead (no letter can be
// placed), hooky (constrained by an adjacent perpendicular word, i.e. a
// real hook), or unconstrained. When the move places a tile perpendicular-
// adjacent to the square, the real post-move cross set would require a KWG
// traversal, so it is approximated with the per-letter hook_flex table; a
// pre-move dead square is left dead even though a fresh adjacent tile
// technically changes its perpendicular pattern. ld and
// premium_word_multiplier feed
// score_exposure only; a caller that never reads it may pass NULL and any
// multiplier.
static PATCrossInfo pat_effective_cross_info(
    const Square *lane, int idx, int dir, const PATMoveOverlay *overlay,
    int row, int col, const uint8_t *unseen_counts,
    const LetterDistribution *ld, int premium_word_multiplier) {
  const uint64_t base_cross_set = square_get_cross_set(&lane[idx]);
  PATCrossInfo info;
  // Exact created hooks: if a fresh tile sits perpendicular-adjacent,
  // resolve the whole perpendicular pattern on the GADDAG and score it
  // like a real hook; an empty set makes the square dead.
  if (overlay != NULL && overlay->kwg != NULL && ld != NULL) {
    bool fresh_adjacent = false;
    for (int side = -1; side <= 1; side += 2) {
      const int perp_row =
          (dir == BOARD_HORIZONTAL_DIRECTION) ? row + side : row;
      const int perp_col =
          (dir == BOARD_HORIZONTAL_DIRECTION) ? col : col + side;
      MachineLetter fresh_letter;
      if (perp_row >= 0 && perp_row < BOARD_DIM && perp_col >= 0 &&
          perp_col < BOARD_DIM &&
          pat_move_covers(overlay, perp_row, perp_col, &fresh_letter)) {
        fresh_adjacent = true;
      }
    }
    if (fresh_adjacent) {
      int cross_score = 0;
      const uint64_t cross_set =
          pat_fresh_cross_set(overlay->kwg, ld, overlay->lanes, dir, row, col,
                              overlay, &cross_score);
      info.dead = (cross_set == 0);
      info.hooky = !info.dead && (cross_set != TRIVIAL_CROSS_SET);
      info.flex = info.hooky ? pat_set_flex(unseen_counts, cross_set) : 0;
      info.letter_set = info.hooky ? cross_set : 0;
      info.score_exposure = 0;
      if (info.hooky) {
        const BonusSquare bonus = square_get_bonus_square(&lane[idx]);
        const int letter_multiplier = bonus_square_get_letter_multiplier(bonus);
        const int word_multiplier = bonus_square_get_word_multiplier(bonus);
        int64_t exposure = 0;
        uint64_t remaining = cross_set & ~(uint64_t)1;
        while (remaining) {
          const int machine_letter = pat_ctz(remaining);
          remaining &= remaining - 1;
          if (unseen_counts[machine_letter] == 0) {
            continue;
          }
          exposure +=
              (int64_t)unseen_counts[machine_letter] *
              pat_letter_score_exposure(
                  cross_score, equity_to_int(ld_get_score(ld, machine_letter)),
                  letter_multiplier, word_multiplier, premium_word_multiplier);
        }
        info.score_exposure = (int)(exposure / PAT_HOOK_SCORE_SCALE);
      }
      return info;
    }
  }
  info.dead = (base_cross_set == 0);

  info.hooky = !info.dead && (base_cross_set != TRIVIAL_CROSS_SET);
  info.flex = info.hooky ? pat_set_flex(unseen_counts, base_cross_set) : 0;
  info.letter_set = info.hooky ? base_cross_set : 0;
  info.score_exposure = 0;
  const BonusSquare bonus = square_get_bonus_square(&lane[idx]);
  const int letter_multiplier = bonus_square_get_letter_multiplier(bonus);
  const int word_multiplier = bonus_square_get_word_multiplier(bonus);
  if (info.hooky && ld != NULL) {
    const int cross_score = equity_to_int(square_get_cross_score(&lane[idx]));
    int64_t exposure = 0;
    uint64_t remaining = base_cross_set & ~(uint64_t)1;
    while (remaining) {
      const int machine_letter = pat_ctz(remaining);
      remaining &= remaining - 1;
      if (unseen_counts[machine_letter] == 0) {
        continue;
      }
      exposure +=
          (int64_t)unseen_counts[machine_letter] *
          pat_letter_score_exposure(
              cross_score, equity_to_int(ld_get_score(ld, machine_letter)),
              letter_multiplier, word_multiplier, premium_word_multiplier);
    }
    info.score_exposure = (int)(exposure / PAT_HOOK_SCORE_SCALE);
  }
  if (!overlay || info.dead) {
    return info;
  }
  int fresh_flex = -1;
  int fresh_score = 0;
  for (int side = -1; side <= 1; side += 2) {
    int perp_row = row;
    int perp_col = col;
    if (dir == BOARD_HORIZONTAL_DIRECTION) {
      perp_row += side;
    } else {
      perp_col += side;
    }
    if (perp_row < 0 || perp_row >= BOARD_DIM || perp_col < 0 ||
        perp_col >= BOARD_DIM) {
      continue;
    }
    MachineLetter fresh_letter;
    if (pat_move_covers(overlay, perp_row, perp_col, &fresh_letter)) {
      const int flex =
          overlay->hook_flex[get_unblanked_machine_letter(fresh_letter)];
      if (fresh_flex < 0 || flex < fresh_flex) {
        fresh_flex = flex;
        fresh_score = (ld != NULL && !get_is_blanked(fresh_letter))
                          ? equity_to_int(ld_get_score(ld, fresh_letter))
                          : 0;
      }
    }
  }
  if (fresh_flex >= 0) {
    info.flex = (info.hooky && info.flex < fresh_flex) ? info.flex : fresh_flex;
    // The hook the move itself creates has no real cross set or cross
    // score yet: the hooked word is approximated by the fresh tile facing
    // this square and the admissible letters by a typical two-point tile,
    // weighted by the same two-letter-word count the flexibility uses.
    if (ld != NULL) {
      const int fresh_exposure =
          fresh_flex *
          pat_letter_score_exposure(fresh_score, 2, letter_multiplier,
                                    word_multiplier, premium_word_multiplier) /
          PAT_HOOK_SCORE_SCALE;
      if (!info.hooky || fresh_exposure < info.score_exposure) {
        info.score_exposure = fresh_exposure;
      }
    }
    info.hooky = true;
  }
  return info;
}

// Scans one (premium square, dir) unit: walks outward from the empty premium
// square along its lane on both sides, accumulating hook, floater, and
// triple-triple features binned by the number of tiles a word reaching the
// square must play. A triple-triple span is counted from both of its
// endpoints; training and evaluation count it the same way, so the weight
// absorbs the double count.
// extent_lo/extent_hi (optional) receive the lowest and highest lane index
// the walk visited, including the square it broke on: a move can only change
// this unit's features by placing a tile on one of those squares or directly
// beside them.
// score_channels says whether to compute the hook-score channels;
// evaluation skips them when their weights are all zero (see
// PATEvalContext.score_channels).
void pat_scan_unit(const Square *lanes, const LetterDistribution *ld,
                   const uint8_t *unseen_counts, const PATWeights *pat,
                   int premium_row, int premium_col, int premium_class, int dir,
                   const PATMoveOverlay *overlay, int32_t *features,
                   int *extent_lo, int *extent_hi, int opponent_rack_size,
                   uint64_t *hook_letters_out, bool score_channels) {
  const int max_reach =
      (opponent_rack_size < RACK_SIZE) ? opponent_rack_size : RACK_SIZE;
  // Feature extraction needs every score channel even from a zero bootstrap.
  // Runtime scans with zero hook-score weights can skip the exposure work;
  // exact created hooks still need ld for their cross-set calculation.
  const LetterDistribution *hook_score_ld =
      score_channels || (overlay != NULL && overlay->kwg != NULL) ? ld : NULL;
  // Each premium class writes its own hook and floater-value channels. The
  // richer channels (floater flexibility, the lexicon through-table, and
  // the triple-triple pair) stay exclusive to triple word squares, which
  // are the ones worth the feature budget.
  int hook_base = PAT_FEATURE_HOOK_START;
  int float_score_base = PAT_FEATURE_FLOAT_SCORE_START;
  if (premium_class == PAT_PREMIUM_DWS) {
    hook_base = PAT_FEATURE_DWS_HOOK_START;
    float_score_base = PAT_FEATURE_DWS_FLOAT_SCORE_START;
  } else if (premium_class == PAT_PREMIUM_TLS) {
    hook_base = PAT_FEATURE_TLS_HOOK_START;
    float_score_base = PAT_FEATURE_TLS_FLOAT_SCORE_START;
  } else if (premium_class == PAT_PREMIUM_DLS) {
    hook_base = PAT_FEATURE_DLS_HOOK_START;
    float_score_base = PAT_FEATURE_DLS_FLOAT_SCORE_START;
  } else if (premium_class == PAT_PREMIUM_QWS) {
    hook_base = PAT_FEATURE_QWS_HOOK_START;
    float_score_base = PAT_FEATURE_QWS_FLOAT_SCORE_START;
  } else if (premium_class == PAT_PREMIUM_QLS) {
    hook_base = PAT_FEATURE_QLS_HOOK_START;
    float_score_base = PAT_FEATURE_QLS_FLOAT_SCORE_START;
  }
  const bool full_channels = (premium_class == PAT_PREMIUM_TWS);
  const int lane_index =
      (dir == BOARD_HORIZONTAL_DIRECTION) ? premium_row : premium_col;
  const int premium_idx =
      (dir == BOARD_HORIZONTAL_DIRECTION) ? premium_col : premium_row;
  const Square *lane = board_get_row_cache(lanes, lane_index, dir);
  if (extent_lo) {
    *extent_lo = premium_idx;
  }
  if (extent_hi) {
    *extent_hi = premium_idx;
  }

  if (pat_effective_letter(lane, premium_idx, overlay,
                           pat_unit_row(dir, lane_index, premium_idx),
                           pat_unit_col(dir, lane_index, premium_idx)) !=
      ALPHABET_EMPTY_SQUARE_MARKER) {
    // The TWS square is covered (by the board or by the move itself):
    // nothing along this lane can reach it. Covering a TWS is exactly the
    // blocking reward.
    return;
  }
  const int premium_word_multiplier = bonus_square_get_word_multiplier(
      square_get_bonus_square(&lane[premium_idx]));
  const PATCrossInfo premium_info = pat_effective_cross_info(
      lane, premium_idx, dir, overlay,
      pat_unit_row(dir, lane_index, premium_idx),
      pat_unit_col(dir, lane_index, premium_idx), unseen_counts, hook_score_ld,
      premium_word_multiplier);
  if (premium_info.dead) {
    // No word along this lane can cover the TWS square at all.
    return;
  }
  if (premium_info.hooky) {
    // A one-tile play on the TWS square itself completes a perpendicular
    // word at triple word score: hook access at d = 1.
    features[hook_base] += premium_info.flex;
    if (full_channels) {
      features[PAT_FEATURE_HOOK_SCORE_START] += premium_info.score_exposure;
    }
    if (hook_letters_out) {
      *hook_letters_out |= premium_info.letter_set;
    }
  }

  for (int side = -1; side <= 1; side += 2) {
    // Number of empty squares a word covering the span from the current
    // scan position through the TWS square must fill, i.e. the number of
    // tiles the opponent must play. The TWS square itself is the first.
    int empties_used = 1;
    bool span_has_floater = false;
    int span_floater_flex = 0;
    bool span_has_hook = premium_info.hooky;
    int prev_empty_idx = premium_idx;
    int last_visited_idx = premium_idx;
    int idx = premium_idx + side;
    while (idx >= 0 && idx < BOARD_DIM) {
      last_visited_idx = idx;
      if (square_get_is_brick(&lane[idx])) {
        break;
      }
      const int square_row = pat_unit_row(dir, lane_index, idx);
      const int square_col = pat_unit_col(dir, lane_index, idx);
      const MachineLetter letter =
          pat_effective_letter(lane, idx, overlay, square_row, square_col);
      if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
        // A run of tiles: floater (playthrough) access. The run costs the
        // opponent no tiles, so the whole run shares the bin of the empty
        // span between it and the TWS square.
        const int distance_bin = empties_used;
        const MachineLetter facing_letter = letter;
        bool run_has_fresh_tile = false;
        // The run's letters in encounter order (facing tile first), for
        // the run-keyed through lookup.
        MachineLetter run_letters[BOARD_DIM];
        int run_length = 0;
        const bool run_through =
            pat != NULL && pat->run_through && full_channels;
        while (idx >= 0 && idx < BOARD_DIM &&
               !square_get_is_brick(&lane[idx])) {
          const int run_row = pat_unit_row(dir, lane_index, idx);
          const int run_col = pat_unit_col(dir, lane_index, idx);
          const MachineLetter run_letter =
              pat_effective_letter(lane, idx, overlay, run_row, run_col);
          if (run_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
            break;
          }
          last_visited_idx = idx;
          MachineLetter fresh_letter;
          if (pat_move_covers(overlay, run_row, run_col, &fresh_letter)) {
            run_has_fresh_tile = true;
          }
          int tile_score = 0;
          if (!get_is_blanked(run_letter)) {
            tile_score = equity_to_int(ld_get_score(ld, run_letter));
          }
          features[float_score_base + distance_bin - 1] += tile_score;
          // What a word through this floater would actually lay down on
          // the way to the triple. The span it must cover is the empties
          // between the two plus both endpoints; a blank contributes
          // nothing to score above but reaches whatever its letter reaches.
          if (run_through && run_length < BOARD_DIM) {
            run_letters[run_length++] =
                get_unblanked_machine_letter(run_letter);
          }
          if (pat != NULL && full_channels && !run_through) {
            const MachineLetter unblanked =
                get_unblanked_machine_letter(run_letter);
            const int span = distance_bin + 1;
            if (span < PAT_MAX_THROUGH_LEN) {
              // A run beyond the premium (side > 0) ends the word that
              // reaches it from the premium; a run before it begins that
              // word. See PATWeights.signed_through.
              if (pat->signed_through) {
                const int word_end = (side > 0) ? 1 : 0;
                features[PAT_FEATURE_FLOAT_THROUGH_SCORE_START + distance_bin -
                         1] +=
                    pat->through_score_end[word_end][unblanked][span];
                features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + distance_bin -
                         1] +=
                    pat->through_count_end[word_end][unblanked][span];
              } else {
                features[PAT_FEATURE_FLOAT_THROUGH_SCORE_START + distance_bin -
                         1] += pat->through_score[unblanked][span];
                features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + distance_bin -
                         1] += pat->through_count[unblanked][span];
              }
            }
          }
          idx += side;
        }
        if (run_through && run_length > 0) {
          // The whole run at the end of a word of exactly the covering
          // length: a suffix when the run lies beyond the premium (the
          // word runs premium to run, encounter order is word order), a
          // prefix when before it (word order is the reverse). Keyed by
          // the run's far-end letters, up to PAT_RUN_THROUGH_MAX_KEY.
          const int word_length = distance_bin + run_length;
          const int key_len = (run_length < PAT_RUN_THROUGH_MAX_KEY)
                                  ? run_length
                                  : PAT_RUN_THROUGH_MAX_KEY;
          MachineLetter key[PAT_RUN_THROUGH_MAX_KEY];
          for (int key_idx = 0; key_idx < key_len; key_idx++) {
            // Far-end letters in word order: beyond the premium the far end
            // is the last encountered and word order is encounter order;
            // before it, word order is reversed, so the word's first
            // letters are the last encountered, read backwards.
            key[key_idx] = (side > 0)
                               ? run_letters[run_length - key_len + key_idx]
                               : run_letters[run_length - 1 - key_idx];
          }
          const int word_end = (side > 0) ? 1 : 0;
          features[PAT_FEATURE_FLOAT_THROUGH_SCORE_START + distance_bin - 1] +=
              pat_get_run_through_score(pat, word_end, key, key_len,
                                        word_length);
          features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + distance_bin - 1] +=
              pat_get_run_through_count(pat, word_end, key, key_len,
                                        word_length);
        }
        int run_flex;
        // A fresh tile implies an overlay; the check lets the analyzer
        // see it.
        if (run_has_fresh_tile && overlay != NULL) {
          // The run's real extension sets do not exist yet; approximate
          // with the two-letter-word flexibility of the tile facing the
          // TWS square.
          run_flex =
              overlay->hook_flex[get_unblanked_machine_letter(facing_letter)];
        } else {
          // The letters that could extend this run toward the TWS square.
          // game_gen_cross_set stores a run's own extension sets on its
          // rightmost (highest lane index) tile, and the leftward one also
          // on the empty square just before the run; an empty square's
          // right_extension_set is never written and stays trivial. The
          // legacy reads below therefore see the trivial set (or the wrong
          // run's) and count every unseen tile; see
          // PATWeights.lexicon_floaters for why both are kept.
          uint64_t extension_set;
          if (pat != NULL && pat->lexicon_floaters) {
            extension_set =
                (side > 0)
                    ? square_get_left_extension_set(&lane[prev_empty_idx])
                    : square_get_right_extension_set(&lane[prev_empty_idx - 1]);
          } else {
            extension_set =
                (side > 0)
                    ? square_get_right_extension_set(&lane[prev_empty_idx])
                    : square_get_left_extension_set(&lane[prev_empty_idx]);
          }
          run_flex = pat_set_flex(unseen_counts, extension_set);
          if (hook_letters_out) {
            *hook_letters_out |= extension_set;
          }
        }
        if (full_channels) {
          features[PAT_FEATURE_FLOAT_FLEX_START + distance_bin - 1] += run_flex;
        }
        span_has_floater = true;
        span_floater_flex += run_flex;
        continue;
      }
      const PATCrossInfo info = pat_effective_cross_info(
          lane, idx, dir, overlay, square_row, square_col, unseen_counts,
          hook_score_ld, premium_word_multiplier);
      if (info.dead) {
        break;
      }
      empties_used++;
      if (empties_used > max_reach) {
        break;
      }
      if (full_channels && bonus_square_get_word_multiplier(
                               square_get_bonus_square(&lane[idx])) == 3) {
        // A second empty TWS within reach: the triple-triple span from the
        // scanned TWS square through this one.
        if (span_has_floater) {
          features[PAT_FEATURE_TT_FLOATER] += 1 + span_floater_flex;
        } else if (span_has_hook) {
          features[PAT_FEATURE_TT_HOOK_ONLY] += 1;
        }
        break;
      }
      if (info.hooky) {
        features[hook_base + empties_used - 1] += info.flex;
        if (full_channels) {
          features[PAT_FEATURE_HOOK_SCORE_START + empties_used - 1] +=
              info.score_exposure;
        }
        if (hook_letters_out) {
          *hook_letters_out |= info.letter_set;
        }
        span_has_hook = true;
      }
      prev_empty_idx = idx;
      idx += side;
    }
    if (side < 0 && extent_lo) {
      *extent_lo = last_visited_idx;
    } else if (side > 0 && extent_hi) {
      *extent_hi = last_visited_idx;
    }
  }
}

// Scans one double-double window: the lane squares from lo to hi, whose
// endpoints are both double word squares. A word covering the window doubles
// twice, so it is worth about as much as a triple-triple and is defended the
// same way. The window is dead when either endpoint is covered (covering one
// is exactly the blocking reward), when a square inside it is bricked or has
// an empty cross set, or when it still needs more fresh tiles than a rack
// holds. A live window also needs somewhere to attach: a playthrough tile
// inside it, or a hookable empty square.
void pat_scan_dd_unit(const Square *lanes, const uint8_t *unseen_counts,
                      int dir, int lane_index, int lo, int hi, int tier,
                      const PATMoveOverlay *overlay, int32_t *features,
                      int *extent_lo, int *extent_hi, int opponent_rack_size,
                      uint64_t *hook_letters_out) {
  const int max_reach =
      (opponent_rack_size < RACK_SIZE) ? opponent_rack_size : RACK_SIZE;
  const int tier_base =
      PAT_FEATURE_WINDOW_START + tier * PAT_WINDOW_FEATURES_PER_TIER;
  const Square *lane = board_get_row_cache(lanes, lane_index, dir);
  if (extent_lo != NULL) {
    *extent_lo = lo;
  }
  if (extent_hi != NULL) {
    *extent_hi = hi;
  }
  int empties = 0;
  bool has_floater = false;
  bool has_hook = false;
  for (int idx = lo; idx <= hi; idx++) {
    if (square_get_is_brick(&lane[idx])) {
      return;
    }
    const int square_row = pat_unit_row(dir, lane_index, idx);
    const int square_col = pat_unit_col(dir, lane_index, idx);
    const MachineLetter letter =
        pat_effective_letter(lane, idx, overlay, square_row, square_col);
    if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
      if (idx == lo || idx == hi) {
        return;
      }
      has_floater = true;
      continue;
    }
    // Never reads score_exposure, so ld and the premium multiplier are
    // don't-cares here.
    const PATCrossInfo info =
        pat_effective_cross_info(lane, idx, dir, overlay, square_row,
                                 square_col, unseen_counts, NULL, 2);
    if (info.dead) {
      return;
    }
    if (info.hooky) {
      has_hook = true;
      if (hook_letters_out) {
        *hook_letters_out |= info.letter_set;
      }
    }
    empties++;
  }
  if (empties > max_reach) {
    return;
  }
  if (has_floater) {
    features[tier_base] += 1;
  } else if (has_hook) {
    features[tier_base + 1] += 1;
  } else {
    return;
  }
  features[tier_base + 2] += max_reach - empties;
}

// Finds the double-double windows: consecutive pairs of double word squares
// in one lane, near enough that a single word could cover both. Horizontal
// lanes come first, then vertical, each scanned in increasing order, so the
// truncation at PAT_MAX_DD is deterministic and training and evaluation
// always agree. Whether a window is currently live is left to the scan.
// Which window tier the product of two word multipliers belongs to.
static int pat_window_tier(int product) {
  if (product <= 4) {
    return 0; // double-double
  }
  if (product <= 8) {
    return 1; // double-triple, double-quad
  }
  if (product == 9) {
    return 2; // triple-triple
  }
  return 3; // triple-quad, quad-quad
}

// Finds the windows: consecutive pairs of word-multiplier squares in one
// lane, near enough that a single word could cover both. Horizontal lanes
// come first, then vertical, each scanned in increasing order, so the
// truncation at PAT_MAX_DD is deterministic and training and evaluation
// always agree. Whether a window is currently live is left to the scan.
int pat_find_dd(const Square *lanes, uint8_t *dd_dirs, uint8_t *dd_lanes,
                uint8_t *dd_los, uint8_t *dd_his, uint8_t *dd_tiers) {
  int num_dd = 0;
  for (int dir = 0; dir < 2; dir++) {
    for (int lane_index = 0; lane_index < BOARD_DIM; lane_index++) {
      const Square *lane = board_get_row_cache(lanes, lane_index, dir);
      int previous_idx = -1;
      int previous_multiplier = 0;
      for (int idx = 0; idx < BOARD_DIM; idx++) {
        const int word_multiplier = bonus_square_get_word_multiplier(
            square_get_bonus_square(&lane[idx]));
        if (word_multiplier < 2) {
          continue;
        }
        if (previous_idx >= 0 && idx - previous_idx <= PAT_DD_MAX_SPAN) {
          if (num_dd == PAT_MAX_DD) {
            return num_dd;
          }
          dd_dirs[num_dd] = (uint8_t)dir;
          dd_lanes[num_dd] = (uint8_t)lane_index;
          dd_los[num_dd] = (uint8_t)previous_idx;
          dd_his[num_dd] = (uint8_t)idx;
          dd_tiers[num_dd] =
              (uint8_t)pat_window_tier(previous_multiplier * word_multiplier);
          num_dd++;
        }
        previous_idx = idx;
        previous_multiplier = word_multiplier;
      }
    }
  }
  return num_dd;
}

// Which premium class a square belongs to, or -1 if it is not one worth
// walking a lane for.
static int pat_premium_class_of(const Square *square) {
  const BonusSquare bonus = square_get_bonus_square(square);
  const int word_multiplier = bonus_square_get_word_multiplier(bonus);
  if (word_multiplier >= 4) {
    return PAT_PREMIUM_QWS;
  }
  if (word_multiplier == 3) {
    return PAT_PREMIUM_TWS;
  }
  if (word_multiplier == 2) {
    return PAT_PREMIUM_DWS;
  }
  const int letter_multiplier = bonus_square_get_letter_multiplier(bonus);
  if (letter_multiplier >= 4) {
    return PAT_PREMIUM_QLS;
  }
  if (letter_multiplier == 3) {
    return PAT_PREMIUM_TLS;
  }
  if (letter_multiplier == 2) {
    return PAT_PREMIUM_DLS;
  }
  return -1;
}

// Finds up to PAT_MAX_PREMIUM uncovered premium squares in row-major order
// (the truncation is deterministic, so training and evaluation always
// agree). Bricked and occupied squares are excluded: a covered premium
// square can never be uncovered by a move.
int pat_find_premium_squares(const Square *lanes, uint8_t *premium_rows,
                             uint8_t *premium_cols, uint8_t *premium_classes) {
  int num_premium = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    const Square *lane =
        board_get_row_cache(lanes, row, BOARD_HORIZONTAL_DIRECTION);
    for (int col = 0; col < BOARD_DIM; col++) {
      const Square *square = &lane[col];
      if (square_get_is_brick(square) ||
          square_get_letter(square) != ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      const int premium_class = pat_premium_class_of(square);
      if (premium_class < 0) {
        continue;
      }
      if (num_premium == PAT_MAX_PREMIUM) {
        return num_premium;
      }
      premium_rows[num_premium] = (uint8_t)row;
      premium_cols[num_premium] = (uint8_t)col;
      premium_classes[num_premium] = (uint8_t)premium_class;
      num_premium++;
    }
  }
  return num_premium;
}

void pat_extract_features(const Square *lanes, const LetterDistribution *ld,
                          const Rack *player_rack, const PATWeights *pat,
                          int opponent_rack_size, int32_t *features) {
  memset(features, 0, sizeof(int32_t) * PAT_NUM_FEATURES);
  uint8_t unseen_counts[MAX_ALPHABET_SIZE];
  pat_compute_unseen_counts(lanes, ld, player_rack, unseen_counts);
  uint8_t premium_rows[PAT_MAX_PREMIUM];
  uint8_t premium_cols[PAT_MAX_PREMIUM];
  uint8_t premium_classes[PAT_MAX_PREMIUM];
  const int num_premium = pat_find_premium_squares(
      lanes, premium_rows, premium_cols, premium_classes);
  for (int premium_idx = 0; premium_idx < num_premium; premium_idx++) {
    pat_scan_unit(lanes, ld, unseen_counts, pat, premium_rows[premium_idx],
                  premium_cols[premium_idx], premium_classes[premium_idx],
                  BOARD_HORIZONTAL_DIRECTION, NULL, features, NULL, NULL,
                  opponent_rack_size, NULL, true);
    pat_scan_unit(lanes, ld, unseen_counts, pat, premium_rows[premium_idx],
                  premium_cols[premium_idx], premium_classes[premium_idx],
                  BOARD_VERTICAL_DIRECTION, NULL, features, NULL, NULL,
                  opponent_rack_size, NULL, true);
  }
  uint8_t dd_dirs[PAT_MAX_DD];
  uint8_t dd_lanes[PAT_MAX_DD];
  uint8_t dd_los[PAT_MAX_DD];
  uint8_t dd_his[PAT_MAX_DD];
  uint8_t dd_tiers[PAT_MAX_DD];
  const int num_dd =
      pat_find_dd(lanes, dd_dirs, dd_lanes, dd_los, dd_his, dd_tiers);
  for (int dd_idx = 0; dd_idx < num_dd; dd_idx++) {
    pat_scan_dd_unit(lanes, unseen_counts, dd_dirs[dd_idx], dd_lanes[dd_idx],
                     dd_los[dd_idx], dd_his[dd_idx], dd_tiers[dd_idx], NULL,
                     features, NULL, NULL, opponent_rack_size, NULL);
  }
}

int64_t pat_dot_raw(const PATWeights *pat, const int32_t *features) {
  int64_t acc = 0;
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    acc += (int64_t)pat->weights[feature_index] * features[feature_index];
  }
  return acc;
}

Equity pat_clamp_dot(int64_t acc) {
  if (acc < EQUITY_MIN_VALUE) {
    acc = EQUITY_MIN_VALUE;
  }
  if (acc > 0) {
    // Cannot happen with the enforced weight and feature signs; clamp
    // anyway so the shadow invariant survives any future bug here.
    acc = 0;
  }
  return (Equity)acc;
}

Equity pat_dot(const PATWeights *pat, const int32_t *features) {
  return pat_clamp_dot(pat_dot_raw(pat, features));
}

void pat_extract_features_combined(const Square *lanes,
                                   const LetterDistribution *ld,
                                   const Rack *player_rack,
                                   const PATWeights *pat,
                                   int opponent_rack_size, double *features) {
  pat_require_prepared(pat);
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    features[feature_index] = 0.0;
  }
  uint8_t unseen_counts[MAX_ALPHABET_SIZE];
  pat_compute_unseen_counts(lanes, ld, player_rack, unseen_counts);
  uint8_t premium_rows[PAT_MAX_PREMIUM];
  uint8_t premium_cols[PAT_MAX_PREMIUM];
  uint8_t premium_classes[PAT_MAX_PREMIUM];
  const int num_premium = pat_find_premium_squares(
      lanes, premium_rows, premium_cols, premium_classes);
  uint8_t dd_dirs[PAT_MAX_DD];
  uint8_t dd_lanes[PAT_MAX_DD];
  uint8_t dd_los[PAT_MAX_DD];
  uint8_t dd_his[PAT_MAX_DD];
  uint8_t dd_tiers[PAT_MAX_DD];
  const int num_dd =
      pat_find_dd(lanes, dd_dirs, dd_lanes, dd_los, dd_his, dd_tiers);
  const int num_units = num_premium * 2 + num_dd;

  int32_t unit_features[PAT_MAX_SCAN_UNITS][PAT_NUM_FEATURES];
  int worst_unit = -1;
  Equity worst_penalty = 0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    int32_t *row = unit_features[unit_index];
    memset(row, 0, sizeof(int32_t) * PAT_NUM_FEATURES);
    if (unit_index < num_premium * 2) {
      const int premium_idx = unit_index / 2;
      pat_scan_unit(lanes, ld, unseen_counts, pat, premium_rows[premium_idx],
                    premium_cols[premium_idx], premium_classes[premium_idx],
                    unit_index % 2, NULL, row, NULL, NULL, opponent_rack_size,
                    NULL, true);
    } else {
      const int dd_idx = unit_index - num_premium * 2;
      pat_scan_dd_unit(lanes, unseen_counts, dd_dirs[dd_idx], dd_lanes[dd_idx],
                       dd_los[dd_idx], dd_his[dd_idx], dd_tiers[dd_idx], NULL,
                       row, NULL, NULL, opponent_rack_size, NULL);
    }
    const Equity penalty = pat_dot(pat, row);
    if (penalty < worst_penalty) {
      worst_penalty = penalty;
      worst_unit = unit_index;
    }
  }

  // Untrained weights rank every unit alike, so there is no worst one to
  // charge in full. Fall back to the sum, which makes the first generation
  // an ordinary fit and gives later ones something to rank with.
  const double gamma = (worst_unit >= 0) ? pat->combine_gamma : 1.0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
         feature_index++) {
      features[feature_index] +=
          gamma * (double)unit_features[unit_index][feature_index];
    }
  }
  if (worst_unit >= 0) {
    for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
         feature_index++) {
      features[feature_index] +=
          (1.0 - gamma) * (double)unit_features[worst_unit][feature_index];
    }
  }
}
