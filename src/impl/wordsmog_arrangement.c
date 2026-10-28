#include "wordsmog_arrangement.h"

#include "../def/bit_rack_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/wordsmog_defs.h"
#include "../ent/equity.h"
#include "../ent/letter_distribution.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  // At least the most ways to choose the blanks' squares: each square, or
  // each pair of squares.
  WORDSMOG_MAX_BLANK_CHOICES = RACK_SIZE * RACK_SIZE,
};

// A key ranks a partial arrangement by equity, then score: equity scaled
// above a score that stays below the scale, since tile scores and
// coefficients are nonnegative.
static const int64_t WORDSMOG_KEY_SCALE = (int64_t)1 << 32;
static const int64_t WORDSMOG_NO_KEY = INT64_MIN;

static inline int64_t wordsmog_key(Equity score, Equity adjustment) {
  return (int64_t)(score + adjustment) * WORDSMOG_KEY_SCALE + score;
}

// Index of the lowest set bit of a nonzero bitset, following the
// guarded-intrinsic pattern.
static inline int wordsmog_lowest_bit(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(bitset);
#else
  int index = 0;
  while ((bitset & 1U) == 0) {
    bitset >>= 1U;
    index++;
  }
  return index;
#endif
}

// The number of set bits, following the guarded-intrinsic pattern.
static inline int wordsmog_bit_count(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_popcountll)
  return __builtin_popcountll(bitset);
#else
  int count = 0;
  while (bitset != 0) {
    bitset &= bitset - 1;
    count++;
  }
  return count;
#endif
}

// Sorts values, at most RACK_SIZE of them, highest first.
static void sort_descending(Equity *values, int num_values) {
  for (int value_idx = 1; value_idx < num_values; value_idx++) {
    const Equity value = values[value_idx];
    int insert_idx = value_idx;
    while (insert_idx > 0 && values[insert_idx - 1] < value) {
      values[insert_idx] = values[insert_idx - 1];
      insert_idx--;
    }
    values[insert_idx] = value;
  }
}

// Builds the tiles' states (see WordsmogTiles.num_states), which only the DP
// and the counts read, so most tiles, whose placements are settled without
// them, never need them.
static void wordsmog_tiles_build_states(WordsmogTiles *tiles) {
  int num_states = 1;
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    tiles->strides[letter_idx] = num_states;
    num_states *= tiles->counts[letter_idx] + 1;
  }
  assert(num_states <= WORDSMOG_MAX_STATES);
  tiles->num_states = num_states;
  // Count through the states as an odometer, one digit per letter, and count
  // how many states have each number of tiles.
  uint8_t digits[RACK_SIZE] = {0};
  uint8_t totals[WORDSMOG_MAX_STATES];
  int num_with_total[RACK_SIZE + 1] = {0};
  for (int state = 0; state < num_states; state++) {
    int total = 0;
    uint8_t present = 0;
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      tiles->digits[state][letter_idx] = digits[letter_idx];
      total += digits[letter_idx];
      if (digits[letter_idx] > 0) {
        present |= (uint8_t)(1U << letter_idx);
      }
    }
    tiles->present[state] = present;
    totals[state] = (uint8_t)total;
    num_with_total[total]++;
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      if (digits[letter_idx] < tiles->counts[letter_idx]) {
        digits[letter_idx]++;
        break;
      }
      digits[letter_idx] = 0;
    }
  }
  tiles->state_starts[0] = 0;
  for (int total = 0; total <= RACK_SIZE; total++) {
    tiles->state_starts[total + 1] =
        (uint8_t)(tiles->state_starts[total] + num_with_total[total]);
  }
  int filled[RACK_SIZE + 1] = {0};
  for (int state = 0; state < num_states; state++) {
    const int total = totals[state];
    tiles->states_by_total[tiles->state_starts[total] + filled[total]++] =
        (uint8_t)state;
  }
}

