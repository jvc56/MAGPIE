#include "nerfed_player.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../ent/board.h"
#include "../ent/dictionary_word.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/words.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_gen.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  NERFED_MOVE_LIST_CAPACITY = 1024,
  NERFED_MAX_WORD_FEATS = 400000,
  NERFED_MAX_PHONY_BELIEFS = 200000,
  NERFED_LINE_BUFFER_SIZE = 128,
};

// Stage B visibility model (dominated-miss logistic, delta = 10 equity pts)
// fitted on 380,370 corpus positions, with explicit word-length terms.
// Predicts P(MISS play); visibility is its complement. Feature order
// matches nerfed_player_miss_probability.
enum { NERFED_NUM_MISS_COEFFS = 17 };

// Separate coefficient sets per lexicon family: geometry terms transfer
// almost exactly, but literacy matters ~2x more in CSW and the length
// effects differ (fit comparison on 314k TWL / 66k CSW positions).
// Feature order: bias, rating_z, min_logplay, min_loglit, any_absent,
// (n_words-1)/2, blank, through, new/7, log_class, len2, len7plus,
// rtg*play, rtg*lit, rtg*blank, rtg*len2, rtg*len7plus.
static const double NERFED_MISS_COEFFS_TWL[NERFED_NUM_MISS_COEFFS] = {
    0.805,  -0.656, -0.749, -0.149, 0.584, 0.531, 0.523, 0.610,  -0.056,
    -0.226, 0.456,  -0.329, 0.082,  0.049, 0.050, 0.097, -0.246,
};
static const double NERFED_MISS_COEFFS_CSW[NERFED_NUM_MISS_COEFFS] = {
    0.810,  -0.624, -0.700, -0.330, 0.671, 0.478, 0.527, 0.466,  -0.120,
    -0.281, 0.311,  -0.281, 0.083,  0.118, 0.077, 0.041, -0.261,
};

// Stage A valuation noise: sigma = exp(C0 + C1 * rating_z) equity points.
static const double NERFED_SIGMA_C0 = 1.59233;
static const double NERFED_SIGMA_C1 = -0.17506;

// Exchange propensity: P(exchange | delta, rating) fitted on 18,077 corpus
// turns with bag >= 7, where delta = best tile-play equity minus best
// exchange equity (capped at 60, scaled per 10 pts). Weak players keep
// exchanging even when strong plays are available (delta x rating term).
// Intercept and rating terms are calibrated so simulated exchanges per
// game match the corpus by rating (0.45 at 1000 .. 0.245 at 2200); the
// margin terms are the corpus logistic fit.
enum { NERFED_NUM_EXCH_COEFFS = 8 };
// Rack-texture-aware propensity (all texture terms significant): bad
// vowel balance, duplicates, Q without U, and a held blank all raise the
// exchange probability beyond what the engine margin explains. Intercept
// and rating terms are recalibrated so simulated exchanges per game match
// the corpus by rating.
static const double NERFED_EXCH_COEFFS[NERFED_NUM_EXCH_COEFFS] = {
    -2.344, // intercept (calibrated, 4-point)
    -0.661, // delta / 10
    0.157,  // rating_z (calibrated, 4-point)
    -0.149, // (delta / 10) x rating_z
    0.182,  // |vowels - 3|
    0.176,  // duplicate tiles
    0.215,  // Q without U
    0.319,  // holds a blank
};
static const double NERFED_EXCH_DELTA_CAP = 60.0;

// Keep-selection model (conditional logit on 6,494 corpus exchanges):
// utility = equity / sigma_exch + gamma * tiles_thrown, with
// sigma_exch = exp(K0 + K1 * rating_z). Humans of all ratings throw
// slightly more tiles than the engine-best keep (gamma > 0), and weak
// players keep noisier leaves (sigma_exch 4.2 pts at 1000 vs 1.7 at 2200).
static const double NERFED_KEEP_SIGMA_C0 = 1.045;
static const double NERFED_KEEP_SIGMA_C1 = -0.229;
static const double NERFED_KEEP_THROW_BIAS = 0.1075;
// Equity assigned to "no exchange available" in the fit when movegen had
// no exchange row (matches the fit's default margin baseline).
static const double NERFED_NO_EXCHANGE_EQUITY = -10.0;

// Knowledge caps: subjective word confidence almost never reaches 0 or
// 100%, EXCEPT for the highest-playability words (for higher-rated
// players) and highest-literacy words, where certainty is released.
static const double NERFED_KNOW_CAP = 0.04;

// Static-eval challenge decision: outcomes are mapped through a win
// sigmoid of the score margin, so desperation challenges emerge when the
// game is on the line (accept ~ certain loss; challenge has variance).
static const double NERFED_WIN_SIGMOID_SCALE = 40.0;
static const double NERFED_TEMPO_VALUE = 30.0;
static const double NERFED_CHALLENGE_THRESH_C0 = 0.020;
static const double NERFED_CHALLENGE_THRESH_RTG = -0.004;
// Corpus failed-challenge targets (solidified via the canonical GCG token
// grammar, tools/challenge_measure.py): CSW/5-point failed challenges are
// 0.47-0.70/game and roughly FLAT across ratings (cheap challenges get
// thrown liberally at every level); TWL/double failed challenges run
// ~0.50/game at 1000 down to ~0.14 at 2000.
static const double NERFED_CHALLENGE_NOISE = 0.004;
// Speculative challenge: beyond the rational-EV decision, players throw
// challenges at obscure-LOOKING plays on a hunch, regardless of whether
// they believe the word. This is what keeps experts challenging (the
// belief gate leaves them almost no rational opportunities) and what
// drives the bulk of failed challenges, especially under the cheap
// 5-point rule. P(speculative challenge) = attention * base(rule) *
// sigmoid((center - min_logplay)/scale): common plays (high playability)
// are essentially never speculatively challenged; floor-obscure plays
// are challenged at the family base rate. Rating-flat by design, so it
// does not collapse at the expert end the way the EV path does.
static const double NERFED_CHALLENGE_SPEC_5PT = 0.64;
static const double NERFED_CHALLENGE_SPEC_DOUBLE = 0.50;
static const double NERFED_CHALLENGE_SPEC_CENTER = 0.5;
static const double NERFED_CHALLENGE_SPEC_SCALE = 0.9;
// Rating dependence of speculation, per family. CSW/5-point is nearly
// FLAT (the corpus failed-challenge rate barely moves with rating —
// cheap challenges are thrown at every level); TWL/double falls with
// rating (an expensive lost turn deters strong players). Scales the base
// rate by exp(-slope * rating_z).
static const double NERFED_CHALLENGE_SPEC_RTG_5PT = -0.05;
static const double NERFED_CHALLENGE_SPEC_RTG_DOUBLE = 0.26;
// Under double challenge, challengers undervalue the turn they stand to
// lose when they smell a phony (risk-seeking on the gotcha): the lost
// turn enters the challenge EU at this fraction of its true tempo value.
// (The bulk of failed double challenges now comes from the speculative
// term above; this only shapes the rational-EV path.)
static const double NERFED_CHALLENGE_DOUBLE_TEMPO_FACTOR = 0.35;
// Prior probability an opponent's play is a phony: the act of playing a
// word is strong evidence of validity, so the challenge decision uses the
// Bayesian posterior, not raw unfamiliarity. Two rating dependencies,
// both keyed on the CHALLENGER's own rating:
//  - Base prior falls with challenger rating: weak players believe
//    phonies are everywhere (they and their peers play them constantly)
//    and challenge liberally; experts hold a low base and challenge
//    selectively.
//  - Opponent-strength tracking rises with challenger rating: an expert
//    correctly credits a strong opponent's obscure word (a 2200's word
//    is rarely a phony), but a weak player does no such reasoning -- they
//    challenge the weird-looking word regardless of who played it. The
//    opponent-rating slope is therefore scaled by a sigmoid of the
//    challenger's rating (near 0 for beginners, near 1 for experts).
// Corpus: matched-rating successful challenges run 0.47/game at 1000 ->
// 0.21 at 2000, and a weak player challenges a strong opponent's obscure
// words at nearly the same rate as a peer's -- opponent-blind.
static const double NERFED_OPP_PHONY_PRIOR = 0.04;
static const double NERFED_OPP_PHONY_PRIOR_RTG = 0.40;
static const double NERFED_OPP_PHONY_PRIOR_CHALLENGER_RTG = 0.35;
static const double NERFED_OPP_PHONY_PRIOR_MIN = 0.005;
static const double NERFED_OPP_PHONY_PRIOR_MAX = 0.15;
// Opponent rating z assumed for an unnerfed (full-strength) opponent.
static const double NERFED_UNNERFED_OPP_RATING_Z = 2.33;
// Spread term in the challenge utility: keeps the challenge gradient
// alive when the win sigmoid saturates (players still challenge for
// spread when far ahead or behind).
static const double NERFED_SPREAD_UTIL = 0.004;
// Extra offset on the pseudo-coverage likelihood: how completely players
// know the common-shaped word classes (sweep-calibrated).
static const double NERFED_COVERAGE_OFFSET = -1.0;
static const double NERFED_COVERAGE_OFFSET_RTG = -0.4;
// 5-point challenges are cheap, so Collins players habitually verify:
// their effective willingness to treat unfamiliarity as evidence is
// higher (drives the corpus 72% vs 51% family gap). Rating-dependent:
// the habit only pays for players whose coverage is actually good —
// a flat bonus floods weak-vs-weak pairs with bad challenges (measured
// 0.66/game at 1400v1400 vs the ~0.12 corpus rate).
static const double NERFED_COVERAGE_5PT_BONUS = -0.4;
static const double NERFED_COVERAGE_5PT_BONUS_RTG = -0.05;
// Double-challenge coverage confidence: TWL players also overestimate
// how completely they know the word classes ("I'd know that word"),
// which is what makes them occasionally challenge obscure VALID words
// and eat the lost turn. Paired with the raised double threshold below:
// the bonus worsens the posterior's phony/valid selectivity (raising the
// bad-challenge fraction) while the threshold holds the overall phony-
// challenge rate at the corpus ~51%.
static const double NERFED_COVERAGE_DOUBLE_BONUS = -1.6;
static const double NERFED_CHALLENGE_THRESH_DOUBLE_EXTRA = 0.020;
// Attention: under double challenge most plays are accepted without any
// phony evaluation at all — challenging is deliberate and risky, and
// the corpus 51% catch rate reflects attention x accuracy, not low
// accuracy (the posterior is ~90% accurate when it engages). 5pt
// culture checks everything (attention 1.0 implicit).
static const double NERFED_CHALLENGE_DOUBLE_ATTENTION = 0.57;
static const double NERFED_CHALLENGE_5PT_ATTENTION = 0.80;
// Attention rises toward 1.0 for obscure/suspicious plays, scaled by how
// well the challenger recognizes suspicion (a logistic in the
// challenger's rating): a strong player attends to a blatant play almost
// always, a beginner barely more than to an ordinary one. Centered below
// average so that mid-strength players already recognize most obscure
// plays and experts are essentially certain to look.
static const double NERFED_CHALLENGE_ATTN_RECOG_CENTER = -0.5;
static const double NERFED_CHALLENGE_ATTN_RECOG_SCALE = 0.5;
// Static-EU margin (utility units) within which the 3-arm sim chooser
// runs; outside the band the static verdict stands (humans only
// deliberate over close calls, and world-sims are expensive).
static const double NERFED_CHALLENGE_SIM_BAND = 0.06;

