#ifndef MOVEGEN_WOADWAGE_H
#define MOVEGEN_WOADWAGE_H

#include "move_gen_base.h"

// Records the woadwage plays from the anchor loaded in gen, at
// gen->current_anchor_col of the current lane, as recursive_gen records the
// classic ones. Woadwage plays like the classic game, except that every word a
// play forms may also be two words of the lexicon run together. The search
// reads the GADDAG as classic generation does, and wherever a word of the
// lexicon can end it may also start the second word.
void woadwage_gen_anchor(MoveGen *gen);

#endif