void wordsmog_tiles_set_states(WordsmogTiles *tiles) {
  tiles->num_states = 0;
  int num_tiles = 0;
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    for (int count = 0; count < tiles->counts[letter_idx]; count++) {
      tiles->descending_scores[num_tiles++] = tiles->scores[letter_idx];
    }
  }
  for (int blank_idx = 0; blank_idx < tiles->num_blanks; blank_idx++) {
    tiles->descending_scores[num_tiles++] = tiles->blank_score;
  }
  sort_descending(tiles->descending_scores, num_tiles);
}

Equity wordsmog_score_bound(const WordsmogSquare *squares, int num_squares,
                            const WordsmogTiles *tiles) {
  Equity coefficients[RACK_SIZE];
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    coefficients[square_idx] = squares[square_idx].coefficient;
  }
  sort_descending(coefficients, num_squares);
  Equity bound = 0;
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    bound += tiles->descending_scores[square_idx] * coefficients[square_idx];
  }
  return bound;
}

Equity wordsmog_cross_set_bound(const WordsmogSquare *squares, int num_squares,
                                const WordsmogTiles *tiles,
                                Equity score_bound) {
  Equity bound = 0;
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    const WordsmogSquare *square = &squares[square_idx];
    Equity square_best = tiles->num_blanks > 0 ? tiles->blank_score : 0;
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      if (tiles->scores[letter_idx] > square_best &&
          ((square->cross_set >> tiles->letters[letter_idx]) & 1U) != 0) {
        square_best = tiles->scores[letter_idx];
      }
    }
    bound += square_best * square->coefficient;
  }
  return bound < score_bound ? bound : score_bound;
}

// The key of the tile letter_idx on square.
static inline int64_t natural_key(const WordsmogSquare *square,
                                  const WordsmogTiles *tiles, int letter_idx) {
  const MachineLetter ml = tiles->letters[letter_idx];
  const Equity adjustment =
      ((tiles->vowels >> ml) & 1U) != 0 ? square->vowel_adjustment : 0;
  return wordsmog_key(tiles->scores[letter_idx] * square->coefficient,
                      adjustment);
}

static inline bool natural_allowed(const WordsmogSquare *square,
                                   const WordsmogTiles *tiles, int letter_idx) {
  return ((square->cross_set >> tiles->letters[letter_idx]) & 1U) != 0;
}

// Fills best[pos][state] with the best key of placing the tiles of state on
// the natural squares square_indices[pos..num_natural-1], for each state with
// as many tiles as those squares, or WORDSMOG_NO_KEY when they can't all be
// placed. Returns the key of placing every natural tile.
static int64_t natural_dp(const WordsmogSquare *squares,
                          const int *square_indices, int num_natural,
                          const WordsmogTiles *tiles,
                          int64_t best[RACK_SIZE + 1][WORDSMOG_MAX_STATES]) {
  best[num_natural][0] = 0;
  for (int pos = num_natural - 1; pos >= 0; pos--) {
    const WordsmogSquare *square = &squares[square_indices[pos]];
    const int total = num_natural - pos;
    // The square's key for each letter it allows, so that each state only
    // walks the letters it holds that the square allows.
    int64_t keys[RACK_SIZE];
    uint32_t allowed = 0;
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      if (natural_allowed(square, tiles, letter_idx)) {
        allowed |= 1U << letter_idx;
        keys[letter_idx] = natural_key(square, tiles, letter_idx);
      }
    }
    for (int state_idx = tiles->state_starts[total];
         state_idx < tiles->state_starts[total + 1]; state_idx++) {
      const int state = tiles->states_by_total[state_idx];
      int64_t state_best = WORDSMOG_NO_KEY;
      for (uint32_t candidates = tiles->present[state] & allowed;
           candidates != 0; candidates &= candidates - 1) {
        const int letter_idx = wordsmog_lowest_bit(candidates);
        const int64_t rest = best[pos + 1][state - tiles->strides[letter_idx]];
        if (rest == WORDSMOG_NO_KEY) {
          continue;
        }
        const int64_t key = keys[letter_idx] + rest;
        if (key > state_best) {
          state_best = key;
        }
      }
      best[pos][state] = state_best;
    }
  }
  return best[0][tiles->num_states - 1];
}