// Expected-challenge discount when PLAYING risky words: the mover's own
// capped confidence proxies how challengeable the word looks; expected
// cost = P(challenged) * (lost play value + lost tempo).
static const double NERFED_OPP_CHALLENGE_RATE = 0.5;
// Residual doubt about words the player believes they know: subjective
// P(valid | believed) = 1 - NERFED_KNOW_RISK * (1 - prior).
static const double NERFED_KNOW_RISK = 0.35;
// Challenge-bait economics: a CONFIDENT mover gains from a wrong
// challenge (5 pts/word, or the challenger's lost turn under double,
// valued at NERFED_BAIT_DOUBLE_GAIN points); a doubtful mover risks the
// play plus tempo. This is what makes an expert intentionally play
// obscure valid words to elicit challenges. The mover's estimate of
// P(opponent challenges) is built from the shared speculative term (at
// the opponent's rating) OR'd with the rational familiarity channel, so
// it stays consistent with what the opponent's challenge model does.
static const double NERFED_BAIT_DOUBLE_GAIN = 22.0;
static const double NERFED_BAIT_5PT_GAIN_PER_WORD = 5.0;

// Own-play confidence in a believed phony (a word from the player's own
// believed lexicon that is absent from the real one). Corpus phony
// rates (0.78/game at 1400) prove near-zero self-censorship: people
// play TE because they think it is a word. Rating the word from the
// REAL feature table instead (deep obscure -> ~7 pt challenge-risk
// discount) suppressed phony plays ~10x below corpus.
// Rating-dependent: weak players are fearless (corpus 1000s play
// phonies at ~2x the 2000 rate despite believing MORE false words);
// experts risk-assess even words they believe. This, not belief mass,
// carries the corpus rating gradient — belief-mass shrinkage alone
// left rates flat because the believed head saturates.
static const double NERFED_OWN_BELIEF_CONF_BASE = 0.88;
static const double NERFED_OWN_BELIEF_CONF_SLOPE = -0.08;
// Per-word phony belief priors (<real_lex>_phony_beliefs.csv): b0 =
// log(generator tier weight x orthographic plausibility). Belief is a
// per-player PER-TURN draw at p = sigmoid(b0 + offset + slope *
// rating_z), re-realized each turn (never fixed for the whole game;
// only challenge-adjudicated words persist). Two same-rated players
// share the head phonies (TE) but split on the idiosyncratic tail —
// graded overlap is what makes challenges of phonies possible at
// matched ratings (a shared fixed believed set collapses the challenge
// rate to ~4%). Cross-family words get a positive rating slope (experts
// know the other dictionary); the negative in-family slope is mild —
// experts hold fewer false beliefs, but the corpus successful-challenge
// rate at 2200 (~0.24/game CSW) requires them to still play a fair
// number of marginal phonies (word knowledge at the arsenal's edge is
// fuzzy). Too steep a slope starves the expert catch rate.
static const double NERFED_PHONY_BELIEF_OFFSET = -1.2;
// Family split: the TWL candidate pool converts belief mass into plays
// more readily (larger cross-family pool, denser short-word phonies).
static const double NERFED_PHONY_BELIEF_TWL_EXTRA = -0.51;
static const double NERFED_PHONY_BELIEF_CSW_EXTRA = 0.31;
static const double NERFED_PHONY_BELIEF_SLOPE = -0.53;
static const double NERFED_PHONY_BELIEF_SLOPE_CSW = -0.42;
static const double NERFED_PHONY_BELIEF_XFAM_SLOPE = 0.10;
// Cross-family words are a huge pool (40k CSW-only words on the TWL
// side): without an extra offset the belief mass dwarfs the corpus
// phony rate and INVERTS the rating gradient (a 2200 believed ~28k
// other-dictionary words and played 1.9 phonies/game).
static const double NERFED_PHONY_BELIEF_XFAM_OFFSET = -1.5;
// Logit for candidate phonies missing from the belief table (an
// opponent's phony outside this player's candidate pool).
static const double NERFED_PHONY_BELIEF_UNKNOWN_LOGIT = -3.9;
// Equity points per utility unit for the sim-based pick: converts the
// sim's [0, 1] win%/spread utility blend into equity-point equivalents.
// Set from the local slope of utility in spread points for a mid-game
// position (win% moves ~0.4%/pt plus the sigmoid spread channel at
// uspread=0.5).
static const double NERFED_SIM_UTILITY_PER_POINT = 0.0035;
// Glimpse-of-truth weight for the sim-based pick: arm value is
// (1 - w) * static equity + w * sim-implied points, with the fitted
// valuation noise unchanged. At w = 0 the pick reduces to the static
// path; w is the strength-neutrality knob (a full-sim pick at the fitted
// sigma won 83% against the static path at equal rating). Blocking and
// setup intent live exactly where sim and static disagree, so the weight
// stays deliberately partial.
static const double NERFED_SIM_GLIMPSE_W = 0.3;
// Sigma scale for the sim-based pick. The fitted sigma absorbs both
// human valuation noise AND the static eval's own error; the glimpse
// removes part of the latter, so dispersion must rise to keep
// loss-per-turn (and head-to-head strength vs the static path) equal.
// The glimpse edge is w-insensitive (53-54% at w in [0.1, 0.3]; only
// the sign of the correction matters at the noise margin), so
// neutrality is restored here, not by shrinking w.
static const double NERFED_SIM_SIGMA_SCALE = 1.06;
// Empirical-Bayes shrinkage of the glimpse by arm sample count:
// disagreement * n / (n + N0). With stratified arms the per-arm rollout
// counts get thin (600 iters / 24 arms ~ 25 samples -> +-5 pts of
// sampling noise in the mean), and an unshrunk glimpse promotes deep
// arms on noise (measured 47.1% head-to-head, score -14). Shrinkage
// restores trust proportional to evidence.
static const double NERFED_SIM_SHRINK_N0 = 60.0;

// Defaults for words missing from the feature table (centered scales).
static const double NERFED_DEFAULT_LOGPLAY = -2.0;
static const double NERFED_DEFAULT_LOGLIT = -1.0;
// Extra visibility miss for 9+ letter plays. The fitted len7plus feature
// lumps 7/8/9 letter words together, but a 9+ letter play is a through-
// play that humans rarely SEE — the model over-surfaces 9-letter bingos
// ~1.5-1.8x vs corpus while 7- and 8-letter bingos are calibrated. The
// real difficulty is not raw length but the geometry: a 9-letter word
// made by a clean end-hook/extension is far easier to spot than one that
// BRIDGES interior board tiles. So the miss is a small flat term per
// letter beyond 8 (the easy extensions) plus a larger term per interior
// through-tile the play bridges (through-tiles between the first and last
// placed tile). Both apply only at 9+ letters, leaving the calibrated
// 7/8-letter hooks untouched regardless of geometry.
static const double NERFED_LONG9_MISS_PENALTY = 0.35;
static const double NERFED_LONG9_BRIDGE_PENALTY = 0.7;
// Experts still spot long/bridging plays: the penalty tapers above
// average rating (full for <=1600, ~half by 2200) so the expert 9-bingo
// rate keeps rising toward the corpus while mid/low players stay
// suppressed. Beginners are never amplified beyond the full penalty.
static const double NERFED_LONG9_RTG_TAPER = 0.12;
static const double NERFED_LONG9_RTG_FLOOR = 0.25;

// Endgame choice model (conditional logit on 32,675 corpus endgame
// choices): utility = value/sigma + score and outplay preferences.
// Humans choose endgame moves score-greedily (10 score pts pull +68
// equity-pts at rating 1000, +9 at 2200 — imperfect opponent-rack
// tracking) and strongly prefer going out. sigma is residual noise.
static const double NERFED_ENDGAME_SIGMA_C0 = 2.901;
static const double NERFED_ENDGAME_SIGMA_C1 = -0.566;
// Difficulty interaction: hard positions make experts noisier but push
// weak players onto pure score-greed (lower residual noise).
static const double NERFED_ENDGAME_SIGMA_HARD = -0.100;
static const double NERFED_ENDGAME_SIGMA_HARD_RTG = 0.140;
static const double NERFED_ENDGAME_SCORE_PREF_C0 = 1.617;
static const double NERFED_ENDGAME_SCORE_PREF_C1 = 0.030;
static const double NERFED_ENDGAME_OUTPLAY_PREF_C0 = 2.233;
static const double NERFED_ENDGAME_OUTPLAY_PREF_C1 = 0.101;
// Runtime hard-position proxy (logistic, AUC 0.85 vs the snapshot-study
// tiers): tiles remaining, top-2 value gap, and field size.
static const double NERFED_ENDGAME_HARD_BIAS = -2.696;
static const double NERFED_ENDGAME_HARD_TILES = 1.375;
static const double NERFED_ENDGAME_HARD_GAP = -1.041;
static const double NERFED_ENDGAME_HARD_NMOVES = 0.686;

// Offset applied to the knowledge-only miss logit (the Stage B model's
// intercept absorbs spotting misses as well as knowledge misses; endgame
// deliberation is exhaustive, so pure word knowledge should miss less).
// Calibrate against the corpus endgame rank curves.
static const double NERFED_KNOW_OFFSET = -1.0;

typedef struct NerfedWordFeat {
  MachineLetter word[BOARD_DIM];
  uint8_t word_length;
  uint8_t absent;
  float logplay;
  float loglit;
} NerfedWordFeat;

// Per-turn cache of the uniform draw for each distinct rarest-word so that
// all plays sharing that word share the same visibility draw (a player who
// does not know a word misses EVERY placement of it, not each
// independently). Comonotone coupling: the same u is compared against each
// play's own miss probability, so among plays sharing the word the easier
// geometry is still seen more often.
typedef struct NerfedWordDraw {
  MachineLetter word[BOARD_DIM];
  uint8_t word_length;
  double uniform;
} NerfedWordDraw;

enum { NERFED_MAX_WORD_DRAWS = 512 };

// Maximum simmed arms per turn regardless of -numplays.
enum { NERFED_MAX_SIM_ARMS = 64 };

enum { NERFED_MAX_REVEALS = 64 };

// A publicly revealed challenge verdict for one word (per game).
typedef struct NerfedWordReveal {
  MachineLetter word[BOARD_DIM];
  uint8_t word_length;
  int8_t verdict;
} NerfedWordReveal;

// Per-word phony belief prior (see NERFED_PHONY_BELIEF_* constants).
typedef struct NerfedPhonyBelief {
  MachineLetter word[BOARD_DIM];
  uint8_t word_length;
  uint8_t xfam;
  float b0;
} NerfedPhonyBelief;

struct NerfedPlayer {
  double rating_z;
  double sigma;
  double keep_sigma;
  const double *miss_coeffs;
  MachineLetter vowel_mls[5];
  MachineLetter q_ml;
  MachineLetter u_ml;
  NerfedWordFeat *feats;
  int num_feats;
  MoveList *move_list;
  NerfedWordDraw word_draws[NERFED_MAX_WORD_DRAWS];
  int num_word_draws;
  // game-level knowledge seed (persistent per game) and current turn for
  // the per-turn flip draw; 0 game seed = legacy per-call seeds.
  uint64_t game_seed;
  int turn_number;
  // Sim-pick state from nerfed_player_prepare_sim_arms: indices into
  // move_list of ALL visible tile plays this turn (best static equity
  // first) and their word confidences. sim_arm_survivor_idx maps arm j
  // (in sim order) to its survivor index — the stratified subset that
  // got simmed; the remaining survivors keep pure static values in the
  // pick so the fitted choice dispersion over the full visible list is
  // preserved.
  int sim_survivor_indices[NERFED_MOVE_LIST_CAPACITY];
  double sim_survivor_confidence[NERFED_MOVE_LIST_CAPACITY];
  double sim_survivor_challenge_ev[NERFED_MOVE_LIST_CAPACITY];
  int num_sim_survivors;
  int sim_arm_survivor_idx[NERFED_MAX_SIM_ARMS];
  int num_sim_arms;
  // The player's believed lexicon (their game KWG, e.g. NWL23PHALL).
  // When set, belief in words ABSENT from the real-lexicon feature
  // table (i.e. phonies) is decided by the per-word belief priors below
  // via a per-player game-level draw. NULL = hash-draw belief only
  // (endgame and non-phony paths).
  const KWG *believed_kwg;
  NerfedPhonyBelief *phony_beliefs;
  int num_phony_beliefs;
  // Challenge results are public: per-game revealed verdicts.
  // verdict > 0: proven valid; < 0: proven phony (never attempted or
  // challenged again); == 0: suspect (a multi-word play came off and
  // one of its words was phony — ambiguous).
  NerfedWordReveal reveals[NERFED_MAX_REVEALS];
  int num_reveals;
  // Challenge-economics context (set when the phony/challenge flow is
  // active): the rule and the opponent's rating drive the expected
  // value of being challenged.
  bool challenge_ctx_set;
  bool challenge_ctx_rule_5pt;
  double challenge_ctx_opp_rating_z;
};

