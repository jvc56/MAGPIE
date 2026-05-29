#ifndef BAG_H
#define BAG_H

#include "letter_distribution.h"
#include "xoshiro.h"
#include <stdbool.h>
#include <stdint.h>

enum { MAX_BAG_SIZE = 1000 };

typedef struct Bag Bag;

Bag *bag_create(const LetterDistribution *ld, uint64_t seed);
void bag_destroy(Bag *bag);
void bag_copy(Bag *dst, const Bag *src);
Bag *bag_duplicate(const Bag *bag);

int bag_get_letters(const Bag *bag);
int bag_get_letter(const Bag *bag, MachineLetter ml);
bool bag_is_empty(const Bag *bag);

// Copies the bag's ordered tile content into `out`, returning the number of
// tiles. `out` must be at least bag_get_letters(bag) in length. Order matches
// the bag's internal layout: out[0] is the tile player 1 (start side) would
// draw next; out[bag_get_letters(bag)-1] is the tile player 0 (end side)
// would draw next.
int bag_peek_tiles(const Bag *bag, MachineLetter *out);

// Cursor accessors for incremental play/unplay. The cursor is the pair of
// (start_tile_index, end_tile_index); together they define the live window of
// bag->letters. Saving and restoring the cursor undoes any sequence of draws
// that did not mutate bag->letters (i.e. bag_draw_random_letter).
int bag_get_start_cursor(const Bag *bag);
int bag_get_end_cursor(const Bag *bag);
void bag_set_cursors(Bag *bag, int start, int end);

// Deterministically set the bag to exactly tiles[0..n-1] in order (no PRNG).
// For exact (pre)endgame enumerators that know the bag contents and must not
// randomize them. n must not exceed the bag's backing capacity.
void bag_set_to_tiles(Bag *bag, const MachineLetter *tiles, int n);

void bag_add_letter(Bag *bag, MachineLetter letter, int player_draw_index);
bool bag_draw_letter(Bag *bag, MachineLetter letter, int player_draw_index);

MachineLetter bag_draw_random_letter(Bag *bag, int player_draw_index);
void bag_seed(Bag *bag, uint64_t seed);
void bag_reset(const LetterDistribution *ld, Bag *bag);
void bag_shuffle(Bag *bag);
void bag_increment_unseen_count(const Bag *bag, int *unseen_count);

#endif