// Writes the natural tiles of the best arrangement found by natural_dp onto
// their squares, the smallest letter that keeps the best key at each square in
// turn, and adds their score and adjustment to arrangement.
static void
natural_reconstruct(const WordsmogSquare *squares, const int *square_indices,
                    int num_natural, const WordsmogTiles *tiles,
                    int64_t best[RACK_SIZE + 1][WORDSMOG_MAX_STATES],
                    WordsmogArrangement *arrangement) {
  int state = tiles->num_states - 1;
  for (int pos = 0; pos < num_natural; pos++) {
    const WordsmogSquare *square = &squares[square_indices[pos]];
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      if (tiles->digits[state][letter_idx] == 0 ||
          !natural_allowed(square, tiles, letter_idx)) {
        continue;
      }
      const int next_state = state - tiles->strides[letter_idx];
      const int64_t rest = best[pos + 1][next_state];
      if (rest == WORDSMOG_NO_KEY ||
          natural_key(square, tiles, letter_idx) + rest != best[pos][state]) {
        continue;
      }
      const MachineLetter ml = tiles->letters[letter_idx];
      arrangement->tiles[square_indices[pos]] = ml;
      arrangement->score += tiles->scores[letter_idx] * square->coefficient;
      if (((tiles->vowels >> ml) & 1U) != 0) {
        arrangement->adjustment += square->vowel_adjustment;
      }
      state = next_state;
      break;
    }
  }
  assert(state == 0);
}

// A blank's designation on square from the candidates: the one with the best
// adjustment, then the lowest. Returns false when there are none.
static bool blank_designation(const WordsmogSquare *square,
                              const WordsmogTiles *tiles, uint64_t candidates,
                              MachineLetter *designation) {
  if (candidates == 0) {
    return false;
  }
  const uint64_t consonants = candidates & ~(uint64_t)tiles->vowels;
  if (square->vowel_adjustment < 0 && consonants != 0) {
    candidates = consonants;
  }
  *designation = (MachineLetter)wordsmog_lowest_bit(candidates);
  return true;
}

static inline Equity blank_adjustment(const WordsmogSquare *square,
                                      const WordsmogTiles *tiles,
                                      MachineLetter designation) {
  return ((tiles->vowels >> designation) & 1U) != 0 ? square->vowel_adjustment
                                                    : 0;
}

// The designations a blank can take: letters a WMP BitRack can hold.
static const uint64_t WORDSMOG_DESIGNATIONS =
    (((uint64_t)1 << BIT_RACK_MAX_ALPHABET_SIZE) - 1) & ~(uint64_t)1;

static inline bool pair_is_valid(const WordsmogTiles *tiles, MachineLetter x,
                                 MachineLetter y) {
  const MachineLetter low = x < y ? x : y;
  const MachineLetter high = x < y ? y : x;
  return ((tiles->pair_letters[low] >> high) & 1U) != 0;
}

// The designations of blanks on first and second (first before second in
// board order) that keep the main word valid: the pair with the best total
// adjustment, then the lowest first designation, then the lowest second.
// Returns false when there are none.
static bool blank_pair_designations(const WordsmogSquare *first,
                                    const WordsmogSquare *second,
                                    const WordsmogTiles *tiles,
                                    MachineLetter *first_designation,
                                    MachineLetter *second_designation) {
  bool found = false;
  Equity best_adjustment = 0;
  for (uint64_t xs = first->cross_set & WORDSMOG_DESIGNATIONS; xs != 0;
       xs &= xs - 1) {
    const MachineLetter x = (MachineLetter)wordsmog_lowest_bit(xs);
    for (uint64_t ys = second->cross_set & WORDSMOG_DESIGNATIONS; ys != 0;
         ys &= ys - 1) {
      const MachineLetter y = (MachineLetter)wordsmog_lowest_bit(ys);
      if (!pair_is_valid(tiles, x, y)) {
        continue;
      }
      const Equity adjustment = blank_adjustment(first, tiles, x) +
                                blank_adjustment(second, tiles, y);
      if (!found || adjustment > best_adjustment) {
        found = true;
        best_adjustment = adjustment;
        *first_designation = x;
        *second_designation = y;
      }
    }
  }
  return found;
}

