#include "nerfed_player.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/words.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_gen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  NERFED_MOVE_LIST_CAPACITY = 1024,
  NERFED_MAX_WORD_FEATS = 400000,
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
    -2.397, // intercept (calibrated)
    -0.661, // delta / 10
    0.162,  // rating_z (calibrated)
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

// Defaults for words missing from the feature table (centered scales).
static const double NERFED_DEFAULT_LOGPLAY = -2.0;
static const double NERFED_DEFAULT_LOGLIT = -1.0;

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
};

static int nerfed_word_feat_compare(const void *feat_a, const void *feat_b) {
  const NerfedWordFeat *word_feat_a = (const NerfedWordFeat *)feat_a;
  const NerfedWordFeat *word_feat_b = (const NerfedWordFeat *)feat_b;
  if (word_feat_a->word_length != word_feat_b->word_length) {
    return (int)word_feat_a->word_length - (int)word_feat_b->word_length;
  }
  return memcmp(word_feat_a->word, word_feat_b->word, word_feat_a->word_length);
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

NerfedPlayer *nerfed_player_create(const Game *game, int rating,
                                   ErrorStack *error_stack) {
  const Player *player = game_get_player(game, 0);
  const char *lexicon_name = kwg_get_name(player_get_kwg(player));
  const LetterDistribution *ld = game_get_ld(game);
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
  nerfed_player->move_list = move_list_create(NERFED_MOVE_LIST_CAPACITY);
  nerfed_player->num_word_draws = 0;
  return nerfed_player;
}

void nerfed_player_destroy(NerfedPlayer *nerfed_player) {
  if (!nerfed_player) {
    return;
  }
  move_list_destroy(nerfed_player->move_list);
  free(nerfed_player->feats);
  free(nerfed_player);
}

static double nerfed_player_uniform(XoshiroPRNG *prng) {
  // 53-bit mantissa uniform in (0, 1); never returns exactly 0 or 1.
  return ((double)(prng_next(prng) >> 11) + 0.5) / 9007199254740992.0;
}

// P(this play is invisible to the player): Stage B miss logistic with a
// best-class of one (log_class = 0). Writes the play's rarest formed word
// (minimum playability) to rare_word/rare_word_length so the caller can
// share one visibility draw across all plays using that word.
static double nerfed_player_miss_probability(const NerfedPlayer *nerfed_player,
                                             Game *game, const Move *move,
                                             MachineLetter *rare_word,
                                             int *rare_word_length) {
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
    double logplay = NERFED_DEFAULT_LOGPLAY;
    double loglit = NERFED_DEFAULT_LOGLIT;
    double absent = 1.0;
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

const Move *nerfed_player_select_move(NerfedPlayer *nerfed_player, Game *game,
                                      XoshiroPRNG *prng) {
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
        const double gumbel_noise = -nerfed_player->sigma * log(-log(uniform));
        const double value =
            equity_to_double(move_get_equity(move)) + gumbel_noise;
        if (chosen_exchange == NULL || value > chosen_exchange_value) {
          chosen_exchange = move;
          chosen_exchange_value = value;
        }
      }
      return chosen_exchange;
    }
  }

  const Move *chosen = NULL;
  double chosen_value = 0.0;
  const Move *fallback = NULL;
  Equity fallback_equity = 0;
  for (int move_idx = 0; move_idx < move_count; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (fallback == NULL || move_get_equity(move) > fallback_equity) {
      fallback = move;
      fallback_equity = move_get_equity(move);
    }
    if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
      // The exchange decision was already rolled (and declined) above.
      continue;
    }
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      MachineLetter rare_word[BOARD_DIM];
      int rare_word_length = 0;
      const double miss_probability = nerfed_player_miss_probability(
          nerfed_player, game, move, rare_word, &rare_word_length);
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
    const double base_value = (move_equity == EQUITY_PASS_VALUE)
                                  ? -1000.0
                                  : equity_to_double(move_equity);
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
