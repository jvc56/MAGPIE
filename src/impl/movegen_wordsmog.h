#ifndef MOVEGEN_WORDSMOG_H
#define MOVEGEN_WORDSMOG_H

#include "../def/rack_defs.h"
#include "../ent/anchor.h"
#include "../ent/bit_rack.h"
#include "../ent/equity.h"
#include "../ent/wmp.h"
#include "move_gen_base.h"
#include "wordsmog_arrangement.h"
#include <stdbool.h>

// Wordsmog generation with the WMP. A word is valid when its letters are an
// anagram of a word of the lexicon, so a subrack whose letters and the
// playthrough letters are one forms a valid main word in every arrangement.
// Each of an anchor's start columns gets one play per such subrack: its best
// arrangement on the placement's empty squares (see wordsmog_arrangement.h),
// or with the PAT term on, the arrangement and blank designations that are
// best with it.

// Records the wordsmog plays from the anchor, as wordmap_gen records the
// classic ones. The word info table is off in wordsmog, and the RIT's bingo
// words are spellings, so their prunes and fast path are skipped.
void wordsmog_wmp_gen(MoveGen *gen, const Anchor *anchor, bool lazy);

// The PAT term depends on which tile goes on which square, so with it on the
// generator keeps what it needs to search arrangements in gen->wordsmog_pat.
// wordsmog_pat_refine prices more of the plays it kept, once the anchors have
// been recorded; it does nothing when no placement was kept.
void wordsmog_pat_refine(MoveGen *gen);
// Forgets the plays kept, at the start of a generation.
void wordsmog_pat_refine_reset(WordsmogPATRefine *refine);
// Frees the search's state; refine may be NULL.
void wordsmog_pat_refine_destroy(WordsmogPATRefine *refine);

// The tiles of a subrack for wordsmog_best_arrangement: its natural letters,
// its blanks, and the designations that keep main_word, the subrack plus the
// playthrough letters, an anagram of a word. entry is main_word's WMP entry.
void wordsmog_tiles_init(const MoveGen *gen, const BitRack *subrack,
                         const BitRack *main_word, const WMPEntry *entry,
                         WordsmogTiles *tiles);

// The empty squares of the current anchor's placement at start_col, in board
// order, for wordsmog_best_arrangement, and their positions in the word in
// square_letter_idxs. Marks the board's tiles in gen->playthrough_marked, and
// writes the part of every play's score there that does not depend on its
// tiles to base_score. Returns how many squares there are.
int wordsmog_placement_squares(MoveGen *gen, int start_col,
                               WordsmogSquare squares[RACK_SIZE],
                               int square_letter_idxs[RACK_SIZE],
                               Equity *base_score);

#endif
