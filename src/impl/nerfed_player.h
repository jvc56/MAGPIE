#ifndef NERFED_PLAYER_H
#define NERFED_PLAYER_H

#include "../ent/dictionary_word.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"

// A NerfedPlayer models a human of a target rating. Each candidate play is
// independently visible with a probability fitted from the annotated-game
// corpus (word knowledge x geometry), and visible plays are valued as
// static equity plus Gumbel noise with a rating-dependent scale. The player
// chooses the best noisy-valued visible play.
typedef struct NerfedPlayer NerfedPlayer;

// Loads data/lexica/<lexicon>_wordfeats.csv for the game's player 0 lexicon.
NerfedPlayer *nerfed_player_create(const Game *game, int rating,
                                   ErrorStack *error_stack);
void nerfed_player_destroy(NerfedPlayer *nerfed_player);

// Generates all moves for the position and returns the nerfed selection.
// The returned move points into the nerfed player's internal move list and
// is valid until the next call.
const Move *nerfed_player_select_move(NerfedPlayer *nerfed_player, Game *game,
                                      XoshiroPRNG *prng);

// Deterministic word-knowledge draw: whether this player knows the word,
// decided by a hash of (seed, word) against the knowledge-miss probability.
// Deterministic per (seed, word) so an entire endgame search shares one
// coherent believed lexicon (transposition-table safe).
bool nerfed_player_knows_word(const NerfedPlayer *nerfed_player,
                              const MachineLetter *word, int word_length,
                              uint64_t seed);

// Copies the words of word_list that the player knows into filtered_list.
void nerfed_player_filter_word_list(const NerfedPlayer *nerfed_player,
                                    const DictionaryWordList *word_list,
                                    DictionaryWordList *filtered_list,
                                    uint64_t seed);

#endif
