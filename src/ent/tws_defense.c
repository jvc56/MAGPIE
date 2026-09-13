#include "tws_defense.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/tws_defense_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "board.h"
#include "bonus_square.h"
#include "data_filepaths.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct TWDWeights {
  char *name;
  Equity weights[TWD_NUM_FEATURES];
  // Per-letter count of two-letter words containing the letter; the
  // flexibility approximation for hooks and floaters the evaluated move
  // itself creates (see twd_prepare_hook_flex).
  uint8_t hook_flex[MAX_ALPHABET_SIZE];
  // How much a second route to danger counts once the worst one is already
  // counted. The opponent plays one move, so the threats a board offers do
  // not simply add: 0 charges only the worst scan unit, 1 charges every
  // unit in full (the original behaviour), and values between allow for a
  // rack that cannot use the worst route. See twd_combine_unit_penalties.
  double combine_gamma;
  uint64_t mutation_counter;
};

double twd_get_combine_gamma(const TWDWeights *twd) {
  return twd->combine_gamma;
}

void twd_set_combine_gamma(TWDWeights *twd, double combine_gamma) {
  twd->combine_gamma = combine_gamma;
}

const char *twd_get_name(const TWDWeights *twd) { return twd->name; }

Equity twd_get_weight(const TWDWeights *twd, int feature_index) {
  return twd->weights[feature_index];
}

void twd_set_weight(TWDWeights *twd, int feature_index, Equity weight) {
  if (weight > 0) {
    log_fatal("TWS defense weight for feature %d must be <= 0, got %d",
              feature_index, weight);
  }
  twd->weights[feature_index] = weight;
}

uint64_t twd_get_mutation_counter(const TWDWeights *twd) {
  return twd->mutation_counter;
}

void twd_bump_mutation_counter(TWDWeights *twd) { twd->mutation_counter++; }

void twd_feature_name(int feature_index, char *buf, size_t buf_size) {
  if (feature_index >= TWD_FEATURE_HOOK_START &&
      feature_index < TWD_FEATURE_FLOAT_FLEX_START) {
    snprintf(buf, buf_size, "hook_d%d",
             feature_index - TWD_FEATURE_HOOK_START + 1);
  } else if (feature_index < TWD_FEATURE_FLOAT_SCORE_START) {
    snprintf(buf, buf_size, "float_flex_d%d",
             feature_index - TWD_FEATURE_FLOAT_FLEX_START + 1);
  } else if (feature_index < TWD_FEATURE_TT_FLOATER) {
    snprintf(buf, buf_size, "float_score_d%d",
             feature_index - TWD_FEATURE_FLOAT_SCORE_START + 1);
  } else if (feature_index == TWD_FEATURE_TT_FLOATER) {
    snprintf(buf, buf_size, "tt_floater");
  } else if (feature_index == TWD_FEATURE_TT_HOOK_ONLY) {
    snprintf(buf, buf_size, "tt_hook_only");
  } else if (feature_index == TWD_FEATURE_DD_FLOATER) {
    snprintf(buf, buf_size, "dd_floater");
  } else if (feature_index == TWD_FEATURE_DD_HOOK_ONLY) {
    snprintf(buf, buf_size, "dd_hook_only");
  } else if (feature_index == TWD_FEATURE_DD_TILES_SAVED) {
    snprintf(buf, buf_size, "dd_tiles_saved");
  } else {
    log_fatal("invalid TWS defense feature index: %d", feature_index);
  }
}

TWDWeights *twd_create_zeroed(const char *twd_name) {
  TWDWeights *twd = calloc_or_die(1, sizeof(TWDWeights));
  twd->name = string_duplicate(twd_name);
  twd->combine_gamma = TWD_DEFAULT_COMBINE_GAMMA;
  return twd;
}

void twd_destroy(TWDWeights *twd) {
  if (!twd) {
    return;
  }
  free(twd->name);
  free(twd);
}