static int nerfed_word_feat_compare(const void *feat_a, const void *feat_b) {
  const NerfedWordFeat *word_feat_a = (const NerfedWordFeat *)feat_a;
  const NerfedWordFeat *word_feat_b = (const NerfedWordFeat *)feat_b;
  if (word_feat_a->word_length != word_feat_b->word_length) {
    return (int)word_feat_a->word_length - (int)word_feat_b->word_length;
  }
  return memcmp(word_feat_a->word, word_feat_b->word, word_feat_a->word_length);
}

static int nerfed_phony_belief_compare(const void *belief_a,
                                       const void *belief_b) {
  const NerfedPhonyBelief *phony_a = (const NerfedPhonyBelief *)belief_a;
  const NerfedPhonyBelief *phony_b = (const NerfedPhonyBelief *)belief_b;
  if (phony_a->word_length != phony_b->word_length) {
    return (int)phony_a->word_length - (int)phony_b->word_length;
  }
  return memcmp(phony_a->word, phony_b->word, phony_a->word_length);
}

static const NerfedPhonyBelief *
nerfed_player_lookup_belief(const NerfedPlayer *nerfed_player,
                            const MachineLetter *word, int word_length) {
  if (nerfed_player->num_phony_beliefs == 0) {
    return NULL;
  }
  NerfedPhonyBelief key;
  memset(&key, 0, sizeof(key));
  key.word_length = (uint8_t)word_length;
  memcpy(key.word, word, word_length);
  return (const NerfedPhonyBelief *)bsearch(
      &key, nerfed_player->phony_beliefs, nerfed_player->num_phony_beliefs,
      sizeof(NerfedPhonyBelief), nerfed_phony_belief_compare);
}

static const NerfedWordReveal *
nerfed_player_lookup_reveal(const NerfedPlayer *nerfed_player,
                            const MachineLetter *word, int word_length) {
  for (int reveal_idx = 0; reveal_idx < nerfed_player->num_reveals;
       reveal_idx++) {
    const NerfedWordReveal *reveal = &nerfed_player->reveals[reveal_idx];
    if (reveal->word_length == word_length &&
        memcmp(reveal->word, word, word_length) == 0) {
      return reveal;
    }
  }
  return NULL;
}

static const NerfedWordFeat *
nerfed_player_lookup_word(const NerfedPlayer *nerfed_player,
                          const MachineLetter *word, int word_length) {
  NerfedWordFeat key;
  memset(&key, 0, sizeof(key));
  key.word_length = (uint8_t)word_length;
  memcpy(key.word, word, word_length);
  return (const NerfedWordFeat *)bsearch(
      &key, nerfed_player->feats, nerfed_player->num_feats,
      sizeof(NerfedWordFeat), nerfed_word_feat_compare);
}

// Tests one candidate stem: if it is a valid word more recognizable than
// the best seen so far, adopt its logplay and loglit.
static void nerfed_backfill_try(const NerfedPlayer *nerfed_player,
                                const MachineLetter *cand, int cand_len,
                                float own_loglit, float *best_logplay,
                                float *best_loglit) {
  if (cand_len < 2) {
    return;
  }
  const NerfedWordFeat *stem =
      nerfed_player_lookup_word(nerfed_player, cand, cand_len);
  if (stem != NULL && stem->logplay > *best_logplay) {
    *best_logplay = stem->logplay;
    *best_loglit = fmaxf(own_loglit, stem->loglit);
  }
}

// A productive inflection (a stem plus -ING/-ED/-S/-ER/-EST/-LY, handling
// e-drop and consonant doubling) should be as VISIBLE as its stem: a
// literate player reads CONSIDERING off CONSIDER + -ING on sight even
// though the engine almost never plays the long form, so its self-play
// logplay floors. Self-play frequency is the wrong signal for visibility;
// recognizability is. Backfills each inflection's logplay/loglit from its
// most recognizable valid stem. Runs once at load over the sorted table
// (stems, being base words, are not themselves rewritten, so order is
// immaterial). This is a runtime stand-in for a morphology-aware feature
// that should eventually be precomputed into the wordfeats data.
static void nerfed_player_backfill_inflections(NerfedPlayer *nerfed_player,
                                               const LetterDistribution *ld) {
  MachineLetter s[10];
  if (ld_str_to_mls(ld, "INGEDSRTLY", false, s, 10) != 10) {
    return; // non-English distribution: skip morphology backfill
  }
  const MachineLetter mI = s[0], mN = s[1], mG = s[2], mE = s[3], mD = s[4];
  const MachineLetter mS = s[5], mR = s[6], mT = s[7], mL = s[8], mY = s[9];
  for (int feat_idx = 0; feat_idx < nerfed_player->num_feats; feat_idx++) {
    NerfedWordFeat *feat = &nerfed_player->feats[feat_idx];
    const int len = feat->word_length;
    const MachineLetter *w = feat->word;
    const float own_loglit = feat->loglit;
    float best_logplay = feat->logplay;
    float best_loglit = feat->loglit;
    MachineLetter cand[BOARD_DIM];
#define BF_TRY(clen)                                                           \
  nerfed_backfill_try(nerfed_player, cand, (clen), own_loglit, &best_logplay,  \
                      &best_loglit)
    if (len >= 5 && w[len - 3] == mI && w[len - 2] == mN && w[len - 1] == mG) {
      memcpy(cand, w, (size_t)(len - 3) * sizeof(MachineLetter));
      BF_TRY(len - 3);                // WALKING -> WALK
      cand[len - 3] = mE;             // e-drop
      BF_TRY(len - 2);                // BAKING -> BAKE
      if (w[len - 4] == w[len - 5]) { // undouble
        BF_TRY(len - 4);              // STOPPING -> STOP
      }
    } else if (len >= 4 && w[len - 2] == mE && w[len - 1] == mD) {
      memcpy(cand, w, (size_t)(len - 1) * sizeof(MachineLetter));
      BF_TRY(len - 2); // WALKED -> WALK
      BF_TRY(len - 1); // BAKED -> BAKE (drop D)
      if (len >= 5 && w[len - 3] == w[len - 4]) {
        BF_TRY(len - 3); // STOPPED -> STOP
      }
    } else if (len >= 5 && w[len - 3] == mE && w[len - 2] == mS &&
               w[len - 1] == mT) {
      memcpy(cand, w, (size_t)(len - 2) * sizeof(MachineLetter));
      BF_TRY(len - 3); // TALLEST -> TALL
      BF_TRY(len - 2); // NICEST -> NICE
      if (w[len - 4] == w[len - 5]) {
        BF_TRY(len - 4); // BIGGEST -> BIG
      }
    } else if (len >= 4 && w[len - 2] == mE && w[len - 1] == mR) {
      memcpy(cand, w, (size_t)(len - 1) * sizeof(MachineLetter));
      BF_TRY(len - 2); // WALKER -> WALK
      BF_TRY(len - 1); // NICER -> NICE
      if (len >= 5 && w[len - 3] == w[len - 4]) {
        BF_TRY(len - 3); // BIGGER -> BIG
      }
    } else if (len >= 4 && w[len - 2] == mL && w[len - 1] == mY) {
      memcpy(cand, w, (size_t)(len - 2) * sizeof(MachineLetter));
      BF_TRY(len - 2); // QUICKLY -> QUICK
      if (w[len - 3] == mI) {
        cand[len - 3] = mY;
        BF_TRY(len - 2); // HAPPILY -> HAPPY (ILY -> Y)
      }
    } else if (len >= 3 && w[len - 1] == mS) {
      memcpy(cand, w, (size_t)(len - 1) * sizeof(MachineLetter));
      BF_TRY(len - 1); // CATS -> CAT
      if (len >= 4 && w[len - 2] == mE) {
        BF_TRY(len - 2); // BOXES -> BOX
        if (w[len - 3] == mI) {
          cand[len - 3] = mY;
          BF_TRY(len - 2); // TRIES -> TRY (IES -> Y)
        }
      }
    }
#undef BF_TRY
    feat->logplay = best_logplay;
    feat->loglit = best_loglit;
  }
}

