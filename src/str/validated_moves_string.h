#ifndef VALIDATED_MOVES_STRING_H
#define VALIDATED_MOVES_STRING_H

#include "../ent/letter_distribution.h"
#include "../ent/validated_move.h"

char *validated_moves_get_phonies_string(const LetterDistribution *ld,
                                         const ValidatedMoves *vms, int i);

#endif
