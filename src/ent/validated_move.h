#ifndef VALIDATED_MOVE_H
#define VALIDATED_MOVE_H

#include "../def/game_history_defs.h"
#include "../def/validated_move_defs.h"
#include "../util/io_util.h"
#include "game.h"
#include "letter_distribution.h"
#include "move.h"
#include "words.h"

typedef struct ValidatedMoves ValidatedMoves;

ValidatedMoves *validated_moves_create(const Game *game, int player_index,
                                       const char *moves_string,
                                       bool allow_phonies,
                                       bool allow_playthrough,
                                       ErrorStack *error_stack);
ValidatedMoves *validated_moves_duplicate(const ValidatedMoves *vms_orig);
void validated_moves_destroy(ValidatedMoves *vms);

int validated_moves_get_number_of_moves(const ValidatedMoves *vms);
const Move *validated_moves_get_move(const ValidatedMoves *vms, int i);
const FormedWords *validated_moves_get_formed_words(const ValidatedMoves *vms,
                                                    int i);

bool validated_moves_is_phony(const ValidatedMoves *vms, int vm_index);

void validated_moves_add_to_sorted_move_list(const ValidatedMoves *vms,
                                             MoveList *ml);
void validated_moves_set_rack_to_played_letters(const ValidatedMoves *vms,
                                                int i, Rack *rack_to_set);
#endif