// Whether the candidate beats the best so far: a higher key, or the same key
// and smaller tiles in board order.
static bool arrangement_beats(int64_t key, const WordsmogArrangement *candidate,
                              int num_squares, bool have_best, int64_t best_key,
                              const WordsmogArrangement *best) {
  if (!have_best || key > best_key) {
    return true;
  }
  if (key < best_key) {
    return false;
  }
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    if (candidate->tiles[square_idx] != best->tiles[square_idx]) {
      return candidate->tiles[square_idx] < best->tiles[square_idx];
    }
  }
  return false;
}

// Narrows each square pinned to a natural letter to that letter, and sets the
// squares a blank may not take (pinned to a natural letter) and must take
// (pinned to a blank). Without pins, copies the squares.
static void apply_pins(const WordsmogSquare *squares, int num_squares,
                       const MachineLetter *pins, WordsmogSquare *pinned,
                       uint32_t *blank_forbidden, uint32_t *blank_required) {
  *blank_forbidden = 0;
  *blank_required = 0;
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    pinned[square_idx] = squares[square_idx];
    if (pins == NULL || pins[square_idx] == WORDSMOG_PIN_FREE) {
      continue;
    }
    if (pins[square_idx] == WORDSMOG_PIN_BLANK) {
      *blank_required |= 1U << square_idx;
    } else {
      pinned[square_idx].cross_set &= (uint64_t)1 << pins[square_idx];
      *blank_forbidden |= 1U << square_idx;
    }
  }
}

// Each choice of the blanks' squares, as a bitset of squares, that the pins
// allow: none, each square, or each pair of squares. Returns how many.
static int blank_square_choices(int num_squares, int num_blanks,
                                uint32_t blank_forbidden,
                                uint32_t blank_required,
                                uint32_t choices[WORDSMOG_MAX_BLANK_CHOICES]) {
  int num_choices = 0;
  if (num_blanks == 0) {
    if (blank_required == 0) {
      choices[num_choices++] = 0;
    }
    return num_choices;
  }
  for (int first = 0; first < num_squares; first++) {
    for (int second = first + 1; second < (num_blanks == 2 ? num_squares : 1);
         second++) {
      const uint32_t blanks = (1U << first) | (1U << second);
      if ((blanks & blank_forbidden) == 0 && (blank_required & ~blanks) == 0) {
        choices[num_choices++] = blanks;
      }
    }
    if (num_blanks == 1) {
      const uint32_t blanks = 1U << first;
      if ((blanks & blank_forbidden) == 0 && (blank_required & ~blanks) == 0) {
        choices[num_choices++] = blanks;
      }
    }
  }
  return num_choices;
}

// The squares not in blanks, in board order. Returns how many.
static int natural_squares_of(int num_squares, uint32_t blanks,
                              int natural_squares[RACK_SIZE]) {
  int num_natural = 0;
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    if ((blanks & (1U << square_idx)) == 0) {
      natural_squares[num_natural++] = square_idx;
    }
  }
  return num_natural;
}

// The blank on the square with the lower index of blanks, and the other blank
// if there are two.
static void blank_squares_of(uint32_t blanks, int *first, int *second) {
  *first = wordsmog_lowest_bit(blanks);
  const uint32_t rest = blanks & (blanks - 1);
  *second = rest != 0 ? wordsmog_lowest_bit(rest) : -1;
}

// Removes one copy of value from scores, highest first, keeping the order.
static void remove_score(Equity *scores, int num_scores, Equity value) {
  int score_idx = 0;
  while (scores[score_idx] != value) {
    score_idx++;
  }
  for (; score_idx < num_scores - 1; score_idx++) {
    scores[score_idx] = scores[score_idx + 1];
  }
}

