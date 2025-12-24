#ifndef VALIDATED_MOVES_STRING_H
#define VALIDATED_MOVES_STRING_H

#include "../ent/board.h"
#include "../ent/letter_distribution.h"
#include "../ent/validated_move.h"

char *validated_move_get_phonies_formed(const LetterDistribution *ld,
                                        const ValidatedMoves *vms,
                                        int vm_index);

void string_builder_add_validated_moves_phonies(StringBuilder *sb,
                                                const ValidatedMoves *vms,
                                                const LetterDistribution *ld,
                                                const Board *board);

#endif
