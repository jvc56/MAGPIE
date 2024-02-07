#ifndef VALIDATED_MOVE_H
#define VALIDATED_MOVE_H

#include "../def/game_history_defs.h"
#include "../def/validated_move_defs.h"

#include "game.h"
#include "letter_distribution.h"
#include "move.h"
#include "words.h"

typedef struct ValidatedMoves ValidatedMoves;

ValidatedMoves *validated_moves_create(const Game *game, int player_index,
                                       const char *ucgi_moves_string,
                                       bool allow_phonies,
                                       bool allow_unknown_exchanges);
ValidatedMoves *validated_moves_create_empty();
void validated_moves_destroy(ValidatedMoves *vms);

int validated_moves_get_number_of_moves(const ValidatedMoves *vms);
const Move *validated_moves_get_move(const ValidatedMoves *vms, int i);
const FormedWords *validated_moves_get_formed_words(const ValidatedMoves *vms,
                                                    int i);
const Rack *validated_moves_get_rack(const ValidatedMoves *vms, int i);
const Rack *validated_moves_get_leave(const ValidatedMoves *vms, int i);
bool validated_moves_get_unknown_exchange(const ValidatedMoves *vms, int i);

move_validation_status_t
validated_moves_get_validation_status(const ValidatedMoves *vms);

char *validated_moves_get_phonies_string(const LetterDistribution *ld,
                                         ValidatedMoves *vms, int i);

int validated_moves_get_challenge_points(const ValidatedMoves *vms, int i);
bool validated_moves_get_challenge_turn_loss(const ValidatedMoves *vms, int i);

int static_eval_get_move_score(const LetterDistribution *ld, const Move *move,
                               Board *board, int cross_set_index);
void validated_moves_add_to_move_list(const ValidatedMoves *vms, MoveList *ml);

#endif