// The best arrangement without the DP, when the squares have no vowel
// adjustments and there are no pins and at most one blank. Then the best
// arrangements are those that reach wordsmog_score_bound, if any does: the
// comonotone ones, in which no square holds a lower score than a square with a
// lower coefficient. So each square in board order takes a tile whose score
// lies between the remaining tiles' scores at the ranks of the remaining
// squares that share its coefficient: the smallest letter its cross set allows,
// or else the blank. When that never gets stuck, the arrangement is comonotone,
// and it is the smallest in board order, since at each square every best
// arrangement with the same tiles before it puts one of the tiles considered
// there. Returns false when it gets stuck, leaving the DP to decide.
static bool greedy_arrangement(const WordsmogSquare *squares, int num_squares,
                               const WordsmogTiles *tiles,
                               WordsmogArrangement *best) {
  Equity remaining_scores[RACK_SIZE];
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    remaining_scores[square_idx] = tiles->descending_scores[square_idx];
  }
  int counts[RACK_SIZE];
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    counts[letter_idx] = tiles->counts[letter_idx];
  }
  int num_blanks = tiles->num_blanks;
  best->score = 0;
  best->adjustment = 0;
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    const WordsmogSquare *square = &squares[square_idx];
    int num_above = 0;
    int num_level = 0;
    for (int later_idx = square_idx; later_idx < num_squares; later_idx++) {
      num_above += squares[later_idx].coefficient > square->coefficient;
      num_level += squares[later_idx].coefficient == square->coefficient;
    }
    const Equity highest = remaining_scores[num_above];
    const Equity lowest = remaining_scores[num_above + num_level - 1];
    Equity score = 0;
    bool placed = false;
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      score = tiles->scores[letter_idx];
      if (counts[letter_idx] == 0 || score > highest || score < lowest ||
          !natural_allowed(square, tiles, letter_idx)) {
        continue;
      }
      counts[letter_idx]--;
      best->tiles[square_idx] = tiles->letters[letter_idx];
      placed = true;
      break;
    }
    if (!placed) {
      score = tiles->blank_score;
      MachineLetter designation;
      if (num_blanks == 0 || score > highest || score < lowest ||
          !blank_designation(square, tiles,
                             square->cross_set & tiles->blank_letters &
                                 ~(uint64_t)1,
                             &designation)) {
        return false;
      }
      num_blanks--;
      best->tiles[square_idx] = get_blanked_machine_letter(designation);
    }
    best->score += score * square->coefficient;
    remove_score(remaining_scores, num_squares - square_idx, score);
  }
  return true;
}

// Whether no square has a vowel adjustment, which only the opening move has.
static bool no_vowel_adjustments(const WordsmogSquare *squares,
                                 int num_squares) {
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    if (squares[square_idx].vowel_adjustment != 0) {
      return false;
    }
  }
  return true;
}

// Sets square_tiles[square] to the tiles, numbered with each letter's copies
// in order and then the blank, that the square's cross set allows: a blank
// when some designation keeps every word valid. For at most one blank.
// Returns false when a square allows none of them.
static bool allowed_tiles(const WordsmogSquare *squares, int num_squares,
                          const WordsmogTiles *tiles,
                          uint32_t square_tiles[RACK_SIZE]) {
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    const WordsmogSquare *square = &squares[square_idx];
    uint32_t allowed = 0;
    int tile_idx = 0;
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      const uint32_t copies = (1U << tiles->counts[letter_idx]) - 1;
      if (natural_allowed(square, tiles, letter_idx)) {
        allowed |= copies << tile_idx;
      }
      tile_idx += tiles->counts[letter_idx];
    }
    if (tiles->num_blanks == 1 &&
        (square->cross_set & tiles->blank_letters & ~(uint64_t)1) != 0) {
      allowed |= 1U << tile_idx;
    }
    if (allowed == 0) {
      return false;
    }
    square_tiles[square_idx] = allowed;
  }
  return true;
}

// Finds square_idx a tile among those not yet visited, moving the squares
// already matched to other tiles as needed: one augmenting path.
static bool match_square(const uint32_t square_tiles[RACK_SIZE], int square_idx,
                         uint32_t *visited, int tile_squares[RACK_SIZE]) {
  for (uint32_t candidates = square_tiles[square_idx] & ~*visited;
       candidates != 0; candidates &= candidates - 1) {
    const int tile_idx = wordsmog_lowest_bit(candidates);
    if ((*visited >> tile_idx) & 1U) {
      continue;
    }
    *visited |= 1U << tile_idx;
    if (tile_squares[tile_idx] < 0 ||
        match_square(square_tiles, tile_squares[tile_idx], visited,
                     tile_squares)) {
      tile_squares[tile_idx] = square_idx;
      return true;
    }
  }
  return false;
}