NerfedPlayer *nerfed_player_create(const Game *game, int rating,
                                   ErrorStack *error_stack) {
  const Player *player = game_get_player(game, 0);
  const char *kwg_name = kwg_get_name(player_get_kwg(player));
  const LetterDistribution *ld = game_get_ld(game);
  // strip a PH<rating> believed-lexicon suffix: word features (and the
  // family coefficient choice) belong to the REAL lexicon; phony words
  // are simply absent from the table and get capped-low confidence
  char lexicon_name[64];
  size_t base_len = string_length(kwg_name);
  while (base_len > 0 && isdigit((unsigned char)kwg_name[base_len - 1])) {
    base_len--;
  }
  if (base_len >= 2 && kwg_name[base_len - 2] == 'P' &&
      kwg_name[base_len - 1] == 'H') {
    base_len -= 2;
  } else {
    base_len = string_length(kwg_name);
    // the PHALL union believed lexicon (all candidate phonies; belief
    // comes from the per-word priors, not membership)
    if (base_len > 5 && strcmp(kwg_name + base_len - 5, "PHALL") == 0) {
      base_len -= 5;
    }
  }
  if (base_len >= sizeof(lexicon_name)) {
    base_len = sizeof(lexicon_name) - 1;
  }
  memcpy(lexicon_name, kwg_name, base_len);
  lexicon_name[base_len] = '\0';
  char *feats_filename =
      get_formatted_string("data/lexica/%s_wordfeats.csv", lexicon_name);
  FILE *feats_file = fopen(feats_filename, "r");
  if (!feats_file) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
        get_formatted_string("could not open nerfed player word features: %s",
                             feats_filename));
    free(feats_filename);
    return NULL;
  }
  NerfedPlayer *nerfed_player = malloc_or_die(sizeof(NerfedPlayer));
  nerfed_player->rating_z = (rating - 1500.0) / 300.0;
  nerfed_player->sigma =
      exp(NERFED_SIGMA_C0 + NERFED_SIGMA_C1 * nerfed_player->rating_z);
  nerfed_player->keep_sigma = exp(
      NERFED_KEEP_SIGMA_C0 + NERFED_KEEP_SIGMA_C1 * nerfed_player->rating_z);
  // CSW-family lexica (incl. OSWI/SOWPODS) use the CSW coefficient set.
  if (strncmp(lexicon_name, "CSW", 3) == 0 ||
      strncmp(lexicon_name, "OSW", 3) == 0 ||
      strncmp(lexicon_name, "SOWPODS", 7) == 0) {
    nerfed_player->miss_coeffs = NERFED_MISS_COEFFS_CSW;
  } else {
    nerfed_player->miss_coeffs = NERFED_MISS_COEFFS_TWL;
  }
  const char *vowels = "AEIOU";
  for (int vowel_idx = 0; vowel_idx < 5; vowel_idx++) {
    const char vowel_str[2] = {vowels[vowel_idx], '\0'};
    ld_str_to_mls(ld, vowel_str, false, &nerfed_player->vowel_mls[vowel_idx],
                  1);
  }
  ld_str_to_mls(ld, "Q", false, &nerfed_player->q_ml, 1);
  ld_str_to_mls(ld, "U", false, &nerfed_player->u_ml, 1);
  nerfed_player->feats =
      malloc_or_die(sizeof(NerfedWordFeat) * NERFED_MAX_WORD_FEATS);
  nerfed_player->num_feats = 0;
  char line[NERFED_LINE_BUFFER_SIZE];
  while (fgets(line, sizeof(line), feats_file)) {
    char *comma = strchr(line, ',');
    if (!comma) {
      continue;
    }
    *comma = '\0';
    float logplay = 0;
    float loglit = 0;
    int absent = 0;
    if (sscanf(comma + 1, "%f,%f,%d", &logplay, &loglit, &absent) != 3) {
      continue;
    }
    if (nerfed_player->num_feats >= NERFED_MAX_WORD_FEATS) {
      log_fatal("nerfed player word feature table overflow: %s",
                feats_filename);
    }
    NerfedWordFeat *feat = &nerfed_player->feats[nerfed_player->num_feats];
    memset(feat, 0, sizeof(*feat));
    const int num_mls = ld_str_to_mls(ld, line, false, feat->word, BOARD_DIM);
    if (num_mls <= 1 || num_mls > BOARD_DIM) {
      continue;
    }
    feat->word_length = (uint8_t)num_mls;
    feat->logplay = logplay;
    feat->loglit = loglit;
    feat->absent = (uint8_t)absent;
    nerfed_player->num_feats++;
  }
  fclose_or_die(feats_file);
  free(feats_filename);
  qsort(nerfed_player->feats, nerfed_player->num_feats, sizeof(NerfedWordFeat),
        nerfed_word_feat_compare);
  // Visibility of a productive inflection tracks its stem's, not its own
  // (floored) self-play frequency.
  nerfed_player_backfill_inflections(nerfed_player, ld);
  // Optional per-word phony belief priors (absent for lexica without a
  // generated candidate pool; belief then falls back to the unknown
  // logit for every phony).
  nerfed_player->phony_beliefs = NULL;
  nerfed_player->num_phony_beliefs = 0;
  char *beliefs_filename =
      get_formatted_string("data/lexica/%s_phony_beliefs.csv", lexicon_name);
  FILE *beliefs_file = fopen(beliefs_filename, "r");
  if (beliefs_file) {
    nerfed_player->phony_beliefs =
        malloc_or_die(sizeof(NerfedPhonyBelief) * NERFED_MAX_PHONY_BELIEFS);
    while (fgets(line, sizeof(line), beliefs_file)) {
      char *comma = strchr(line, ',');
      if (!comma) {
        continue;
      }
      *comma = '\0';
      float belief_b0 = 0;
      int xfam = 0;
      if (sscanf(comma + 1, "%f,%d", &belief_b0, &xfam) != 2) {
        continue;
      }
      if (nerfed_player->num_phony_beliefs >= NERFED_MAX_PHONY_BELIEFS) {
        log_fatal("nerfed player phony belief table overflow: %s",
                  beliefs_filename);
      }
      NerfedPhonyBelief *belief =
          &nerfed_player->phony_beliefs[nerfed_player->num_phony_beliefs];
      memset(belief, 0, sizeof(*belief));
      const int num_mls =
          ld_str_to_mls(ld, line, false, belief->word, BOARD_DIM);
      if (num_mls <= 1 || num_mls > BOARD_DIM) {
        continue;
      }
      belief->word_length = (uint8_t)num_mls;
      belief->xfam = (uint8_t)xfam;
      belief->b0 = belief_b0;
      nerfed_player->num_phony_beliefs++;
    }
    fclose_or_die(beliefs_file);
    qsort(nerfed_player->phony_beliefs, nerfed_player->num_phony_beliefs,
          sizeof(NerfedPhonyBelief), nerfed_phony_belief_compare);
  }
  free(beliefs_filename);
  nerfed_player->move_list = move_list_create(NERFED_MOVE_LIST_CAPACITY);
  nerfed_player->num_word_draws = 0;
  nerfed_player->game_seed = 0;
  nerfed_player->turn_number = 0;
  nerfed_player->believed_kwg = NULL;
  nerfed_player->num_reveals = 0;
  nerfed_player->challenge_ctx_set = false;
  nerfed_player->challenge_ctx_rule_5pt = false;
  nerfed_player->challenge_ctx_opp_rating_z = 0.0;
  return nerfed_player;
}

void nerfed_player_destroy(NerfedPlayer *nerfed_player) {
  if (!nerfed_player) {
    return;
  }
  move_list_destroy(nerfed_player->move_list);
  free(nerfed_player->feats);
  free(nerfed_player->phony_beliefs);
  free(nerfed_player);
}

static double nerfed_player_uniform(XoshiroPRNG *prng) {
  // 53-bit mantissa uniform in (0, 1); never returns exactly 0 or 1.
  return ((double)(prng_next(prng) >> 11) + 0.5) / 9007199254740992.0;
}

// How obscure/suspicious a play looks, from the playability of its rarest
// formed word: ~1 for floor-obscure plays (a word absent from the real
// table looks maximally suspicious), ~0 for common plays. Drives both the
// speculative-challenge rate and how much attention the play draws.
static double nerfed_obscurity(double min_logplay) {
  if (min_logplay > 90.0) {
    min_logplay = NERFED_DEFAULT_LOGPLAY;
  }
  return 1.0 / (1.0 + exp(-(NERFED_CHALLENGE_SPEC_CENTER - min_logplay) /
                          NERFED_CHALLENGE_SPEC_SCALE));
}

// P(a rater of rating rater_z speculatively challenges a play whose
// rarest formed word has playability min_logplay), under the given rule.
// The single source of truth for the hunch-challenge rate: the actual
// challenger (nerfed_player_challenge_assess) and the mover's model of
// being baited (nerfed_player_miss_probability) both call this so they
// agree on how often obscure plays draw challenges.
static double nerfed_speculative_challenge_prob(bool rule_5pt,
                                                double min_logplay,
                                                double rater_z) {
  const double spec_base =
      rule_5pt ? NERFED_CHALLENGE_SPEC_5PT : NERFED_CHALLENGE_SPEC_DOUBLE;
  const double spec_rtg = rule_5pt ? NERFED_CHALLENGE_SPEC_RTG_5PT
                                   : NERFED_CHALLENGE_SPEC_RTG_DOUBLE;
  return spec_base * nerfed_obscurity(min_logplay) * exp(-spec_rtg * rater_z);
}

// P(a challenger of rating rater_z engages the challenge decision at all
// (the "attention" gate) for a play of the given obscurity. Ordinary
// plays draw the base engagement rate; obscure/suspicious plays draw
// near-certain attention from a STRONG challenger (they recognize a
// blatant play and look hard), but only slightly more from a beginner
// (who does not register the play as suspicious). Once attended, whether
// it is actually challenged is the knowledge/EV decision downstream — so
// exceptionally bad phonies, which look obscure AND score a high
// p_invalid, are almost always challenged by experts.
static double nerfed_attention_prob(bool rule_5pt, double obscurity,
                                    double rater_z) {
  const double base = rule_5pt ? NERFED_CHALLENGE_5PT_ATTENTION
                               : NERFED_CHALLENGE_DOUBLE_ATTENTION;
  const double recognition =
      1.0 / (1.0 + exp(-(rater_z - NERFED_CHALLENGE_ATTN_RECOG_CENTER) /
                       NERFED_CHALLENGE_ATTN_RECOG_SCALE));
  return base + (1.0 - base) * obscurity * recognition;
}

// P(this play is invisible to the player): Stage B miss logistic with a
// best-class of one (log_class = 0). Writes the play's rarest formed word
// (minimum playability) to rare_word/rare_word_length so the caller can
// share one visibility draw across all plays using that word.
static double nerfed_player_miss_probability(const NerfedPlayer *nerfed_player,
                                             Game *game, const Move *move,
                                             MachineLetter *rare_word,
                                             int *rare_word_length,
                                             double *min_confidence_out,
                                             double *challenge_ev_out) {
  Board *board = game_get_board(game);
  FormedWords *formed_words = formed_words_create(board, move);
  const int num_words = formed_words_get_num_words(formed_words);
  double min_logplay = 99.0;
  double min_loglit = 99.0;
  double any_absent = 0.0;
  MachineLetter word[BOARD_DIM];
  *rare_word_length = 0;
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const int word_length =
        formed_words_get_word_length(formed_words, word_idx);
    const MachineLetter *letters =
        formed_words_get_word(formed_words, word_idx);
    for (int letter_idx = 0; letter_idx < word_length; letter_idx++) {
      word[letter_idx] = get_unblanked_machine_letter(letters[letter_idx]);
    }
    const NerfedWordFeat *feat =
        nerfed_player_lookup_word(nerfed_player, word, word_length);
    // words absent from the REAL-lexicon table can only reach movegen
    // via the believed (PHALL union) lexicon: the play is visible only
    // to a player whose game-level belief draw includes the word.
    if (feat == NULL && nerfed_player->believed_kwg != NULL &&
        !nerfed_player_believes_word(nerfed_player, word, word_length)) {
      formed_words_destroy(formed_words);
      *rare_word_length = 0;
      if (min_confidence_out) {
        *min_confidence_out = 0.0;
      }
      if (challenge_ev_out) {
        *challenge_ev_out = 0.0;
      }
      return 1.0;
    }
    // A believed phony is treated as a decently playable known word
    // (geometry still gates it; the risk discount handles challenge
    // expectations).
    double logplay = 2.0;
    double loglit = 1.0;
    double absent = 0.0;
    if (feat) {
      logplay = feat->logplay;
      loglit = feat->loglit;
      absent = feat->absent ? 1.0 : 0.0;
    }
    if (logplay < min_logplay) {
      min_logplay = logplay;
      memcpy(rare_word, word, word_length);
      *rare_word_length = word_length;
    }
    if (min_confidence_out) {
      // Own-play perspective: a believed phony (absent from the real
      // table, present only via the player's believed lexicon) carries
      // the believer's confidence, not the outside view of an obscure
      // string — that outside view is for CHALLENGERS of other
      // people's words.
      const double word_confidence =
          (feat == NULL && nerfed_player->believed_kwg != NULL)
              ? NERFED_OWN_BELIEF_CONF_BASE +
                    NERFED_OWN_BELIEF_CONF_SLOPE * nerfed_player->rating_z
              : nerfed_player_word_confidence(nerfed_player, word, word_length);
      if (word_confidence < *min_confidence_out) {
        *min_confidence_out = word_confidence;
      }
    }
    if (loglit < min_loglit) {
      min_loglit = loglit;
    }
    if (absent > any_absent) {
      any_absent = absent;
    }
  }
  formed_words_destroy(formed_words);
  if (num_words == 0) {
    return 0.0;
  }
  const int tiles_played = move_get_tiles_played(move);
  double uses_blank = 0.0;
  const int tiles_length = move_get_tiles_length(move);
  for (int tile_idx = 0; tile_idx < tiles_length; tile_idx++) {
    const MachineLetter tile = move_get_tile(move, tile_idx);
    if (tile != PLAYED_THROUGH_MARKER && get_is_blanked(tile)) {
      uses_blank = 1.0;
    }
  }
  const double through = tiles_length > tiles_played ? 1.0 : 0.0;
  const double len2 = tiles_length <= 2 ? 1.0 : 0.0;
  const double len7plus = tiles_length >= 7 ? 1.0 : 0.0;
  const double rating_z = nerfed_player->rating_z;
  if (challenge_ev_out != NULL) {
    *challenge_ev_out = 0.0;
    double conf = 1.0;
    if (min_confidence_out != NULL) {
      conf = *min_confidence_out;
    }
    if (conf < 1.0) {
      const double subjective_valid = 1.0 - NERFED_KNOW_RISK * (1.0 - conf);
      const double move_points = equity_to_double(move_get_equity(move));
      if (!nerfed_player->challenge_ctx_set) {
        // Legacy risk discount (no challenge flow in this game mode).
        *challenge_ev_out = -(1.0 - subjective_valid) *
                            NERFED_OPP_CHALLENGE_RATE *
                            (move_points + NERFED_TEMPO_VALUE);
      } else {
        // Challenge-bait economics: a confident mover GAINS from a wrong
        // challenge (this is what makes an expert intentionally play
        // obscure valid words), a doubtful mover risks the play plus
        // tempo. The mover's estimate of P(opponent challenges) must
        // match what the opponent actually does: the opponent fires on a
        // hunch (the shared speculative term, at the OPPONENT's rating)
        // OR because it finds the word unfamiliar (the rational
        // familiarity channel). ORing the two, with no ad-hoc engagement
        // fudge, keeps the bait EV consistent with the challenge model.
        const double opp_z = nerfed_player->challenge_ctx_opp_rating_z;
        const bool rule_5pt = nerfed_player->challenge_ctx_rule_5pt;
        const double *cf = nerfed_player->miss_coeffs;
        const int rare_len = *rare_word_length;
        const double len2r = (rare_len > 0 && rare_len <= 2) ? 1.0 : 0.0;
        const double len7r = rare_len >= 7 ? 1.0 : 0.0;
        const double opp_unfamiliar_logit =
            NERFED_KNOW_OFFSET + cf[0] + cf[1] * opp_z + cf[2] * min_logplay +
            cf[3] * min_loglit + cf[4] * any_absent + cf[10] * len2r +
            cf[11] * len7r + cf[12] * opp_z * min_logplay +
            cf[13] * opp_z * min_loglit + cf[15] * opp_z * len2r +
            cf[16] * opp_z * len7r;
        const double p_unfamiliar_opp =
            1.0 / (1.0 + exp(-opp_unfamiliar_logit));
        const double attention = rule_5pt ? NERFED_CHALLENGE_5PT_ATTENTION
                                          : NERFED_CHALLENGE_DOUBLE_ATTENTION;
        const double p_rational = attention * p_unfamiliar_opp;
        const double p_spec =
            nerfed_speculative_challenge_prob(rule_5pt, min_logplay, opp_z);
        const double p_challenge = 1.0 - (1.0 - p_spec) * (1.0 - p_rational);
        const double gain =
            rule_5pt ? NERFED_BAIT_5PT_GAIN_PER_WORD * (double)num_words
                     : NERFED_BAIT_DOUBLE_GAIN;
        *challenge_ev_out =
            p_challenge *
            (subjective_valid * gain -
             (1.0 - subjective_valid) * (move_points + NERFED_TEMPO_VALUE));
      }
    }
  }
  const double features[NERFED_NUM_MISS_COEFFS] = {
      1.0,
      rating_z,
      min_logplay,
      min_loglit,
      any_absent,
      (num_words - 1) / 2.0,
      uses_blank,
      through,
      tiles_played / 7.0,
      0.0,
      len2,
      len7plus,
      rating_z * min_logplay,
      rating_z * min_loglit,
      rating_z * uses_blank,
      rating_z * len2,
      rating_z * len7plus,
  };
  double logit = 0.0;
  for (int coeff_idx = 0; coeff_idx < NERFED_NUM_MISS_COEFFS; coeff_idx++) {
    logit += nerfed_player->miss_coeffs[coeff_idx] * features[coeff_idx];
  }
  // Extra miss for 9+ letter through-plays (see NERFED_LONG9_MISS_PENALTY):
  // a flat term for the easy extensions plus a bridge term counting the
  // interior board tiles the play threads between (the hard ones humans
  // miss). Only at 9+ letters, so calibrated 7/8-letter hooks are untouched.
  if (tiles_length > 8) {
    const double long9 = (double)(tiles_length - 8);
    int first_played = -1;
    int last_played = -1;
    for (int idx = 0; idx < tiles_length; idx++) {
      if (move_get_tile(move, idx) != PLAYED_THROUGH_MARKER) {
        if (first_played < 0) {
          first_played = idx;
        }
        last_played = idx;
      }
    }
    double interior_through = 0.0;
    for (int idx = first_played; idx <= last_played; idx++) {
      if (move_get_tile(move, idx) == PLAYED_THROUGH_MARKER) {
        interior_through += 1.0;
      }
    }
    const double taper =
        fmin(1.0, fmax(NERFED_LONG9_RTG_FLOOR,
                       1.0 - NERFED_LONG9_RTG_TAPER * rating_z));
    logit += taper * (NERFED_LONG9_MISS_PENALTY * long9 +
                      NERFED_LONG9_BRIDGE_PENALTY * interior_through);
  }
  return 1.0 / (1.0 + exp(-logit));
}