// Parses the weights file contents into twd. The format is:
//   line 1: the magic header (TWD_MAGIC_HEADER)
//   then, ignoring empty lines and lines starting with '#', exactly
//   TWD_NUM_FEATURES lines of "<feature_name>,<millipoints>", in canonical
//   feature order, every value <= 0.
static void twd_parse_contents(TWDWeights *twd, const char *twd_name,
                               const StringSplitter *split_contents,
                               ErrorStack *error_stack) {
  const int num_lines = string_splitter_get_number_of_items(split_contents);
  if (num_lines < 1 ||
      !strings_equal(string_splitter_get_item(split_contents, 0),
                     TWD_MAGIC_HEADER)) {
    error_stack_push(
        error_stack, ERROR_STATUS_TWD_INVALID_HEADER,
        get_formatted_string(
            "TWS defense file '%s' does not start with the header '%s'",
            twd_name, TWD_MAGIC_HEADER));
    return;
  }
  int feature_index = 0;
  char expected_name[64];
  for (int line_index = 1; line_index < num_lines; line_index++) {
    const char *line = string_splitter_get_item(split_contents, line_index);
    if (is_string_empty_or_whitespace(line) || line[0] == '#') {
      continue;
    }
    if (has_prefix(TWD_GAMMA_ROW_PREFIX, line)) {
      twd->combine_gamma = strtod(line + strlen(TWD_GAMMA_ROW_PREFIX), NULL);
      continue;
    }
    if (feature_index >= TWD_NUM_FEATURES) {
      error_stack_push(error_stack, ERROR_STATUS_TWD_WRONG_NUMBER_OF_ROWS,
                       get_formatted_string(
                           "TWS defense file '%s' has more than %d weight rows",
                           twd_name, TWD_NUM_FEATURES));
      return;
    }
    const char *comma = strchr(line, ',');
    if (!comma) {
      error_stack_push(
          error_stack, ERROR_STATUS_TWD_INVALID_ROW,
          get_formatted_string(
              "TWS defense file '%s' line %d is not '<name>,<value>': %s",
              twd_name, line_index + 1, line));
      return;
    }
    twd_feature_name(feature_index, expected_name, sizeof(expected_name));
    const size_t name_length = (size_t)(comma - line);
    if (strlen(expected_name) != name_length ||
        strncmp(line, expected_name, name_length) != 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_TWD_INVALID_ROW,
          get_formatted_string("TWS defense file '%s' line %d names feature "
                               "'%.*s' but '%s' was expected",
                               twd_name, line_index + 1, (int)name_length, line,
                               expected_name));
      return;
    }
    const int weight = string_to_int(comma + 1, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_TWD_INVALID_ROW,
          get_formatted_string(
              "TWS defense file '%s' line %d has an invalid weight: %s",
              twd_name, line_index + 1, comma + 1));
      return;
    }
    if (weight > 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_TWD_POSITIVE_WEIGHT,
          get_formatted_string(
              "TWS defense file '%s' line %d has a positive weight (%d); "
              "applied weights must be <= 0 so the defense term can never "
              "increase a move's equity",
              twd_name, line_index + 1, weight));
      return;
    }
    twd->weights[feature_index] = weight;
    feature_index++;
  }
  if (feature_index != TWD_NUM_FEATURES) {
    error_stack_push(
        error_stack, ERROR_STATUS_TWD_WRONG_NUMBER_OF_ROWS,
        get_formatted_string(
            "TWS defense file '%s' has %d weight rows but %d were expected",
            twd_name, feature_index, TWD_NUM_FEATURES));
  }
}

TWDWeights *twd_create(const char *data_paths, const char *twd_name,
                       ErrorStack *error_stack) {
  char *twd_filename = data_filepaths_get_readable_filename(
      data_paths, twd_name, DATA_FILEPATH_TYPE_TWS_DEFENSE, error_stack);
  TWDWeights *twd = NULL;
  if (error_stack_is_empty(error_stack)) {
    char *file_contents =
        fileproxy_get_string_from_filename(twd_filename, error_stack);
    if (error_stack_is_empty(error_stack)) {
      StringSplitter *split_contents =
          split_string_by_newline(file_contents, error_stack);
      if (error_stack_is_empty(error_stack)) {
        twd = twd_create_zeroed(twd_name);
        twd_parse_contents(twd, twd_name, split_contents, error_stack);
      }
      string_splitter_destroy(split_contents);
    }
    free(file_contents);
  }
  free(twd_filename);
  if (!error_stack_is_empty(error_stack)) {
    twd_destroy(twd);
    twd = NULL;
  }
  return twd;
}

void twd_write(const TWDWeights *twd, const char *data_paths,
               const char *twd_name, ErrorStack *error_stack) {
  char *twd_filename = data_filepaths_get_writable_filename(
      data_paths, twd_name, DATA_FILEPATH_TYPE_TWS_DEFENSE, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(twd_filename);
    return;
  }
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(sb, "%s\n", TWD_MAGIC_HEADER);
  string_builder_add_formatted_string(sb, "%s%.6f\n", TWD_GAMMA_ROW_PREFIX,
                                      twd->combine_gamma);
  string_builder_add_string(
      sb, "# trained TWS defense weights; units: milli-equity per feature "
          "unit; all values <= 0\n");
  char feature_name[64];
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    twd_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,%d\n", feature_name,
                                        twd->weights[feature_index]);
  }
  write_string_to_file(twd_filename, "w", string_builder_peek(sb), error_stack);
  string_builder_destroy(sb);
  free(twd_filename);
}

void twd_prepare_hook_flex(TWDWeights *twd, const KWG *kwg,
                           const LetterDistribution *ld) {
  memset(twd->hook_flex, 0, sizeof(twd->hook_flex));
  if (!kwg) {
    return;
  }
  int counts[MAX_ALPHABET_SIZE] = {0};
  const uint32_t dawg_root = kwg_get_dawg_root_node_index(kwg);
  const int ld_size = ld_get_size(ld);
  for (int first_ml = 1; first_ml < ld_size; first_ml++) {
    const uint32_t node_index =
        kwg_get_next_node_index(kwg, dawg_root, (MachineLetter)first_ml);
    if (node_index == 0) {
      continue;
    }
    uint64_t extension_set = 0;
    const uint64_t accepted_set =
        kwg_get_letter_sets(kwg, node_index, &extension_set) & ~(uint64_t)1;
    for (int second_ml = 1; second_ml < ld_size; second_ml++) {
      if (accepted_set & ((uint64_t)1 << second_ml)) {
        counts[first_ml]++;
        counts[second_ml]++;
      }
    }
  }
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    if (counts[ml] > UINT8_MAX) {
      counts[ml] = UINT8_MAX;
    }
    twd->hook_flex[ml] = (uint8_t)counts[ml];
  }
}

