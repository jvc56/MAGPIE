#ifndef NERFED_PLAYER_H
#define NERFED_PLAYER_H

#include "../ent/dictionary_word.h"
#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
#include "../ent/sim_results.h"
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

// Sim-based selection ("noisy simming"): the exchange model and the
// visibility gate run first; the surviving visible plays become the arms
// of a BAI sim whose utilities the pick below perturbs with the fitted
// valuation noise. Generates all moves, rolls the exchange decision, and
// gates visibility. Returns the selected move directly when no sim is
// needed (exchange chosen, or zero or one visible play); otherwise fills
// arms with the visible plays (best static equity first, up to the arms
// list's capacity) and returns NULL.
const Move *nerfed_player_prepare_sim_arms(NerfedPlayer *nerfed_player,
                                           Game *game, MoveList *arms,
                                           XoshiroPRNG *prng);

// Picks over ALL visible survivors from the prepare call: simmed arms
// value as static equity plus a partial glimpse of the sim's
// disagreement (centered on arm 0), non-simmed survivors keep pure
// static values, and every survivor gets the challenge-risk discount and
// the fitted Gumbel valuation noise. The utility weights must match the
// weights the sim ran with. The returned move points into the nerfed
// player's internal move list and is valid until the next prepare call.
const Move *nerfed_player_pick_simmed_move(NerfedPlayer *nerfed_player,
                                           const SimResults *sim_results,
                                           double utility_w_winpct,
                                           double utility_w_spread,
                                           double utility_spread_scale,
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

// The player's believed lexicon (their game KWG, e.g. CSW24PH1400).
// When set, belief in words absent from the real lexicon (phonies) is
// decided by membership here: the mover trusts their own lexicon, and a
// challenger never suspects a phony they themselves believe. Real-word
// belief keeps the knowledge-model draw.
void nerfed_player_set_believed_kwg(NerfedPlayer *nerfed_player,
                                    const KWG *kwg);

// Game-level knowledge lifecycle: call at game start (per-player seed)
// and per turn (enables the small knowledge-flip probability).
void nerfed_player_start_game(NerfedPlayer *nerfed_player, uint64_t game_seed);
void nerfed_player_set_turn(NerfedPlayer *nerfed_player, int turn_number);
bool nerfed_player_believes_word(const NerfedPlayer *nerfed_player,
                                 const MachineLetter *word, int word_length);
// Subjective P(word is valid), capped away from 0/1 except for
// saturated-playability (rating-scaled) or saturated-literacy words.
double nerfed_player_word_confidence(const NerfedPlayer *nerfed_player,
                                     const MachineLetter *word,
                                     int word_length);

// Challenge decision against an opponent's selected move under the given
// challenge rule (true = 5-point per word, false = double challenge).
// opponent (the mover) sets the phony prior: strong opponents' obscure
// words are credible; NULL means an unnerfed (full-strength) opponent.
bool nerfed_player_challenge_decision(const NerfedPlayer *nerfed_player,
                                      const NerfedPlayer *opponent, Game *game,
                                      const Move *move, bool rule_5pt,
                                      XoshiroPRNG *prng);

// Selects among the solve's top-K PVs by value plus Gumbel noise of the
// rating-fitted endgame sigma, swaps the choice into multi-PV slot 0, and
// forces it as the best PV. Deterministic per (seed, pv index).
void nerfed_player_pick_endgame_pv(const NerfedPlayer *nerfed_player,
                                   const Game *game,
                                   EndgameResults *endgame_results,
                                   uint64_t seed);

#endif