// Returns the per-turn shared uniform draw for the given rarest word,
// drawing a fresh one on first sight. Linear scan; a turn rarely has more
// than a few dozen distinct rare words.
static double nerfed_player_word_draw(NerfedPlayer *nerfed_player,
                                      XoshiroPRNG *prng,
                                      const MachineLetter *word,
                                      int word_length) {
  for (int draw_idx = 0; draw_idx < nerfed_player->num_word_draws; draw_idx++) {
    NerfedWordDraw *draw = &nerfed_player->word_draws[draw_idx];
    if (draw->word_length == word_length &&
        memcmp(draw->word, word, word_length) == 0) {
      return draw->uniform;
    }
  }
  const double uniform = nerfed_player_uniform(prng);
  if (nerfed_player->num_word_draws < NERFED_MAX_WORD_DRAWS) {
    NerfedWordDraw *draw =
        &nerfed_player->word_draws[nerfed_player->num_word_draws++];
    memcpy(draw->word, word, word_length);
    draw->word_length = (uint8_t)word_length;
    draw->uniform = uniform;
  }
  return uniform;
}

static void nerfed_player_generate_moves(NerfedPlayer *nerfed_player,
                                         Game *game) {
  MoveList *move_list = nerfed_player->move_list;
  move_list_reset(move_list);
  const MoveGenArgs args = {.game = game,
                            .move_list = move_list,
                            .move_record_type = MOVE_RECORD_ALL,
                            .move_sort_type = MOVE_SORT_EQUITY,
                            .override_kwg = NULL,
                            .eq_margin_movegen = 0,
                            .target_equity = EQUITY_MAX_VALUE,
                            .target_leave_size_for_exchange_cutoff =
                                UNSET_LEAVE_SIZE};
  generate_moves(&args);
  nerfed_player->num_word_draws = 0;
}

// Rolls the corpus exchange-propensity model over the generated move list
// and, when it fires, selects the exchange with the noisy keep model.
// Returns NULL when the player keeps their rack (or has no exchange).
static const Move *nerfed_player_maybe_exchange(NerfedPlayer *nerfed_player,
                                                const Game *game,
                                                XoshiroPRNG *prng) {
  const MoveList *move_list = nerfed_player->move_list;
  const int move_count = move_list_get_count(move_list);
  // Exchange decision (corpus propensity model): compare the best tile
  // play against the best exchange and roll P(exchange | margin, rating).
  // On an exchange, the keep is selected among exchange options with the
  // same Gumbel valuation noise as plays (weak players keep weaker leaves).
  bool has_play = false;
  bool has_exchange = false;
  double best_play_equity = 0.0;
  double best_exchange_equity = NERFED_NO_EXCHANGE_EQUITY;
  for (int move_idx = 0; move_idx < move_count; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    const Equity move_equity = move_get_equity(move);
    if (move_equity == EQUITY_PASS_VALUE) {
      continue;
    }
    const double equity = equity_to_double(move_equity);
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (!has_play || equity > best_play_equity) {
        has_play = true;
        best_play_equity = equity;
      }
    } else if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
      if (!has_exchange || equity > best_exchange_equity) {
        has_exchange = true;
        best_exchange_equity = equity;
      }
    }
  }
  if (has_exchange) {
    double delta = (has_play ? best_play_equity : NERFED_NO_EXCHANGE_EQUITY) -
                   best_exchange_equity;
    if (delta > NERFED_EXCH_DELTA_CAP) {
      delta = NERFED_EXCH_DELTA_CAP;
    }
    const double delta10 = delta / 10.0;
    const double rating_z = nerfed_player->rating_z;
    // Rack texture: bad racks get exchanged beyond what the margin says.
    const Rack *rack = player_get_rack(
        game_get_player(game, game_get_player_on_turn_index(game)));
    int num_vowels = 0;
    for (int vowel_idx = 0; vowel_idx < 5; vowel_idx++) {
      num_vowels += rack_get_letter(rack, nerfed_player->vowel_mls[vowel_idx]);
    }
    int num_duplicates = 0;
    const int dist_size = rack_get_dist_size(rack);
    for (int ml = 0; ml < dist_size; ml++) {
      const int letter_count = rack_get_letter(rack, (MachineLetter)ml);
      if (letter_count > 1) {
        num_duplicates += letter_count - 1;
      }
    }
    const double vowel_dev = fabs((double)num_vowels - 3.0);
    const double q_no_u = (rack_get_letter(rack, nerfed_player->q_ml) > 0 &&
                           rack_get_letter(rack, nerfed_player->u_ml) == 0)
                              ? 1.0
                              : 0.0;
    const double has_blank =
        rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0 ? 1.0 : 0.0;
    const double exchange_logit =
        NERFED_EXCH_COEFFS[0] + NERFED_EXCH_COEFFS[1] * delta10 +
        NERFED_EXCH_COEFFS[2] * rating_z +
        NERFED_EXCH_COEFFS[3] * delta10 * rating_z +
        NERFED_EXCH_COEFFS[4] * vowel_dev +
        NERFED_EXCH_COEFFS[5] * num_duplicates +
        NERFED_EXCH_COEFFS[6] * q_no_u + NERFED_EXCH_COEFFS[7] * has_blank;
    const double exchange_probability = 1.0 / (1.0 + exp(-exchange_logit));
    if (nerfed_player_uniform(prng) < exchange_probability) {
      const Move *chosen_exchange = NULL;
      double chosen_exchange_value = 0.0;
      for (int move_idx = 0; move_idx < move_count; move_idx++) {
        const Move *move = move_list_get_move(move_list, move_idx);
        if (move_get_type(move) != GAME_EVENT_EXCHANGE) {
          continue;
        }
        const double uniform = nerfed_player_uniform(prng);
        const double gumbel_noise =
            -nerfed_player->keep_sigma * log(-log(uniform));
        // Keep-choice model: noisy leave equity plus the fitted throw
        // bias (humans throw more tiles than pure leave value says).
        const double value =
            equity_to_double(move_get_equity(move)) +
            NERFED_KEEP_THROW_BIAS * move_get_tiles_played(move) + gumbel_noise;
        if (chosen_exchange == NULL || value > chosen_exchange_value) {
          chosen_exchange = move;
          chosen_exchange_value = value;
        }
      }
      return chosen_exchange;
    }
  }
  return NULL;
}

