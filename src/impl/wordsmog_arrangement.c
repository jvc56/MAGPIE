#include "wordsmog_arrangement.h"

#include "../def/bit_rack_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/equity.h"
#include "../ent/letter_distribution.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

enum {
  // A natural-tile state counts the tiles of each letter still to place, in
  // mixed radix: at most 2^RACK_SIZE states, when every tile differs.
  WORDSMOG_MAX_STATES = 1 << RACK_SIZE,
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

// The natural tiles' states: each state's count of each letter, and its
// total.
typedef struct NaturalStates {
  int num_states;
  int strides[RACK_SIZE];
  uint8_t digits[WORDSMOG_MAX_STATES][RACK_SIZE];
  uint8_t totals[WORDSMOG_MAX_STATES];
} NaturalStates;

static void natural_states_init(const WordsmogTiles *tiles,
                                NaturalStates *states) {
  int num_states = 1;
  for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
    states->strides[letter_idx] = num_states;
    num_states *= tiles->counts[letter_idx] + 1;
  }
  assert(num_states <= WORDSMOG_MAX_STATES);
  states->num_states = num_states;
  for (int state = 0; state < num_states; state++) {
    int total = 0;
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      const int digit = (state / states->strides[letter_idx]) %
                        (tiles->counts[letter_idx] + 1);
      states->digits[state][letter_idx] = (uint8_t)digit;
      total += digit;
    }
    states->totals[state] = (uint8_t)total;
  }
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
                          const NaturalStates *states,
                          int64_t best[RACK_SIZE + 1][WORDSMOG_MAX_STATES]) {
  best[num_natural][0] = 0;
  for (int pos = num_natural - 1; pos >= 0; pos--) {
    const WordsmogSquare *square = &squares[square_indices[pos]];
    for (int state = 0; state < states->num_states; state++) {
      if (states->totals[state] != num_natural - pos) {
        continue;
      }
      int64_t state_best = WORDSMOG_NO_KEY;
      for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
        if (states->digits[state][letter_idx] == 0 ||
            !natural_allowed(square, tiles, letter_idx)) {
          continue;
        }
        const int64_t rest = best[pos + 1][state - states->strides[letter_idx]];
        if (rest == WORDSMOG_NO_KEY) {
          continue;
        }
        const int64_t key = natural_key(square, tiles, letter_idx) + rest;
        if (key > state_best) {
          state_best = key;
        }
      }
      best[pos][state] = state_best;
    }
  }
  return best[0][states->num_states - 1];
}

// Writes the natural tiles of the best arrangement found by natural_dp onto
// their squares, the smallest letter that keeps the best key at each square in
// turn, and adds their score and adjustment to arrangement.
static void
natural_reconstruct(const WordsmogSquare *squares, const int *square_indices,
                    int num_natural, const WordsmogTiles *tiles,
                    const NaturalStates *states,
                    int64_t best[RACK_SIZE + 1][WORDSMOG_MAX_STATES],
                    WordsmogArrangement *arrangement) {
  int state = states->num_states - 1;
  for (int pos = 0; pos < num_natural; pos++) {
    const WordsmogSquare *square = &squares[square_indices[pos]];
    for (int letter_idx = 0; letter_idx < tiles->num_letters; letter_idx++) {
      if (states->digits[state][letter_idx] == 0 ||
          !natural_allowed(square, tiles, letter_idx)) {
        continue;
      }
      const int next_state = state - states->strides[letter_idx];
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

bool wordsmog_best_arrangement(const WordsmogSquare *squares, int num_squares,
                               const WordsmogTiles *tiles,
                               WordsmogArrangement *best) {
  assert(num_squares <= RACK_SIZE);
  assert(tiles->num_blanks >= 0 && tiles->num_blanks <= 2);
  NaturalStates states;
  natural_states_init(tiles, &states);
  assert(states.totals[states.num_states - 1] ==
         num_squares - tiles->num_blanks);
  int64_t dp[RACK_SIZE + 1][WORDSMOG_MAX_STATES];
  bool have_best = false;
  int64_t best_key = WORDSMOG_NO_KEY;
  // Each choice of the blanks' squares, in board order: none, each square,
  // or each pair of squares.
  const int num_blanks = tiles->num_blanks;
  for (int first = 0; first < (num_blanks >= 1 ? num_squares : 1); first++) {
    for (int second = first + 1;
         second < (num_blanks == 2 ? num_squares : first + 2); second++) {
      WordsmogArrangement candidate = {.score = 0, .adjustment = 0};
      int64_t blank_key = 0;
      if (num_blanks == 1) {
        MachineLetter designation;
        if (!blank_designation(&squares[first], tiles,
                               squares[first].cross_set & tiles->blank_letters &
                                   ~(uint64_t)1,
                               &designation)) {
          continue;
        }
        const Equity score = tiles->blank_score * squares[first].coefficient;
        const Equity adjustment =
            blank_adjustment(&squares[first], tiles, designation);
        candidate.tiles[first] = get_blanked_machine_letter(designation);
        candidate.score += score;
        candidate.adjustment += adjustment;
        blank_key = wordsmog_key(score, adjustment);
      } else if (num_blanks == 2) {
        MachineLetter first_designation;
        MachineLetter second_designation;
        if (!blank_pair_designations(&squares[first], &squares[second], tiles,
                                     &first_designation, &second_designation)) {
          continue;
        }
        const int blank_squares[2] = {first, second};
        const MachineLetter designations[2] = {first_designation,
                                               second_designation};
        for (int blank_idx = 0; blank_idx < 2; blank_idx++) {
          const WordsmogSquare *square = &squares[blank_squares[blank_idx]];
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
      int num_natural = 0;
      for (int square_idx = 0; square_idx < num_squares; square_idx++) {
        const bool is_blank_square = (num_blanks >= 1 && square_idx == first) ||
                                     (num_blanks == 2 && square_idx == second);
        if (!is_blank_square) {
          natural_squares[num_natural++] = square_idx;
        }
      }
      const int64_t natural_best =
          natural_dp(squares, natural_squares, num_natural, tiles, &states, dp);
      if (natural_best == WORDSMOG_NO_KEY) {
        continue;
      }
      natural_reconstruct(squares, natural_squares, num_natural, tiles, &states,
                          dp, &candidate);
      const int64_t key = blank_key + natural_best;
      if (arrangement_beats(key, &candidate, num_squares, have_best, best_key,
                            best)) {
        *best = candidate;
        best_key = key;
        have_best = true;
      }
    }
  }
  return have_best;
}
