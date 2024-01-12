#ifndef VALIDATED_MOVE_H
#define VALIDATED_MOVE_H

#include "../def/game_history_defs.h"

#include "game.h"

typedef struct ValidatedMoves ValidatedMoves;

ValidatedMoves *validated_moves_create(const Game *game, int player_index,
                                       const char *moves, bool allow_phonies);
ValidatedMoves *validated_moves_create_empty();
void validated_moves_destroy(ValidatedMoves *vms);

int validated_moves_get_moves(ValidatedMoves *vms);
move_validation_status_t
validated_moves_get_validation_status(ValidatedMoves *vms);

bool validated_moves_combine(ValidatedMoves *vms1, ValidatedMoves *vms2);

#endif