const Move *nerfed_player_select_move(NerfedPlayer *nerfed_player, Game *game,
                                      XoshiroPRNG *prng) {
  nerfed_player_generate_moves(nerfed_player, game);
  const Move *exchange =
      nerfed_player_maybe_exchange(nerfed_player, game, prng);
  if (exchange != NULL) {
    return exchange;
  }
  const MoveList *move_list = nerfed_player->move_list;
  const int move_count = move_list_get_count(move_list);
  const Move *chosen = NULL;
  double chosen_value = 0.0;
  const Move *fallback = NULL;
  Equity fallback_equity = 0;
  for (int move_idx = 0; move_idx < move_count; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    double move_confidence = 1.0;
    if (fallback == NULL || move_get_equity(move) > fallback_equity) {
      fallback = move;
      fallback_equity = move_get_equity(move);
    }
    if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
      // The exchange decision was already rolled (and declined) above.
      continue;
    }
    double challenge_ev = 0.0;
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      MachineLetter rare_word[BOARD_DIM];
      int rare_word_length = 0;
      move_confidence = 1.0;
      const double miss_probability = nerfed_player_miss_probability(
          nerfed_player, game, move, rare_word, &rare_word_length,
          &move_confidence, &challenge_ev);
      // Plays sharing their rarest word share one visibility draw so a
      // word the player does not know is missed at every placement.
      const double uniform =
          rare_word_length > 0
              ? nerfed_player_word_draw(nerfed_player, prng, rare_word,
                                        rare_word_length)
              : nerfed_player_uniform(prng);
      if (uniform < miss_probability) {
        continue;
      }
    }
    const double uniform = nerfed_player_uniform(prng);
    const double gumbel_noise = -nerfed_player->sigma * log(-log(uniform));
    // A pass carries the EQUITY_PASS_VALUE sentinel; value it like the
    // sim/static displays do (-1000) so it is only chosen as a last resort.
    const Equity move_equity = move_get_equity(move);
    double base_value = (move_equity == EQUITY_PASS_VALUE)
                            ? -1000.0
                            : equity_to_double(move_equity);
    // challenge economics: risk of being challenged off vs the bait
    // value of eliciting a wrong challenge (computed alongside the
    // visibility features).
    if (move_equity != EQUITY_PASS_VALUE) {
      base_value += challenge_ev;
    }
    const double value = base_value + gumbel_noise;
    if (chosen == NULL || value > chosen_value) {
      chosen = move;
      chosen_value = value;
    }
  }
  if (chosen == NULL) {
    // Every play was missed and no pass/exchange was generated; play the
    // best move rather than forfeiting the turn.
    chosen = fallback;
  }
  return chosen;
}

const Move *nerfed_player_prepare_sim_arms(NerfedPlayer *nerfed_player,
                                           Game *game, MoveList *arms,
                                           XoshiroPRNG *prng) {
  nerfed_player_generate_moves(nerfed_player, game);
  MoveList *move_list = nerfed_player->move_list;
  // Sort the generated heap so arms are taken best-static-equity first.
  move_list_sort_moves(move_list);
  const Move *exchange =
      nerfed_player_maybe_exchange(nerfed_player, game, prng);
  if (exchange != NULL) {
    return exchange;
  }
  const int move_count = move_list_get_count(move_list);
  const int arm_capacity = move_list_get_capacity(arms);
  move_list_reset(arms);
  nerfed_player->num_sim_survivors = 0;
  for (int move_idx = 0; move_idx < move_count; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
      continue;
    }
    MachineLetter rare_word[BOARD_DIM];
    int rare_word_length = 0;
    double move_confidence = 1.0;
    double challenge_ev = 0.0;
    const double miss_probability = nerfed_player_miss_probability(
        nerfed_player, game, move, rare_word, &rare_word_length,
        &move_confidence, &challenge_ev);
    // Plays sharing their rarest word share one visibility draw so a
    // word the player does not know is missed at every placement.
    const double uniform =
        rare_word_length > 0
            ? nerfed_player_word_draw(nerfed_player, prng, rare_word,
                                      rare_word_length)
            : nerfed_player_uniform(prng);
    if (uniform < miss_probability) {
      continue;
    }
    const int survivor_idx = nerfed_player->num_sim_survivors;
    nerfed_player->sim_survivor_indices[survivor_idx] = move_idx;
    nerfed_player->sim_survivor_confidence[survivor_idx] = move_confidence;
    nerfed_player->sim_survivor_challenge_ev[survivor_idx] = challenge_ev;
    nerfed_player->num_sim_survivors++;
    if (nerfed_player->num_sim_survivors >= NERFED_MOVE_LIST_CAPACITY) {
      break;
    }
  }
  const int num_survivors = nerfed_player->num_sim_survivors;
  if (num_survivors == 0) {
    // Every play was missed; play the best static move rather than
    // forfeiting the turn (the move list is sorted, so index 0 is best).
    return move_list_get_move(move_list, 0);
  }
  if (num_survivors == 1) {
    return move_list_get_move(move_list,
                              nerfed_player->sim_survivor_indices[0]);
  }
  // Stratified arm selection: the top half of the arm budget takes the
  // best static survivors (including arm 0, the glimpse baseline); the
  // rest are geometrically jittered strata over the remaining
  // survivors. Blocking and setup plays are often statically mediocre
  // (they sacrifice points), so head-only arms can never PROMOTE a
  // sleeper — tiered coverage lets the sim discover them.
  int num_arms_target = arm_capacity;
  if (num_arms_target > num_survivors) {
    num_arms_target = num_survivors;
  }
  if (num_arms_target > NERFED_MAX_SIM_ARMS) {
    num_arms_target = NERFED_MAX_SIM_ARMS;
  }
  nerfed_player->num_sim_arms = 0;
  if (num_arms_target >= num_survivors) {
    for (int survivor_idx = 0; survivor_idx < num_survivors; survivor_idx++) {
      nerfed_player->sim_arm_survivor_idx[survivor_idx] = survivor_idx;
    }
    nerfed_player->num_sim_arms = num_survivors;
  } else {
    const int head_count = (num_arms_target + 1) / 2;
    for (int arm_idx = 0; arm_idx < head_count; arm_idx++) {
      nerfed_player->sim_arm_survivor_idx[arm_idx] = arm_idx;
    }
    nerfed_player->num_sim_arms = head_count;
    const int tail_count = num_arms_target - head_count;
    const double log_span = log((double)num_survivors / (double)head_count);
    int prev_survivor_idx = head_count - 1;
    for (int tail_idx = 0; tail_idx < tail_count; tail_idx++) {
      const double stratum =
          ((double)tail_idx + nerfed_player_uniform(prng)) / (double)tail_count;
      int survivor_idx = (int)((double)head_count * exp(stratum * log_span));
      if (survivor_idx <= prev_survivor_idx) {
        survivor_idx = prev_survivor_idx + 1;
      }
      if (survivor_idx >= num_survivors) {
        break;
      }
      nerfed_player->sim_arm_survivor_idx[nerfed_player->num_sim_arms++] =
          survivor_idx;
      prev_survivor_idx = survivor_idx;
    }
  }
  move_list_reset(arms);
  for (int arm_idx = 0; arm_idx < nerfed_player->num_sim_arms; arm_idx++) {
    move_list_add_move(
        arms,
        move_list_get_move(move_list,
                           nerfed_player->sim_survivor_indices
                               [nerfed_player->sim_arm_survivor_idx[arm_idx]]));
  }
  // Arms were added in ascending survivor order (= descending static
  // equity), so sorting restores the same order for the sim.
  move_list_sort_moves(arms);
  return NULL;
}

// Sim-implied points advantage of arm play_idx relative to arm 0 (the
// best static survivor), minus the static advantage — i.e. how much the
// sim DISAGREES with static valuation, in points. Returns 0.0 (no
// glimpse) for unsampled arms.
static double nerfed_player_sim_disagreement(
    const SimResults *sim_results, int play_idx, double baseline_utility,
    double baseline_static_points, double static_points,
    double utility_w_winpct, double utility_w_spread,
    double utility_spread_scale) {
  const SimmedPlay *simmed_play =
      sim_results_get_simmed_play(sim_results, play_idx);
  const uint64_t num_samples =
      stat_get_num_samples(simmed_play_get_win_pct_stat(simmed_play));
  if (num_samples == 0) {
    return 0.0;
  }
  const double win_mean =
      stat_get_mean(simmed_play_get_win_pct_stat(simmed_play));
  const double spread_mean =
      stat_get_mean(simmed_play_get_equity_stat(simmed_play));
  const double utility = sim_utility_blend(
      win_mean, double_to_equity(spread_mean), utility_w_winpct,
      utility_w_spread, utility_spread_scale);
  const double sim_advantage_points =
      (utility - baseline_utility) / NERFED_SIM_UTILITY_PER_POINT;
  const double shrink =
      (double)num_samples / ((double)num_samples + NERFED_SIM_SHRINK_N0);
  return shrink *
         (sim_advantage_points - (static_points - baseline_static_points));
}

const Move *nerfed_player_pick_simmed_move(NerfedPlayer *nerfed_player,
                                           const SimResults *sim_results,
                                           double utility_w_winpct,
                                           double utility_w_spread,
                                           double utility_spread_scale,
                                           XoshiroPRNG *prng) {
  const int num_arms = sim_results_get_number_of_plays(sim_results);
  const MoveList *move_list = nerfed_player->move_list;
  // Baseline for centering the glimpse term: arm 0, the best static
  // survivor, which always has samples once the sim has run.
  const Move *baseline_move =
      move_list_get_move(move_list, nerfed_player->sim_survivor_indices[0]);
  const double baseline_static_points =
      equity_to_double(move_get_equity(baseline_move));
  const SimmedPlay *baseline_play = sim_results_get_simmed_play(sim_results, 0);
  const double baseline_utility = sim_utility_blend(
      stat_get_mean(simmed_play_get_win_pct_stat(baseline_play)),
      double_to_equity(
          stat_get_mean(simmed_play_get_equity_stat(baseline_play))),
      utility_w_winpct, utility_w_spread, utility_spread_scale);
  const Move *chosen = NULL;
  double chosen_value = 0.0;
  int arm_cursor = 0;
  for (int survivor_idx = 0; survivor_idx < nerfed_player->num_sim_survivors;
       survivor_idx++) {
    const Move *move = move_list_get_move(
        move_list, nerfed_player->sim_survivor_indices[survivor_idx]);
    const double static_points = equity_to_double(move_get_equity(move));
    double value = static_points;
    // Glimpse of truth: simmed arms feel a fraction of the sim's
    // disagreement with static valuation; non-simmed survivors keep
    // pure static values, preserving the fitted choice dispersion over
    // the full visible move list (at w = 0 this pick reduces to the
    // static path). Arms and survivors are both in descending static
    // equity order, so a single cursor aligns them.
    if (arm_cursor < nerfed_player->num_sim_arms && arm_cursor < num_arms &&
        nerfed_player->sim_arm_survivor_idx[arm_cursor] == survivor_idx) {
      value += NERFED_SIM_GLIMPSE_W *
               nerfed_player_sim_disagreement(
                   sim_results, arm_cursor, baseline_utility,
                   baseline_static_points, static_points, utility_w_winpct,
                   utility_w_spread, utility_spread_scale);
      arm_cursor++;
    }
    // challenge economics, as in the static path (risk of being
    // challenged off vs the bait value of eliciting a wrong challenge).
    value += nerfed_player->sim_survivor_challenge_ev[survivor_idx];
    const double uniform = nerfed_player_uniform(prng);
    const double gumbel_noise =
        -nerfed_player->sigma * NERFED_SIM_SIGMA_SCALE * log(-log(uniform));
    value += gumbel_noise;
    if (chosen == NULL || value > chosen_value) {
      chosen = move;
      chosen_value = value;
    }
  }
  return chosen;
}

// splitmix64 finalizer for the deterministic word-knowledge draw.
static uint64_t nerfed_player_word_hash(const MachineLetter *word,
                                        int word_length, uint64_t seed) {
  uint64_t hash = seed ^ 0x9E3779B97F4A7C15ULL;
  for (int letter_idx = 0; letter_idx < word_length; letter_idx++) {
    hash ^= word[letter_idx];
    hash *= 0xBF58476D1CE4E5B9ULL;
  }
  hash ^= hash >> 30;
  hash *= 0x94D049BB133111EBULL;
  hash ^= hash >> 31;
  return hash;
}