int twd_get_hook_flex(const TWDWeights *twd, MachineLetter ml) {
  return twd->hook_flex[ml];
}

static inline int twd_ctz(uint64_t bits) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(bits);
#else
  int count = 0;
  while (!(bits & 1)) {
    bits >>= 1;
    count++;
  }
  return count;
#endif
}

// The blank marker occupies bit 0 of cross and extension sets; flexibility
// counts real letters only.
// The flexibility of a hook or extension point: how many tiles the opponent
// could still hold that fit it. Counting unseen tiles rather than the
// letters the set admits makes a hook needing a J the near-nothing it
// usually is, and makes a hook only the evaluating player can fill (its
// letters all sitting on their own rack) score as no threat at all.
static inline int twd_set_flex(const uint8_t *unseen_counts,
                               uint64_t letter_set) {
  int flex = 0;
  uint64_t remaining = letter_set & ~(uint64_t)1;
  while (remaining) {
    const int machine_letter = twd_ctz(remaining);
    remaining &= remaining - 1;
    flex += unseen_counts[machine_letter];
  }
  return flex;
}

// Fills unseen_counts (MAX_ALPHABET_SIZE entries) with the tiles that are
// neither on the board nor on the evaluating player's rack, which is
// exactly the pool the opponent draws from plus what they already hold.
// Blanks on the board are counted against the blank.
static void twd_compute_unseen_counts(const Square *lanes,
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

// Overlay describing the candidate move's fresh tiles on top of the
// pre-move board. hook_flex approximates the flexibility of hooks and
// floaters the move itself creates, whose real cross and extension sets do
// not exist yet.
typedef struct TWDMoveOverlay {
  const Move *move;
  int row_start;
  int col_start;
  int row_end;
  int col_end;
  bool vertical;
  const uint8_t *hook_flex;
} TWDMoveOverlay;

// Returns true and sets *fresh_letter_out if the move places a fresh tile
// on (row, col). Played-through positions fall through to the board.
static inline bool twd_move_covers(const TWDMoveOverlay *overlay, int row,
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

static inline int twd_unit_row(int dir, int lane_index, int idx) {
  return (dir == BOARD_HORIZONTAL_DIRECTION) ? lane_index : idx;
}

static inline int twd_unit_col(int dir, int lane_index, int idx) {
  return (dir == BOARD_HORIZONTAL_DIRECTION) ? idx : lane_index;
}

static inline MachineLetter twd_effective_letter(const Square *lane, int idx,
                                                 const TWDMoveOverlay *overlay,
                                                 int row, int col) {
  MachineLetter fresh_letter;
  if (twd_move_covers(overlay, row, col, &fresh_letter)) {
    return fresh_letter;
  }
  return square_get_letter(&lane[idx]);
}

typedef struct TWDCrossInfo {
  bool dead;
  bool hooky;
  int flex;
} TWDCrossInfo;

// The perpendicular constraint at an empty square: dead (no letter can be
// placed), hooky (constrained by an adjacent perpendicular word, i.e. a
// real hook), or unconstrained. When the move places a tile perpendicular-
// adjacent to the square, the real post-move cross set would require a KWG
// traversal, so it is approximated with the per-letter hook_flex table; a
// pre-move dead square is left dead even though a fresh adjacent tile
// technically changes its perpendicular pattern.
static TWDCrossInfo twd_effective_cross_info(const Square *lane, int idx,
                                             int dir,
                                             const TWDMoveOverlay *overlay,
                                             int row, int col,
                                             const uint8_t *unseen_counts) {
  const uint64_t base_cross_set = square_get_cross_set(&lane[idx]);
  TWDCrossInfo info;
  info.dead = (base_cross_set == 0);
  info.hooky = !info.dead && (base_cross_set != TRIVIAL_CROSS_SET);
  info.flex = info.hooky ? twd_set_flex(unseen_counts, base_cross_set) : 0;
  if (!overlay || info.dead) {
    return info;
  }
  int fresh_flex = -1;
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
    if (twd_move_covers(overlay, perp_row, perp_col, &fresh_letter)) {
      const int flex =
          overlay->hook_flex[get_unblanked_machine_letter(fresh_letter)];
      if (fresh_flex < 0 || flex < fresh_flex) {
        fresh_flex = flex;
      }
    }
  }
  if (fresh_flex >= 0) {
    info.flex = (info.hooky && info.flex < fresh_flex) ? info.flex : fresh_flex;
    info.hooky = true;
  }
  return info;
}

// Scans one (TWS, dir) unit: walks outward from the empty TWS square along
// its lane on both sides, accumulating hook, floater, and triple-triple
// features binned by the number of tiles a word reaching the TWS must play.
// A triple-triple span is counted from both of its endpoint TWS squares;
// the double counting is consistent between training and evaluation, so the
// trained weight absorbs it.
// extent_lo/extent_hi (optional) receive the lowest and highest lane index
// the walk visited, including the square it broke on: a move can only
// change this unit's features by placing a tile on one of those squares or
// directly beside them in the perpendicular direction.
static void twd_scan_unit(const Square *lanes, const LetterDistribution *ld,
                          const uint8_t *unseen_counts, int tws_row,
                          int tws_col, int dir, const TWDMoveOverlay *overlay,
                          int32_t *features, int *extent_lo, int *extent_hi) {
  const int lane_index =
      (dir == BOARD_HORIZONTAL_DIRECTION) ? tws_row : tws_col;
  const int tws_idx = (dir == BOARD_HORIZONTAL_DIRECTION) ? tws_col : tws_row;
  const Square *lane = board_get_row_cache(lanes, lane_index, dir);
  if (extent_lo) {
    *extent_lo = tws_idx;
  }
  if (extent_hi) {
    *extent_hi = tws_idx;
  }

  if (twd_effective_letter(lane, tws_idx, overlay,
                           twd_unit_row(dir, lane_index, tws_idx),
                           twd_unit_col(dir, lane_index, tws_idx)) !=
      ALPHABET_EMPTY_SQUARE_MARKER) {
    // The TWS square is covered (by the board or by the move itself):
    // nothing along this lane can reach it. Covering a TWS is exactly the
    // blocking reward.
    return;
  }
  const TWDCrossInfo tws_info = twd_effective_cross_info(
      lane, tws_idx, dir, overlay, twd_unit_row(dir, lane_index, tws_idx),
      twd_unit_col(dir, lane_index, tws_idx), unseen_counts);
  if (tws_info.dead) {
    // No word along this lane can cover the TWS square at all.
    return;
  }
  if (tws_info.hooky) {
    // A one-tile play on the TWS square itself completes a perpendicular
    // word at triple word score: hook access at d = 1.
    features[TWD_FEATURE_HOOK_START] += tws_info.flex;
  }

  for (int side = -1; side <= 1; side += 2) {
    // Number of empty squares a word covering the span from the current
    // scan position through the TWS square must fill, i.e. the number of
    // tiles the opponent must play. The TWS square itself is the first.
    int empties_used = 1;
    bool span_has_floater = false;
    int span_floater_flex = 0;
    bool span_has_hook = tws_info.hooky;
    int prev_empty_idx = tws_idx;
    int last_visited_idx = tws_idx;
    int idx = tws_idx + side;
    while (idx >= 0 && idx < BOARD_DIM) {
      last_visited_idx = idx;
      if (square_get_is_brick(&lane[idx])) {
        break;
      }
      const int square_row = twd_unit_row(dir, lane_index, idx);
      const int square_col = twd_unit_col(dir, lane_index, idx);
      const MachineLetter letter =
          twd_effective_letter(lane, idx, overlay, square_row, square_col);
      if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
        // A run of tiles: floater (playthrough) access. The run costs the
        // opponent no tiles, so the whole run shares the bin of the empty
        // span between it and the TWS square.
        const int distance_bin = empties_used;
        const MachineLetter facing_letter = letter;
        bool run_has_fresh_tile = false;
        while (idx >= 0 && idx < BOARD_DIM &&
               !square_get_is_brick(&lane[idx])) {
          const int run_row = twd_unit_row(dir, lane_index, idx);
          const int run_col = twd_unit_col(dir, lane_index, idx);
          const MachineLetter run_letter =
              twd_effective_letter(lane, idx, overlay, run_row, run_col);
          if (run_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
            break;
          }
          last_visited_idx = idx;
          MachineLetter fresh_letter;
          if (twd_move_covers(overlay, run_row, run_col, &fresh_letter)) {
            run_has_fresh_tile = true;
          }
          int tile_score = 0;
          if (!get_is_blanked(run_letter)) {
            tile_score = equity_to_int(ld_get_score(ld, run_letter));
          }
          features[TWD_FEATURE_FLOAT_SCORE_START + distance_bin - 1] +=
              tile_score;
          idx += side;
        }
        int run_flex;
        if (run_has_fresh_tile) {
          // The run's real extension sets do not exist yet; approximate
          // with the two-letter-word flexibility of the tile facing the
          // TWS square.
          run_flex =
              overlay->hook_flex[get_unblanked_machine_letter(facing_letter)];
        } else {
          // The empty square between the run and the TWS square carries the
          // extension set of the adjacent word on the run's side.
          const uint64_t extension_set =
              (side > 0) ? square_get_right_extension_set(&lane[prev_empty_idx])
                         : square_get_left_extension_set(&lane[prev_empty_idx]);
          run_flex = twd_set_flex(unseen_counts, extension_set);
        }
        features[TWD_FEATURE_FLOAT_FLEX_START + distance_bin - 1] += run_flex;
        span_has_floater = true;
        span_floater_flex += run_flex;
        continue;
      }
      const TWDCrossInfo info = twd_effective_cross_info(
          lane, idx, dir, overlay, square_row, square_col, unseen_counts);
      if (info.dead) {
        break;
      }
      empties_used++;
      if (empties_used > RACK_SIZE) {
        break;
      }
      if (bonus_square_get_word_multiplier(
              square_get_bonus_square(&lane[idx])) == 3) {
        // A second empty TWS within reach: the triple-triple span from the
        // scanned TWS square through this one.
        if (span_has_floater) {
          features[TWD_FEATURE_TT_FLOATER] += 1 + span_floater_flex;
        } else if (span_has_hook) {
          features[TWD_FEATURE_TT_HOOK_ONLY] += 1;
        }
        break;
      }
      if (info.hooky) {
        features[TWD_FEATURE_HOOK_START + empties_used - 1] += info.flex;
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
static void twd_scan_dd_unit(const Square *lanes, const uint8_t *unseen_counts,
                             int dir, int lane_index, int lo, int hi,
                             const TWDMoveOverlay *overlay, int32_t *features,
                             int *extent_lo, int *extent_hi) {
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
    const int square_row = twd_unit_row(dir, lane_index, idx);
    const int square_col = twd_unit_col(dir, lane_index, idx);
    const MachineLetter letter =
        twd_effective_letter(lane, idx, overlay, square_row, square_col);
    if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
      if (idx == lo || idx == hi) {
        return;
      }
      has_floater = true;
      continue;
    }
    const TWDCrossInfo info = twd_effective_cross_info(
        lane, idx, dir, overlay, square_row, square_col, unseen_counts);
    if (info.dead) {
      return;
    }
    if (info.hooky) {
      has_hook = true;
    }
    empties++;
  }
  if (empties > RACK_SIZE) {
    return;
  }
  if (has_floater) {
    features[TWD_FEATURE_DD_FLOATER] += 1;
  } else if (has_hook) {
    features[TWD_FEATURE_DD_HOOK_ONLY] += 1;
  } else {
    return;
  }
  features[TWD_FEATURE_DD_TILES_SAVED] += RACK_SIZE - empties;
}

// Finds the double-double windows: consecutive pairs of double word squares
// in one lane, near enough that a single word could cover both. Horizontal
// lanes come first, then vertical, each scanned in increasing order, so the
// truncation at TWD_MAX_DD is deterministic and training and evaluation
// always agree. Whether a window is currently live is left to the scan.
static int twd_find_dd(const Square *lanes, uint8_t *dd_dirs, uint8_t *dd_lanes,
                       uint8_t *dd_los, uint8_t *dd_his) {
  int num_dd = 0;
  for (int dir = 0; dir < 2; dir++) {
    for (int lane_index = 0; lane_index < BOARD_DIM; lane_index++) {
      const Square *lane = board_get_row_cache(lanes, lane_index, dir);
      int previous_dw = -1;
      for (int idx = 0; idx < BOARD_DIM; idx++) {
        if (bonus_square_get_word_multiplier(
                square_get_bonus_square(&lane[idx])) != 2) {
          continue;
        }
        if (previous_dw >= 0 && idx - previous_dw <= TWD_DD_MAX_SPAN) {
          if (num_dd == TWD_MAX_DD) {
            return num_dd;
          }
          dd_dirs[num_dd] = (uint8_t)dir;
          dd_lanes[num_dd] = (uint8_t)lane_index;
          dd_los[num_dd] = (uint8_t)previous_dw;
          dd_his[num_dd] = (uint8_t)idx;
          num_dd++;
        }
        previous_dw = idx;
      }
    }
  }
  return num_dd;
}

// Finds up to TWD_MAX_TWS uncovered TWS squares in row-major order (the
// truncation is deterministic, so training and evaluation always agree).
// Bricked and occupied squares are excluded: a covered TWS can never be
// uncovered by a move.
static int twd_find_tws(const Square *lanes, uint8_t *tws_rows,
                        uint8_t *tws_cols) {
  int num_tws = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    const Square *lane =
        board_get_row_cache(lanes, row, BOARD_HORIZONTAL_DIRECTION);
    for (int col = 0; col < BOARD_DIM; col++) {
      const Square *square = &lane[col];
      if (square_get_is_brick(square) ||
          square_get_letter(square) != ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      if (bonus_square_get_word_multiplier(square_get_bonus_square(square)) ==
          3) {
        if (num_tws == TWD_MAX_TWS) {
          return num_tws;
        }
        tws_rows[num_tws] = (uint8_t)row;
        tws_cols[num_tws] = (uint8_t)col;
        num_tws++;
      }
    }
  }
  return num_tws;
}

void twd_extract_features(const Square *lanes, const LetterDistribution *ld,
                          const Rack *player_rack, int32_t *features) {
  memset(features, 0, sizeof(int32_t) * TWD_NUM_FEATURES);
  uint8_t unseen_counts[MAX_ALPHABET_SIZE];
  twd_compute_unseen_counts(lanes, ld, player_rack, unseen_counts);
  uint8_t tws_rows[TWD_MAX_TWS];
  uint8_t tws_cols[TWD_MAX_TWS];
  const int num_tws = twd_find_tws(lanes, tws_rows, tws_cols);
  for (int tws_idx = 0; tws_idx < num_tws; tws_idx++) {
    twd_scan_unit(lanes, ld, unseen_counts, tws_rows[tws_idx],
                  tws_cols[tws_idx], BOARD_HORIZONTAL_DIRECTION, NULL, features,
                  NULL, NULL);
    twd_scan_unit(lanes, ld, unseen_counts, tws_rows[tws_idx],
                  tws_cols[tws_idx], BOARD_VERTICAL_DIRECTION, NULL, features,
                  NULL, NULL);
  }
  uint8_t dd_dirs[TWD_MAX_DD];
  uint8_t dd_lanes[TWD_MAX_DD];
  uint8_t dd_los[TWD_MAX_DD];
  uint8_t dd_his[TWD_MAX_DD];
  const int num_dd = twd_find_dd(lanes, dd_dirs, dd_lanes, dd_los, dd_his);
  for (int dd_idx = 0; dd_idx < num_dd; dd_idx++) {
    twd_scan_dd_unit(lanes, unseen_counts, dd_dirs[dd_idx], dd_lanes[dd_idx],
                     dd_los[dd_idx], dd_his[dd_idx], NULL, features, NULL,
                     NULL);
  }
}

static int64_t twd_dot_raw(const TWDWeights *twd, const int32_t *features) {
  int64_t acc = 0;
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    acc += (int64_t)twd->weights[feature_index] * features[feature_index];
  }
  return acc;
}

static Equity twd_dot(const TWDWeights *twd, const int32_t *features) {
  int64_t acc = twd_dot_raw(twd, features);
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

// The per-row and per-column unit masks are 64-bit.
static_assert(TWD_MAX_SCAN_UNITS <= 64,
              "TWS defense unit masks require at most 64 scan units");

// Combines the per-unit penalties into the position's defense term. The
// opponent plays one move next turn, so two open lanes are not two separate
// losses: the worst route is charged in full and every other route at
// gamma, which is a convex combination of the minimum and the sum. Gamma 1
// is the plain sum. Being a convex combination of non-positive values the
// result is non-positive, and it is nondecreasing in every unit penalty,
// which is what lets the shadow bound below zero out the units a move can
// reach.
static Equity twd_combine(int64_t worst, int64_t sum, double combine_gamma) {
  double combined =
      (1.0 - combine_gamma) * (double)worst + combine_gamma * (double)sum;
  if (combined > 0.0) {
    combined = 0.0;
  }
  if (combined < (double)EQUITY_MIN_VALUE) {
    combined = (double)EQUITY_MIN_VALUE;
  }
  return (Equity)llround(combined);
}

static Equity twd_combine_unit_penalties(const Equity *unit_penalties,
                                         int num_units, double combine_gamma) {
  int64_t sum = 0;
  int64_t worst = 0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    const int64_t penalty = unit_penalties[unit_index];
    sum += penalty;
    if (penalty < worst) {
      worst = penalty;
    }
  }
  return twd_combine(worst, sum, combine_gamma);
}

// The moves affecting the units can at best zero out each affected unit's (<=
// 0) baseline contribution; every unaffected unit keeps its baseline exactly.
static Equity twd_units_penalty_bound(const TWDEvalContext *twd_eval_ctx,
                                      uint64_t affected_units) {
  // A move can at best zero out every unit it reaches, and the combination
  // is nondecreasing in each unit, so combining with those units at zero
  // bounds the term from above. A zeroed unit adds nothing to the sum and
  // can never be the worst, so it simply drops out of both.
  int64_t sum = 0;
  int64_t worst = 0;
  for (int unit_index = 0; unit_index < twd_eval_ctx->num_units; unit_index++) {
    if ((affected_units >> unit_index) & 1) {
      continue;
    }
    const int64_t penalty = twd_eval_ctx->unit_penalty[unit_index];
    sum += penalty;
    if (penalty < worst) {
      worst = penalty;
    }
  }
  return twd_combine(worst, sum, twd_eval_ctx->weights->combine_gamma);
}

void twd_eval_context_disable(TWDEvalContext *twd_eval_ctx) {
  twd_eval_ctx->weights = NULL;
}

// Scans one of the context's units into `features`, reporting the lane it
// walks and the span of lane squares that walk could read. Units 2*i and
// 2*i+1 are TWS i's horizontal and vertical walks; the units after those are
// the double-double windows.
static void twd_scan_context_unit(const TWDEvalContext *twd_eval_ctx,
                                  int unit_index, const TWDMoveOverlay *overlay,
                                  int32_t *features, int *dir_out,
                                  int *lane_out, int *extent_lo,
                                  int *extent_hi) {
  const int num_tws_units = twd_eval_ctx->num_tws * 2;
  if (unit_index < num_tws_units) {
    const int tws_idx = unit_index / 2;
    const int dir = unit_index % 2;
    const int tws_row = twd_eval_ctx->tws_rows[tws_idx];
    const int tws_col = twd_eval_ctx->tws_cols[tws_idx];
    *dir_out = dir;
    *lane_out = (dir == BOARD_HORIZONTAL_DIRECTION) ? tws_row : tws_col;
    twd_scan_unit(twd_eval_ctx->lanes, twd_eval_ctx->ld,
                  twd_eval_ctx->unseen_counts, tws_row, tws_col, dir, overlay,
                  features, extent_lo, extent_hi);
    return;
  }
  const int dd_idx = unit_index - num_tws_units;
  *dir_out = twd_eval_ctx->dd_dirs[dd_idx];
  *lane_out = twd_eval_ctx->dd_lanes[dd_idx];
  twd_scan_dd_unit(twd_eval_ctx->lanes, twd_eval_ctx->unseen_counts,
                   twd_eval_ctx->dd_dirs[dd_idx],
                   twd_eval_ctx->dd_lanes[dd_idx], twd_eval_ctx->dd_los[dd_idx],
                   twd_eval_ctx->dd_his[dd_idx], overlay, features, extent_lo,
                   extent_hi);
}

void twd_eval_context_load(TWDEvalContext *twd_eval_ctx,
                           const TWDWeights *weights, const Square *lanes,
                           const LetterDistribution *ld,
                           const Rack *player_rack) {
  twd_eval_ctx->weights = weights;
  if (!weights) {
    return;
  }
  twd_eval_ctx->ld = ld;
  twd_eval_ctx->lanes = lanes;
  twd_compute_unseen_counts(lanes, ld, player_rack,
                            twd_eval_ctx->unseen_counts);
  twd_eval_ctx->num_tws =
      twd_find_tws(lanes, twd_eval_ctx->tws_rows, twd_eval_ctx->tws_cols);
  twd_eval_ctx->num_dd =
      twd_find_dd(lanes, twd_eval_ctx->dd_dirs, twd_eval_ctx->dd_lanes,
                  twd_eval_ctx->dd_los, twd_eval_ctx->dd_his);
  twd_eval_ctx->num_units = twd_eval_ctx->num_tws * 2 + twd_eval_ctx->num_dd;
  memset(twd_eval_ctx->unit_mask_by_row, 0,
         sizeof(twd_eval_ctx->unit_mask_by_row));
  memset(twd_eval_ctx->unit_mask_by_col, 0,
         sizeof(twd_eval_ctx->unit_mask_by_col));
  int32_t features[TWD_NUM_FEATURES] = {0};
  for (int unit_index = 0; unit_index < twd_eval_ctx->num_units; unit_index++) {
    int32_t *unit_features = twd_eval_ctx->unit_features[unit_index];
    memset(unit_features, 0, sizeof(int32_t) * TWD_NUM_FEATURES);
    int dir = 0;
    int lane = 0;
    int extent_lo = 0;
    int extent_hi = 0;
    twd_scan_context_unit(twd_eval_ctx, unit_index, NULL, unit_features, &dir,
                          &lane, &extent_lo, &extent_hi);
    twd_eval_ctx->unit_penalty[unit_index] = twd_dot(weights, unit_features);
    for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
         feature_index++) {
      features[feature_index] += unit_features[feature_index];
    }
    // A move affects this unit only when it has a tile on or directly
    // beside the lane (perpendicular halo of one) within the span of
    // squares the baseline walk visited: squares beyond the walk's break
    // point are unreachable within the empty-square budget either way.
    // Farther effects (a move extending a distant perpendicular word
    // into a lane square's cross set) are deliberately ignored in the
    // per-move delta.
    const uint64_t unit_bit = (uint64_t)1 << unit_index;
    uint64_t *halo_masks = (dir == BOARD_HORIZONTAL_DIRECTION)
                               ? twd_eval_ctx->unit_mask_by_row
                               : twd_eval_ctx->unit_mask_by_col;
    uint64_t *extent_masks = (dir == BOARD_HORIZONTAL_DIRECTION)
                                 ? twd_eval_ctx->unit_mask_by_col
                                 : twd_eval_ctx->unit_mask_by_row;
    for (int halo = lane - 1; halo <= lane + 1; halo++) {
      if (halo >= 0 && halo < BOARD_DIM) {
        halo_masks[halo] |= unit_bit;
      }
    }
    for (int idx = extent_lo; idx <= extent_hi; idx++) {
      extent_masks[idx] |= unit_bit;
    }
  }
  twd_eval_ctx->pre_penalty = twd_combine_unit_penalties(
      twd_eval_ctx->unit_penalty, twd_eval_ctx->num_units,
      weights->combine_gamma);
  for (int lane = 0; lane < BOARD_DIM; lane++) {
    twd_eval_ctx->lane_penalty_bound[BOARD_HORIZONTAL_DIRECTION][lane] =
        twd_units_penalty_bound(twd_eval_ctx,
                                twd_eval_ctx->unit_mask_by_row[lane]);
    twd_eval_ctx->lane_penalty_bound[BOARD_VERTICAL_DIRECTION][lane] =
        twd_units_penalty_bound(twd_eval_ctx,
                                twd_eval_ctx->unit_mask_by_col[lane]);
  }
}

