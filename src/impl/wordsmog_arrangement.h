#ifndef WORDSMOG_ARRANGEMENT_H
#define WORDSMOG_ARRANGEMENT_H

#include "../def/bit_rack_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/wordsmog_defs.h"
#include "../ent/equity.h"
#include <stdbool.h>
#include <stdint.h>

// In wordsmog, every arrangement of a valid tile multiset on a placement's
// empty squares forms a valid main word, so a play is chosen by arranging the
// tiles: each square takes a letter its cross set allows, and a blank takes a
// designation that also keeps the main word valid. A tile's score counts
// once per word it is in, so each square has a coefficient, its letter
// multiplier times the main word's multiplier plus its own word multiplier
// when it has a cross word, and the tiles add sum(score * coefficient).

// One empty square of a placement, in board order.
typedef struct WordsmogSquare {
  // The letters allowed here (bit ml), of the alphabet and without the blank
  // bit.
  uint64_t cross_set;
  int coefficient;
  // Added to equity when the tile here is a vowel: the opening move's
  // placement penalty (<= 0), or 0.
  Equity vowel_adjustment;
} WordsmogSquare;

// The tiles to arrange: distinct natural letters in ascending order, with
// their counts, and up to two blanks.
typedef struct WordsmogTiles {
  int num_letters;
  MachineLetter letters[RACK_SIZE];
  int counts[RACK_SIZE];
  Equity scores[RACK_SIZE];
  int num_blanks;
  Equity blank_score;
  // Bit ml is set for vowels, for both natural tiles and blank designations.
  uint32_t vowels;
  // With one blank, the designations that keep the main word valid.
  uint32_t blank_letters;
  // With two blanks, pair_letters[x] holds each y >= x for which the pair
  // {x, y} keeps the main word valid.
  uint32_t pair_letters[BIT_RACK_MAX_ALPHABET_SIZE];
  // Set from the letters and counts by wordsmog_tiles_set_states: each
  // multiset of the natural tiles, a state, in mixed radix with one digit per
  // letter, and its count of each letter. The states with t tiles are
  // states_by_total[state_starts[t] .. state_starts[t + 1] - 1].
  int num_states;
  int strides[RACK_SIZE];
  uint8_t digits[WORDSMOG_MAX_STATES][RACK_SIZE];
  uint8_t states_by_total[WORDSMOG_MAX_STATES];
  uint8_t state_starts[RACK_SIZE + 2];
  // Every tile's score, blanks included, highest first.
  Equity descending_scores[RACK_SIZE];
} WordsmogTiles;

// Sets the tiles' states, which every function below reads, once their letters
// and counts are set.
void wordsmog_tiles_set_states(WordsmogTiles *tiles);

typedef struct WordsmogArrangement {
  // The tile on each square, a blank as its blanked designation.
  MachineLetter tiles[RACK_SIZE];
  // sum(score * coefficient) over the squares.
  Equity score;
  // The sum of the vowel adjustments.
  Equity adjustment;
} WordsmogArrangement;

// An upper bound on the score of every arrangement of tiles on the squares:
// the tiles' scores, highest first, times the coefficients, highest first.
Equity wordsmog_score_bound(const WordsmogSquare *squares, int num_squares,
                            const WordsmogTiles *tiles);

// pins, when not NULL, holds a WORDSMOG_PIN_* value or a natural letter for
// each square (see wordsmog_defs.h): the arrangements considered are those
// that put that letter, or a blank, on each pinned square.

// Finds the arrangement of tiles on the num_squares squares (as many as there
// are tiles) with the highest score + adjustment, then the highest score, then
// the smallest tiles in board order, a natural tile ranking below any blank,
// as compare_moves ranks plays. A blank takes the designation with the best
// adjustment, then the lowest. Returns false when no arrangement is legal.
bool wordsmog_best_arrangement(const WordsmogSquare *squares, int num_squares,
                               const WordsmogTiles *tiles,
                               const MachineLetter *pins,
                               WordsmogArrangement *best);

// Counts the legal arrangements: in arrangements with each blank's designation
// left open, and in plays with every designation that keeps every word valid.
void wordsmog_count_arrangements(const WordsmogSquare *squares, int num_squares,
                                 const WordsmogTiles *tiles,
                                 const MachineLetter *pins,
                                 uint64_t *arrangements, uint64_t *plays);

#endif