// Whether each square can take a different tile it allows, which is whether
// any arrangement is legal.
static bool tiles_can_match(const uint32_t square_tiles[RACK_SIZE],
                            int num_squares) {
  int tile_squares[RACK_SIZE];
  for (int tile_idx = 0; tile_idx < num_squares; tile_idx++) {
    tile_squares[tile_idx] = -1;
  }
  for (int square_idx = 0; square_idx < num_squares; square_idx++) {
    uint32_t visited = 0;
    if (!match_square(square_tiles, square_idx, &visited, tile_squares)) {
      return false;
    }
  }
  return true;
}

bool wordsmog_best_arrangement(const WordsmogSquare *squares, int num_squares,
                               WordsmogTiles *tiles, const MachineLetter *pins,
                               WordsmogArrangement *best) {
  assert(num_squares <= RACK_SIZE);
  assert(tiles->num_blanks >= 0 && tiles->num_blanks <= 2);
  // Without pins and with at most one blank, most placements are settled
  // without the DP: many allow no arrangement, and in many the greedy
  // arrangement reaches the score bound.
  if (pins == NULL && tiles->num_blanks <= 1) {
    uint32_t square_tiles[RACK_SIZE];
    if (!allowed_tiles(squares, num_squares, tiles, square_tiles)) {
      return false;
    }
    if (no_vowel_adjustments(squares, num_squares) &&
        greedy_arrangement(squares, num_squares, tiles, best)) {
      return true;
    }
    if (!tiles_can_match(square_tiles, num_squares)) {
      return false;
    }
  }
  WordsmogSquare pinned[RACK_SIZE];
  uint32_t blank_forbidden;
  uint32_t blank_required;
  apply_pins(squares, num_squares, pins, pinned, &blank_forbidden,
             &blank_required);
  if (tiles->num_states == 0) {
    wordsmog_tiles_build_states(tiles);
  }
  uint32_t choices[WORDSMOG_MAX_BLANK_CHOICES];
  const int num_choices = blank_square_choices(
      num_squares, tiles->num_blanks, blank_forbidden, blank_required, choices);
  int64_t dp[RACK_SIZE + 1][WORDSMOG_MAX_STATES];
  bool have_best = false;
  int64_t best_key = WORDSMOG_NO_KEY;
  for (int choice_idx = 0; choice_idx < num_choices; choice_idx++) {
    const uint32_t blanks = choices[choice_idx];
    WordsmogArrangement candidate = {.score = 0, .adjustment = 0};
    int64_t blank_key = 0;
    if (tiles->num_blanks > 0) {
      int first;
      int second;
      blank_squares_of(blanks, &first, &second);
      MachineLetter designations[2];
      if (tiles->num_blanks == 1) {
        if (!blank_designation(&pinned[first], tiles,
                               pinned[first].cross_set & tiles->blank_letters &
                                   ~(uint64_t)1,
                               &designations[0])) {
          continue;
        }
      } else if (!blank_pair_designations(&pinned[first], &pinned[second],
                                          tiles, &designations[0],
                                          &designations[1])) {
        continue;
      }
      const int blank_squares[2] = {first, second};
      for (int blank_idx = 0; blank_idx < tiles->num_blanks; blank_idx++) {
        const WordsmogSquare *square = &pinned[blank_squares[blank_idx]];
        const Equity score = tiles->blank_score * square->coefficient;
        const Equity adjustment =
            blank_adjustment(square, tiles, designations[blank_idx]);
        candidate.tiles[blank_squares[blank_idx]] =
            get_blanked_machine_letter(designations[blank_idx]);
        candidate.score += score;
        candidate.adjustment += adjustment;
        blank_key += wordsmog_key(score, adjustment);
      }
    }
    int natural_squares[RACK_SIZE];
    const int num_natural =
        natural_squares_of(num_squares, blanks, natural_squares);
    const int64_t natural_best =
        natural_dp(pinned, natural_squares, num_natural, tiles, dp);
    if (natural_best == WORDSMOG_NO_KEY) {
      continue;
    }
    natural_reconstruct(pinned, natural_squares, num_natural, tiles, dp,
                        &candidate);
    const int64_t key = blank_key + natural_best;
    if (arrangement_beats(key, &candidate, num_squares, have_best, best_key,
                          best)) {
      *best = candidate;
      best_key = key;
      have_best = true;
    }
  }
  return have_best;
}