bool nerfed_player_knows_word(const NerfedPlayer *nerfed_player,
                              const MachineLetter *word, int word_length,
                              uint64_t seed) {
  const NerfedWordFeat *feat =
      nerfed_player_lookup_word(nerfed_player, word, word_length);
  double logplay = NERFED_DEFAULT_LOGPLAY;
  double loglit = NERFED_DEFAULT_LOGLIT;
  double absent = 1.0;
  if (feat) {
    logplay = feat->logplay;
    loglit = feat->loglit;
    absent = feat->absent ? 1.0 : 0.0;
  }
  const double len2 = word_length <= 2 ? 1.0 : 0.0;
  const double len7plus = word_length >= 7 ? 1.0 : 0.0;
  const double rating_z = nerfed_player->rating_z;
  const double *c = nerfed_player->miss_coeffs;
  // knowledge-only subset of the miss model: geometry terms zeroed.
  const double logit = NERFED_KNOW_OFFSET + c[0] + c[1] * rating_z +
                       c[2] * logplay + c[3] * loglit + c[4] * absent +
                       c[10] * len2 + c[11] * len7plus +
                       c[12] * rating_z * logplay + c[13] * rating_z * loglit +
                       c[15] * rating_z * len2 + c[16] * rating_z * len7plus;
  const double miss_probability = 1.0 / (1.0 + exp(-logit));
  const uint64_t hash = nerfed_player_word_hash(word, word_length, seed);
  const double uniform = ((double)(hash >> 11) + 0.5) / 9007199254740992.0;
  return uniform >= miss_probability;
}

// Subjective probability that a word is valid, capped away from 0/1.
// The caps shrink to zero as playability (scaled by rating) or literacy
// saturate: everyone is CERTAIN about CAT; experts are certain about QI.
double nerfed_player_word_confidence(const NerfedPlayer *nerfed_player,
                                     const MachineLetter *word,
                                     int word_length) {
  const NerfedWordReveal *reveal =
      nerfed_player_lookup_reveal(nerfed_player, word, word_length);
  if (reveal != NULL && reveal->verdict > 0) {
    return 0.98;
  }
  if (reveal != NULL && reveal->verdict < 0) {
    return 0.02;
  }
  const NerfedWordFeat *feat =
      nerfed_player_lookup_word(nerfed_player, word, word_length);
  double logplay = NERFED_DEFAULT_LOGPLAY;
  double loglit = NERFED_DEFAULT_LOGLIT;
  double absent = 1.0;
  if (feat) {
    logplay = feat->logplay;
    loglit = feat->loglit;
    absent = feat->absent ? 1.0 : 0.0;
  }
  const double rating_z = nerfed_player->rating_z;
  const double *c = nerfed_player->miss_coeffs;
  const double len2 = word_length <= 2 ? 1.0 : 0.0;
  const double len7plus = word_length >= 7 ? 1.0 : 0.0;
  const double logit = NERFED_KNOW_OFFSET + c[0] + c[1] * rating_z +
                       c[2] * logplay + c[3] * loglit + c[4] * absent +
                       c[10] * len2 + c[11] * len7plus +
                       c[12] * rating_z * logplay + c[13] * rating_z * loglit +
                       c[15] * rating_z * len2 + c[16] * rating_z * len7plus;
  double confidence = 1.0 - 1.0 / (1.0 + exp(-logit));
  // certainty release: saturated playability (rating-scaled) or literacy
  const double sat_play =
      1.0 / (1.0 + exp(-((logplay - 2.0) + 0.6 * rating_z) / 0.4));
  const double sat_lit = 1.0 / (1.0 + exp(-(loglit - 2.2) / 0.3));
  const double sat = sat_play > sat_lit ? sat_play : sat_lit;
  const double cap = NERFED_KNOW_CAP * (1.0 - sat);
  if (confidence < cap) {
    confidence = cap;
  }
  if (confidence > 1.0 - cap) {
    confidence = 1.0 - cap;
  }
  return confidence;
}

void nerfed_player_filter_word_list(const NerfedPlayer *nerfed_player,
                                    const DictionaryWordList *word_list,
                                    DictionaryWordList *filtered_list,
                                    uint64_t seed) {
  const int word_count = dictionary_word_list_get_count(word_list);
  for (int word_idx = 0; word_idx < word_count; word_idx++) {
    const DictionaryWord *dictionary_word =
        dictionary_word_list_get_word(word_list, word_idx);
    const MachineLetter *word = dictionary_word_get_word(dictionary_word);
    const int word_length = dictionary_word_get_length(dictionary_word);
    if (nerfed_player_knows_word(nerfed_player, word, word_length, seed)) {
      dictionary_word_list_add_word(filtered_list, word, word_length);
    }
  }
}

void nerfed_player_pick_endgame_pv(const NerfedPlayer *nerfed_player,
                                   const Game *game,
                                   EndgameResults *endgame_results,
                                   uint64_t seed) {
  const int num_pvs = endgame_results_get_num_pvs(endgame_results);
  if (num_pvs <= 1) {
    return;
  }
  const double rating_z = nerfed_player->rating_z;
  PVLine *pvs_for_gap = endgame_results_get_multi_pvs(endgame_results);
  const int own_rack_size = rack_get_total_letters(player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game))));
  const int opp_rack_size = rack_get_total_letters(player_get_rack(
      game_get_player(game, 1 - game_get_player_on_turn_index(game))));
  const double tiles_rem = own_rack_size + opp_rack_size;
  double gap12 = 0.0;
  if (num_pvs > 1) {
    gap12 = (double)(pvs_for_gap[0].score - pvs_for_gap[1].score);
    if (gap12 > 30.0) {
      gap12 = 30.0;
    }
  }
  const double hard_logit =
      NERFED_ENDGAME_HARD_BIAS +
      NERFED_ENDGAME_HARD_TILES * ((tiles_rem - 8.0) / 3.0) +
      NERFED_ENDGAME_HARD_GAP * (gap12 / 10.0) +
      NERFED_ENDGAME_HARD_NMOVES * (((double)num_pvs - 30.0) / 15.0);
  const double hard = hard_logit > 0.0 ? 1.0 : 0.0;
  const double sigma =
      exp(NERFED_ENDGAME_SIGMA_C0 + NERFED_ENDGAME_SIGMA_C1 * rating_z +
          NERFED_ENDGAME_SIGMA_HARD * hard +
          NERFED_ENDGAME_SIGMA_HARD_RTG * hard * rating_z);
  const double score_pref =
      NERFED_ENDGAME_SCORE_PREF_C0 + NERFED_ENDGAME_SCORE_PREF_C1 * rating_z;
  const double outplay_pref = NERFED_ENDGAME_OUTPLAY_PREF_C0 +
                              NERFED_ENDGAME_OUTPLAY_PREF_C1 * rating_z;
  const int rack_size = own_rack_size;
  PVLine *multi_pvs = pvs_for_gap;
  int chosen_idx = 0;
  double chosen_value = 0.0;
  for (int pv_idx = 0; pv_idx < num_pvs; pv_idx++) {
    // deterministic per-(seed, pv) uniform via the word-hash finalizer
    const MachineLetter idx_bytes[2] = {(MachineLetter)(pv_idx + 1),
                                        (MachineLetter)(pv_idx >> 7)};
    const uint64_t hash = nerfed_player_word_hash(idx_bytes, 2, seed);
    const double uniform = ((double)(hash >> 11) + 0.5) / 9007199254740992.0;
    const double gumbel_noise = -log(-log(uniform));
    const SmallMove *root_move = &multi_pvs[pv_idx].moves[0];
    const double outplay =
        small_move_get_tiles_played(root_move) >= rack_size ? 1.0 : 0.0;
    // utility units: value/sigma + fitted preferences + standard Gumbel
    const double value =
        (double)multi_pvs[pv_idx].score / sigma +
        score_pref * ((double)small_move_get_score(root_move) / 10.0) +
        outplay_pref * outplay + gumbel_noise;
    if (pv_idx == 0 || value > chosen_value) {
      chosen_idx = pv_idx;
      chosen_value = value;
    }
  }
  if (chosen_idx != 0) {
    const PVLine chosen_pv = multi_pvs[chosen_idx];
    multi_pvs[chosen_idx] = multi_pvs[0];
    multi_pvs[0] = chosen_pv;
  }
  endgame_results_force_best_pvline(endgame_results, &multi_pvs[0],
                                    multi_pvs[0].score);
}

void nerfed_player_start_game(NerfedPlayer *nerfed_player, uint64_t game_seed) {
  nerfed_player->num_reveals = 0;
  nerfed_player->game_seed = game_seed;
  nerfed_player->turn_number = 0;
}

void nerfed_player_set_turn(NerfedPlayer *nerfed_player, int turn_number) {
  nerfed_player->turn_number = turn_number;
}

// Game-level word knowledge with a small per-turn flip. The base draw is
// deterministic per (game_seed, word) so knowledge is persistent within a
// game and hidden from the opponent (their draws use their own seed).
void nerfed_player_set_believed_kwg(NerfedPlayer *nerfed_player,
                                    const KWG *kwg) {
  nerfed_player->believed_kwg = kwg;
}

void nerfed_player_set_challenge_context(NerfedPlayer *nerfed_player,
                                         bool rule_5pt,
                                         double opponent_rating_z) {
  nerfed_player->challenge_ctx_set = true;
  nerfed_player->challenge_ctx_rule_5pt = rule_5pt;
  nerfed_player->challenge_ctx_opp_rating_z = opponent_rating_z;
}

void nerfed_player_reveal_word(NerfedPlayer *nerfed_player,
                               const MachineLetter *word, int word_length,
                               int verdict) {
  NerfedWordReveal *existing = (NerfedWordReveal *)nerfed_player_lookup_reveal(
      nerfed_player, word, word_length);
  if (existing != NULL) {
    // A definitive verdict overrides an earlier suspect marking.
    if (verdict != 0) {
      existing->verdict = (int8_t)verdict;
    }
    return;
  }
  if (nerfed_player->num_reveals >= NERFED_MAX_REVEALS) {
    return;
  }
  NerfedWordReveal *reveal =
      &nerfed_player->reveals[nerfed_player->num_reveals++];
  memcpy(reveal->word, word, word_length);
  reveal->word_length = (uint8_t)word_length;
  reveal->verdict = (int8_t)verdict;
}

bool nerfed_player_believes_word(const NerfedPlayer *nerfed_player,
                                 const MachineLetter *word, int word_length) {
  const NerfedWordReveal *reveal =
      nerfed_player_lookup_reveal(nerfed_player, word, word_length);
  if (reveal != NULL && reveal->verdict > 0) {
    return true;
  }
  if (reveal != NULL && reveal->verdict < 0) {
    // A challenge proved this word phony; it is never attempted (or
    // challenged) again this game.
    return false;
  }
  // Word knowledge is re-realized EACH TURN, never fixed for the whole
  // game (a word missed one turn can be recalled the next; a marginal
  // phony can look real one turn and not the next). The only persistent
  // knowledge is the reveals above, from challenge adjudication. A
  // per-turn seed drives the draw so it is consistent within a turn
  // (all decisions this turn agree) but varies across turns.
  const uint64_t turn_seed =
      nerfed_player->game_seed ^
      (0xA24BAED4963EE407ULL * (uint64_t)(nerfed_player->turn_number + 1));
  const bool is_real_word =
      nerfed_player_lookup_word(nerfed_player, word, word_length) != NULL;
  if (nerfed_player->believed_kwg != NULL && !is_real_word) {
    // A word outside the real lexicon: per-turn belief draw at the
    // word's prior. Head phonies (TE) are believed by almost everyone;
    // the idiosyncratic tail splits, which makes phony challenges
    // possible at matched ratings.
    const NerfedPhonyBelief *belief =
        nerfed_player_lookup_belief(nerfed_player, word, word_length);
    double belief_logit = NERFED_PHONY_BELIEF_UNKNOWN_LOGIT;
    if (belief != NULL) {
      const bool is_csw_family =
          nerfed_player->miss_coeffs == NERFED_MISS_COEFFS_CSW;
      const double slope = belief->xfam    ? NERFED_PHONY_BELIEF_XFAM_SLOPE
                           : is_csw_family ? NERFED_PHONY_BELIEF_SLOPE_CSW
                                           : NERFED_PHONY_BELIEF_SLOPE;
      belief_logit = belief->b0 + NERFED_PHONY_BELIEF_OFFSET +
                     (nerfed_player->miss_coeffs == NERFED_MISS_COEFFS_CSW
                          ? NERFED_PHONY_BELIEF_CSW_EXTRA
                          : NERFED_PHONY_BELIEF_TWL_EXTRA) +
                     (belief->xfam ? NERFED_PHONY_BELIEF_XFAM_OFFSET : 0.0) +
                     slope * nerfed_player->rating_z;
    }
    if (reveal != NULL && reveal->verdict == 0) {
      // One word of a challenged-off play was phony; this might be it.
      belief_logit -= 2.0;
    }
    const double p_believe = 1.0 / (1.0 + exp(-belief_logit));
    const uint64_t belief_hash = nerfed_player_word_hash(
        word, word_length, turn_seed ^ 0x5851F42D4C957F2DULL);
    const double belief_uniform =
        ((double)(belief_hash >> 11) + 0.5) / 9007199254740992.0;
    return belief_uniform < p_believe;
  }
  return nerfed_player_knows_word(nerfed_player, word, word_length, turn_seed);
}

