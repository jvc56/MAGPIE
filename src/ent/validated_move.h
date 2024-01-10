#ifndef VALIDATED_MOVE_H
#define VALIDATED_MOVE_H

#include "game.h"

typedef struct ValidatedMove ValidatedMove;

ValidatedMove *validated_move_create(const Game *game, int move_type, int row,
                                     int col, int dir, uint8_t *tiles,
                                     uint8_t *leave, int ntiles, int nleave);

void validated_move_destroy(ValidatedMove *vm);

typedef struct ValidatedMoves ValidatedMoves;

ValidatedMoves *validated_moves_create(const Game *game, const char *moves);
void validated_moves_destroy(ValidatedMoves *vms);

#endif