// Returns the bitset of scan units the move can affect (see the
// unit_mask_by_row comment in the header).
static inline uint64_t
twd_move_affected_units(const TWDEvalContext *twd_eval_ctx, int row_start,
                        int row_end, int col_start, int col_end) {
  uint64_t row_units = 0;
  for (int row = row_start; row <= row_end; row++) {
    row_units |= twd_eval_ctx->unit_mask_by_row[row];
  }
  uint64_t col_units = 0;
  for (int col = col_start; col <= col_end; col++) {
    col_units |= twd_eval_ctx->unit_mask_by_col[col];
  }
  return row_units & col_units;
}

Equity twd_eval_move_penalty_bound(const TWDEvalContext *twd_eval_ctx,
                                   const Move *move) {
  if (!twd_eval_ctx || !twd_eval_ctx->weights) {
    return 0;
  }
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return twd_eval_ctx->pre_penalty;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int tiles_length = move_get_tiles_length(move);
  const int row_end = vertical ? row_start + tiles_length - 1 : row_start;
  const int col_end = vertical ? col_start : col_start + tiles_length - 1;
  return twd_units_penalty_bound(
      twd_eval_ctx, twd_move_affected_units(twd_eval_ctx, row_start, row_end,
                                            col_start, col_end));
}

Equity twd_eval_move_penalty(const TWDEvalContext *twd_eval_ctx,
                             const Move *move) {
  if (!twd_eval_ctx || !twd_eval_ctx->weights) {
    return 0;
  }
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    // Exchanges and passes leave the board unchanged, so their defense term
    // is exactly the position baseline. Including it keeps the comparison
    // against tile placements (whose term is baseline plus delta) fair.
    return twd_eval_ctx->pre_penalty;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int tiles_length = move_get_tiles_length(move);
  const int row_end = vertical ? row_start + tiles_length - 1 : row_start;
  const int col_end = vertical ? col_start : col_start + tiles_length - 1;
  uint64_t affected_units = twd_move_affected_units(
      twd_eval_ctx, row_start, row_end, col_start, col_end);
  if (!affected_units) {
    return twd_eval_ctx->pre_penalty;
  }
  const TWDMoveOverlay overlay = {
      .move = move,
      .row_start = row_start,
      .col_start = col_start,
      .row_end = row_end,
      .col_end = col_end,
      .vertical = vertical,
      .hook_flex = twd_eval_ctx->weights->hook_flex,
  };
  // Rescan only the units the move can reach and combine the whole set:
  // the units it cannot reach keep exactly the penalty they were loaded
  // with.
  Equity post_move_penalties[TWD_MAX_SCAN_UNITS];
  memcpy(post_move_penalties, twd_eval_ctx->unit_penalty,
         sizeof(Equity) * (size_t)twd_eval_ctx->num_units);
  while (affected_units) {
    const int unit_index = twd_ctz(affected_units);
    affected_units &= affected_units - 1;
    int32_t overlay_features[TWD_NUM_FEATURES] = {0};
    int scan_dir = 0;
    int scan_lane = 0;
    twd_scan_context_unit(twd_eval_ctx, unit_index, &overlay, overlay_features,
                          &scan_dir, &scan_lane, NULL, NULL);
    post_move_penalties[unit_index] =
        twd_dot(twd_eval_ctx->weights, overlay_features);
  }
  return twd_combine_unit_penalties(post_move_penalties,
                                    twd_eval_ctx->num_units,
                                    twd_eval_ctx->weights->combine_gamma);
}