static double nerfed_player_win_utility(double margin) {
  return 1.0 / (1.0 + exp(-margin / NERFED_WIN_SIGMOID_SCALE)) +
         NERFED_SPREAD_UTIL * margin;
}

// Static-eval challenge decision with three outcomes valued through the
// win sigmoid of the post-outcome score margin:
//   accept:            margin stands (their play scored)
//   challenge+invalid: play comes off (score reverts, tempo gained)
//   challenge+valid:   penalty (5 pts/word to them, or my lost turn)
// P(invalid) comes from the challenger's subjective confidence over the
// formed words (game-level knowledge; caps keep certainty away from 0/1
// except saturated-playability/literacy words). Desperation challenges
// emerge when losing: accept ~ certain loss while challenging has
// variance.
// Would-be playability of a word of this length: the pseudo-playability
// signal ("if this were real, how often would it come up?"). Short
// unknown words are damning -- a decent player knows every common-shaped
// word -- while long unknowns carry little evidence.
static double nerfed_player_pseudo_logplay(int word_length) {
  switch (word_length) {
  case 2:
    return 3.5;
  case 3:
    return 2.5;
  case 4:
    return 1.8;
  case 5:
    return 1.2;
  case 6:
    return 0.8;
  case 7:
    return 0.5;
  default:
    return 0.3;
  }
}

void nerfed_player_challenge_assess(const NerfedPlayer *nerfed_player,
                                    const NerfedPlayer *opponent, Game *game,
                                    const Move *move, bool rule_5pt,
                                    XoshiroPRNG *prng,
                                    NerfedChallengeAssessment *assessment) {
  memset(assessment, 0, sizeof(*assessment));
  const double challenger_z = nerfed_player->rating_z;
  Board *board = game_get_board(game);
  FormedWords *formed_words = formed_words_create(board, move);
  const int num_words = formed_words_get_num_words(formed_words);
  // Rarest formed word's playability -> how obscure/suspicious the play
  // looks. Computed BEFORE the attention gate so a blatant (floor-
  // obscure) play draws near-certain attention from a strong challenger
  // while ordinary plays keep the base engagement rate.
  double min_logplay = 99.0;
  MachineLetter word[BOARD_DIM];
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const int word_length =
        formed_words_get_word_length(formed_words, word_idx);
    if (word_length < 2) {
      continue;
    }
    const MachineLetter *letters =
        formed_words_get_word(formed_words, word_idx);
    for (int letter_idx = 0; letter_idx < word_length; letter_idx++) {
      word[letter_idx] = get_unblanked_machine_letter(letters[letter_idx]);
    }
    const NerfedWordFeat *feat =
        nerfed_player_lookup_word(nerfed_player, word, word_length);
    const double logplay = feat ? feat->logplay : NERFED_DEFAULT_LOGPLAY;
    if (logplay < min_logplay) {
      min_logplay = logplay;
    }
  }
  const double obscurity = nerfed_obscurity(min_logplay);
  const double attention =
      nerfed_attention_prob(rule_5pt, obscurity, challenger_z);
  if (nerfed_player_uniform(prng) > attention) {
    formed_words_destroy(formed_words);
    assessment->attended = false;
    return;
  }
  assessment->attended = true;
  const double opponent_rating_z =
      (opponent != NULL) ? opponent->rating_z : NERFED_UNNERFED_OPP_RATING_Z;
  // Base prior: boosted for weak challengers (they see phonies
  // everywhere), unchanged at/above average so the calibrated
  // matched-rating rates for strong players are preserved.
  const double base_prior =
      NERFED_OPP_PHONY_PRIOR *
      exp(-NERFED_OPP_PHONY_PRIOR_CHALLENGER_RTG * fmin(challenger_z, 0.0));
  // Opponent-strength tracking: ~0 for beginners (opponent-blind), ~1 for
  // experts (credit a strong opponent's obscure word).
  const double opponent_tracking = 1.0 / (1.0 + exp(-challenger_z));
  double phony_prior = base_prior * exp(-NERFED_OPP_PHONY_PRIOR_RTG *
                                        opponent_tracking * opponent_rating_z);
  if (phony_prior < NERFED_OPP_PHONY_PRIOR_MIN) {
    phony_prior = NERFED_OPP_PHONY_PRIOR_MIN;
  }
  if (phony_prior > NERFED_OPP_PHONY_PRIOR_MAX) {
    phony_prior = NERFED_OPP_PHONY_PRIOR_MAX;
  }
  double p_invalid = 0.0;
  // Second pass over the formed words: the phony posterior. (min_logplay
  // and the attention gate above already used the first pass.)
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const int word_length =
        formed_words_get_word_length(formed_words, word_idx);
    const MachineLetter *letters =
        formed_words_get_word(formed_words, word_idx);
    for (int letter_idx = 0; letter_idx < word_length; letter_idx++) {
      word[letter_idx] = get_unblanked_machine_letter(letters[letter_idx]);
    }
    // a currently-believed word contributes no suspicion
    if (nerfed_player_believes_word(nerfed_player, word, word_length)) {
      continue;
    }
    const double word_confidence =
        nerfed_player_word_confidence(nerfed_player, word, word_length);
    // P(I would not know a word of this shape | it is valid): the
    // knowledge model at the word's pseudo-playability. Tiny for short
    // unknowns (everyone knows the twos) -> large phony posterior; large
    // for long obscure words -> unfamiliarity is uninformative.
    const double pseudo_logplay = nerfed_player_pseudo_logplay(word_length);
    const double rating_z = nerfed_player->rating_z;
    const double *c = nerfed_player->miss_coeffs;
    const double len2 = word_length <= 2 ? 1.0 : 0.0;
    const double len7plus = word_length >= 7 ? 1.0 : 0.0;
    const double miss_logit =
        NERFED_COVERAGE_OFFSET + NERFED_COVERAGE_OFFSET_RTG * rating_z +
        (rule_5pt ? NERFED_COVERAGE_5PT_BONUS +
                        NERFED_COVERAGE_5PT_BONUS_RTG * rating_z
                  : NERFED_COVERAGE_DOUBLE_BONUS) +
        NERFED_KNOW_OFFSET + c[0] + c[1] * rating_z + c[2] * pseudo_logplay +
        c[10] * len2 + c[11] * len7plus + c[12] * rating_z * pseudo_logplay +
        c[15] * rating_z * len2 + c[16] * rating_z * len7plus;
    double p_unfamiliar_given_valid = 1.0 / (1.0 + exp(-miss_logit));
    if (p_unfamiliar_given_valid < 0.02) {
      p_unfamiliar_given_valid = 0.02;
    }
    if (p_unfamiliar_given_valid > 0.9) {
      p_unfamiliar_given_valid = 0.9;
    }
    const double numer = phony_prior * (1.0 - word_confidence);
    const double word_posterior =
        numer / (numer + (1.0 - phony_prior) * p_unfamiliar_given_valid);
    if (word_posterior > p_invalid) {
      p_invalid = word_posterior;
    }
  }
  formed_words_destroy(formed_words);
  // challenger's margin after their opponent's play commits
  const int my_index = 1 - game_get_player_on_turn_index(game);
  const double play_score = equity_to_double(move_get_score(move));
  const double margin_after =
      (double)(equity_to_int(
                   player_get_score(game_get_player(game, my_index))) -
               equity_to_int(
                   player_get_score(game_get_player(game, 1 - my_index)))) -
      play_score;
  const double u_accept = nerfed_player_win_utility(margin_after);
  const double u_off =
      nerfed_player_win_utility(margin_after + play_score + NERFED_TEMPO_VALUE);
  const double u_penalty =
      rule_5pt ? nerfed_player_win_utility(margin_after - 5.0 * num_words)
               : nerfed_player_win_utility(
                     margin_after -
                     NERFED_CHALLENGE_DOUBLE_TEMPO_FACTOR * NERFED_TEMPO_VALUE);
  const double eu_challenge = p_invalid * u_off + (1.0 - p_invalid) * u_penalty;
  assessment->p_invalid = p_invalid;
  assessment->eu_challenge = eu_challenge;
  assessment->u_accept = u_accept;
  assessment->threshold =
      NERFED_CHALLENGE_THRESH_C0 +
      NERFED_CHALLENGE_THRESH_RTG * nerfed_player->rating_z +
      (rule_5pt ? 0.0 : NERFED_CHALLENGE_THRESH_DOUBLE_EXTRA);
  // Speculative (hunch) challenge probability from the play's obscurity.
  assessment->p_speculative = nerfed_speculative_challenge_prob(
      rule_5pt, min_logplay, nerfed_player->rating_z);
}

bool nerfed_player_challenge_decide(const NerfedPlayer *nerfed_player,
                                    const NerfedChallengeAssessment *assessment,
                                    XoshiroPRNG *prng) {
  (void)nerfed_player;
  if (!assessment->attended) {
    return false;
  }
  if (nerfed_player_uniform(prng) < assessment->p_speculative) {
    return true;
  }
  const double noise =
      NERFED_CHALLENGE_NOISE * -log(-log(nerfed_player_uniform(prng)));
  return assessment->eu_challenge - assessment->u_accept + noise >
         assessment->threshold;
}

bool nerfed_player_challenge_is_marginal(
    const NerfedChallengeAssessment *assessment) {
  if (!assessment->attended) {
    return false;
  }
  const double margin =
      assessment->eu_challenge - assessment->u_accept - assessment->threshold;
  return fabs(margin) < NERFED_CHALLENGE_SIM_BAND;
}

bool nerfed_player_challenge_decide_simmed(
    const NerfedPlayer *nerfed_player,
    const NerfedChallengeAssessment *assessment, double u_accept, double u_off,
    double u_fail, XoshiroPRNG *prng) {
  (void)nerfed_player;
  if (!assessment->attended) {
    return false;
  }
  if (nerfed_player_uniform(prng) < assessment->p_speculative) {
    return true;
  }
  const double eu_challenge =
      assessment->p_invalid * u_off + (1.0 - assessment->p_invalid) * u_fail;
  const double noise =
      NERFED_CHALLENGE_NOISE * -log(-log(nerfed_player_uniform(prng)));
  return eu_challenge - u_accept + noise > assessment->threshold;
}

double nerfed_player_sigma_for_rating(int rating) {
  const double rating_z = (rating - 1500.0) / 300.0;
  return exp(NERFED_SIGMA_C0 + NERFED_SIGMA_C1 * rating_z);
}

double nerfed_player_margin_utility(double margin) {
  return nerfed_player_win_utility(margin);
}

double nerfed_player_challenge_state_utility(double win_probability,
                                             double spread) {
  return win_probability + NERFED_SPREAD_UTIL * spread;
}

bool nerfed_player_challenge_decision(const NerfedPlayer *nerfed_player,
                                      const NerfedPlayer *opponent, Game *game,
                                      const Move *move, bool rule_5pt,
                                      XoshiroPRNG *prng) {
  NerfedChallengeAssessment assessment;
  nerfed_player_challenge_assess(nerfed_player, opponent, game, move, rule_5pt,
                                 prng, &assessment);
  return nerfed_player_challenge_decide(nerfed_player, &assessment, prng);
}