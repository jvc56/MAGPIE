#ifndef WORD_PRUNE_H
#define WORD_PRUNE_H

#include "../ent/dictionary_word.h"
#include "../ent/game.h"
#include "../ent/kwg.h"

typedef struct BoardRow {
  MachineLetter letters[BOARD_DIM];
} BoardRow;

typedef struct BoardRows {
  BoardRow rows[BOARD_DIM * 2];
  int num_rows;
} BoardRows;

void generate_possible_words(const Game *game, const KWG *override_kwg,
                             DictionaryWordList *possible_word_list);

// Like generate_possible_words, but also drops playthrough words that could
// never be played because of cross-checks. For every square that already has
// a tile directly before or after it in the perpendicular lane, any word that
// will ever cover that square must, in that perpendicular lane, contain the
// existing tile: so the letters that can ever occupy the square are exactly
// the letters that the perpendicular lane's playthrough words put there. A
// first unconstrained pass records those letters per lane and square; a
// second pass regenerates the playthrough words, refusing a pool letter on
// such a square unless the perpendicular lane recorded it. Both passes draw
// from the union of every unseen tile, so the result is a subset of
// generate_possible_words and a superset of every word playable in any
// position reachable from this one.
//
// Only sound for classic play with one shared lexicon: Wordsmog cross words
// are order-free, and in informed dual-lexicon mode a cross word legal only
// in the other player's lexicon can place a letter this lexicon's pass never
// records. Callers must fall back to generate_possible_words otherwise.
void generate_possible_words_with_cross_checks(
    const Game *game, const KWG *override_kwg,
    DictionaryWordList *possible_word_list);

#endif // WORD_PRUNE_H