// The number of ways to place the tiles of state on the natural squares
// square_indices[pos..num_natural-1], in counts[pos][state], for each state
// with as many tiles as those squares. Returns the number for every natural
// tile.
static uint64_t
natural_count(const WordsmogSquare *squares, const int *square_indices,
              int num_natural, const WordsmogTiles *tiles,
              uint64_t counts[RACK_SIZE + 1][WORDSMOG_MAX_STATES]) {
  counts[num_natural][0] = 1;
  for (int pos = num_natural - 1; pos >= 0; pos--) {
    const WordsmogSquare *square = &squares[square_indices[pos]];
    const int total = num_natural - pos;
    for (int state_idx = tiles->state_starts[total];
         state_idx < tiles->state_starts[total + 1]; state_idx++) {
      const int state = tiles->states_by_total[state_idx];
      uint64_t state_count = 0;
      for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
        if (tiles->digits[state][letter_idx] != 0 &&
            natural_allowed(square, tiles, letter_idx)) {
          state_count += counts[pos + 1][state - tiles->strides[letter_idx]];
        }
      }
      counts[pos][state] = state_count;
    }
  }
  return counts[0][tiles->num_states - 1];
}

// The number of designations of blanks on the squares of blanks that keep
// every word valid.
static uint64_t blank_designation_count(const WordsmogSquare *squares,
                                        const WordsmogTiles *tiles,
                                        uint32_t blanks) {
  int first;
  int second;
  blank_squares_of(blanks, &first, &second);
  const uint64_t first_letters =
      squares[first].cross_set & WORDSMOG_DESIGNATIONS;
  if (tiles->num_blanks == 1) {
    return (uint64_t)wordsmog_bit_count(first_letters & tiles->blank_letters);
  }
  uint64_t count = 0;
  for (uint64_t xs = first_letters; xs != 0; xs &= xs - 1) {
    const MachineLetter x = (MachineLetter)wordsmog_lowest_bit(xs);
    for (uint64_t ys = squares[second].cross_set & WORDSMOG_DESIGNATIONS;
         ys != 0; ys &= ys - 1) {
      if (pair_is_valid(tiles, x, (MachineLetter)wordsmog_lowest_bit(ys))) {
        count++;
      }
    }
  }
  return count;
}

void wordsmog_count_arrangements(const WordsmogSquare *squares, int num_squares,
                                 WordsmogTiles *tiles,
                                 const MachineLetter *pins,
                                 uint64_t *arrangements, uint64_t *plays) {
  WordsmogSquare pinned[RACK_SIZE];
  uint32_t blank_forbidden;
  uint32_t blank_required;
  apply_pins(squares, num_squares, pins, pinned, &blank_forbidden,
             &blank_required);
  if (tiles->num_states == 0) {
    wordsmog_tiles_build_states(tiles);
  }
  uint32_t choices[WORDSMOG_MAX_BLANK_CHOICES];
  const int num_choices = blank_square_choices(
      num_squares, tiles->num_blanks, blank_forbidden, blank_required, choices);
  uint64_t counts[RACK_SIZE + 1][WORDSMOG_MAX_STATES];
  *arrangements = 0;
  *plays = 0;
  for (int choice_idx = 0; choice_idx < num_choices; choice_idx++) {
    const uint32_t blanks = choices[choice_idx];
    const uint64_t designations =
        tiles->num_blanks > 0 ? blank_designation_count(pinned, tiles, blanks)
                              : 1;
    if (designations == 0) {
      continue;
    }
    int natural_squares[RACK_SIZE];
    const int num_natural =
        natural_squares_of(num_squares, blanks, natural_squares);
    const uint64_t natural =
        natural_count(pinned, natural_squares, num_natural, tiles, counts);
    *arrangements += natural;
    *plays += natural * designations;
  }
}
