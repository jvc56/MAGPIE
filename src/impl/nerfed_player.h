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

// Challenge-economics context: the rule and opponent rating drive the
// expected value of being challenged (risk of losing the play vs the
// bait value of eliciting a wrong challenge). Set when the phony/
// challenge flow is active; without it the legacy flat risk discount
// applies.
void nerfed_player_set_challenge_context(NerfedPlayer *nerfed_player,
                                         bool rule_5pt,
                                         double opponent_rating_z);

// Challenge results are public knowledge: record the revealed verdict
// for a word (both players should receive the same reveals).
// verdict > 0: proven valid (failed challenge) — believed and never
// challenged again; verdict < 0: proven phony (single-word play came
// off) — never attempted or challenged again this game; verdict == 0:
// suspect (a multi-word play came off; which word was phony is
// ambiguous) — belief reduced, not eliminated.
void nerfed_player_reveal_word(NerfedPlayer *nerfed_player,
                               const MachineLetter *word, int word_length,
                               int verdict);

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

// Static challenge assessment: the Bayesian posterior that the play is
// invalid plus the static-utility expected values of challenging and
// accepting. attended=false means the play was never evaluated (the
// attention gate) and must not be challenged.
typedef struct NerfedChallengeAssessment {
  double p_invalid;
  double eu_challenge;
  double u_accept;
  double threshold;
  bool attended;
} NerfedChallengeAssessment;

// Fills the assessment for an opponent's selected move under the given
// challenge rule (true = 5-point per word, false = double challenge).
// opponent (the mover) sets the phony prior: strong opponents' obscure
// words are credible; NULL means an unnerfed (full-strength) opponent.
void nerfed_player_challenge_assess(const NerfedPlayer *nerfed_player,
                                    const NerfedPlayer *opponent, Game *game,
                                    const Move *move, bool rule_5pt,
                                    XoshiroPRNG *prng,
                                    NerfedChallengeAssessment *assessment);

// Static decision from an assessment (adds the decision noise).
bool nerfed_player_challenge_decide(const NerfedPlayer *nerfed_player,
                                    const NerfedChallengeAssessment *assessment,
                                    XoshiroPRNG *prng);

// True when the static decision is close enough that the 3-arm sim
// chooser (accept / challenge-invalid / challenge-valid worlds) is
// worth running; clear accepts and clear challenges skip the sims.
bool nerfed_player_challenge_is_marginal(
    const NerfedChallengeAssessment *assessment);

// Sim-based decision: world utilities from rollouts (in the same
// utility scale as the static assessment: win probability plus the
// spread term), mixed by the posterior —
//   EU(challenge) = p_invalid * u_off + (1 - p_invalid) * u_fail
// with the fitted decision noise against the assessment's threshold.
bool nerfed_player_challenge_decide_simmed(
    const NerfedPlayer *nerfed_player,
    const NerfedChallengeAssessment *assessment, double u_accept, double u_off,
    double u_fail, XoshiroPRNG *prng);

// The fitted valuation-noise sigma (equity points) for a target rating.
// Used by exploitative rollouts to model a nerfed opponent's dispersion.
double nerfed_player_sigma_for_rating(int rating);

// The static win-utility of a score margin from the perspective of the
// player holding the margin (win sigmoid plus linear spread term).
// Used as the terminal/fallback state utility by the sim chooser.
double nerfed_player_margin_utility(double margin);

// Rollout world utility in the assessment scale: win probability plus
// the challenge spread term.
double nerfed_player_challenge_state_utility(double win_probability,
                                             double spread);

// Convenience wrapper: assess then statically decide.
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