void twd_extract_features_combined(const Square *lanes,
                                   const LetterDistribution *ld,
                                   const Rack *player_rack,
                                   const TWDWeights *twd, double *features) {
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    features[feature_index] = 0.0;
  }
  uint8_t unseen_counts[MAX_ALPHABET_SIZE];
  twd_compute_unseen_counts(lanes, ld, player_rack, unseen_counts);
  uint8_t tws_rows[TWD_MAX_TWS];
  uint8_t tws_cols[TWD_MAX_TWS];
  const int num_tws = twd_find_tws(lanes, tws_rows, tws_cols);
  uint8_t dd_dirs[TWD_MAX_DD];
  uint8_t dd_lanes[TWD_MAX_DD];
  uint8_t dd_los[TWD_MAX_DD];
  uint8_t dd_his[TWD_MAX_DD];
  const int num_dd = twd_find_dd(lanes, dd_dirs, dd_lanes, dd_los, dd_his);
  const int num_units = num_tws * 2 + num_dd;

  int32_t unit_features[TWD_MAX_SCAN_UNITS][TWD_NUM_FEATURES];
  int worst_unit = -1;
  Equity worst_penalty = 0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    int32_t *row = unit_features[unit_index];
    memset(row, 0, sizeof(int32_t) * TWD_NUM_FEATURES);
    if (unit_index < num_tws * 2) {
      const int tws_idx = unit_index / 2;
      twd_scan_unit(lanes, ld, unseen_counts, tws_rows[tws_idx],
                    tws_cols[tws_idx], unit_index % 2, NULL, row, NULL, NULL);
    } else {
      const int dd_idx = unit_index - num_tws * 2;
      twd_scan_dd_unit(lanes, unseen_counts, dd_dirs[dd_idx], dd_lanes[dd_idx],
                       dd_los[dd_idx], dd_his[dd_idx], NULL, row, NULL, NULL);
    }
    const Equity penalty = twd_dot(twd, row);
    if (penalty < worst_penalty) {
      worst_penalty = penalty;
      worst_unit = unit_index;
    }
  }

  // Untrained weights rank every unit alike, so there is no worst one to
  // charge in full. Fall back to the sum, which makes the first generation
  // an ordinary fit and gives later ones something to rank with.
  const double gamma = (worst_unit >= 0) ? twd->combine_gamma : 1.0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
         feature_index++) {
      features[feature_index] +=
          gamma * (double)unit_features[unit_index][feature_index];
    }
  }
  if (worst_unit >= 0) {
    for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
         feature_index++) {
      features[feature_index] +=
          (1.0 - gamma) * (double)unit_features[worst_unit][feature_index];
    }
  }
}
