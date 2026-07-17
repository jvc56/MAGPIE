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
// fitted on 380,370 corpus positions. Predicts P(MISS play); visibility is
// its complement. Feature order matches nerfed_player_miss_probability.
static const double NERFED_MISS_COEFFS[13] = {
    1.1299,  // bias
    -1.0000, // rating_z (rating-1500)/300
    -0.7078, // min log10 playability over formed words
    -0.1260, // min log10 literacy over formed words
    0.6341,  // any formed word absent from language
    0.5222,  // (num formed words - 1) / 2
    0.5648,  // play uses a blank
    0.4639,  // play goes through tiles
    -0.7523, // tiles played / 7
    -0.2373, // log(best-class size); 1 per play at generation time
    0.1600,  // rating_z x playability
    0.0592,  // rating_z x literacy
    -0.0473, // rating_z x blank
};

// Stage A valuation noise: sigma = exp(C0 + C1 * rating_z) equity points.
static const double NERFED_SIGMA_C0 = 1.59233;
static const double NERFED_SIGMA_C1 = -0.17506;

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

struct NerfedPlayer {
  double rating_z;
  double sigma;
  NerfedWordFeat *feats;
  int num_feats;
  MoveList *move_list;
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
// best-class of one (log_class = 0).
static double nerfed_player_miss_probability(const NerfedPlayer *nerfed_player,
                                             Game *game, const Move *move) {
  Board *board = game_get_board(game);
  FormedWords *formed_words = formed_words_create(board, move);
  const int num_words = formed_words_get_num_words(formed_words);
  double min_logplay = 99.0;
  double min_loglit = 99.0;
  double any_absent = 0.0;
  MachineLetter word[BOARD_DIM];
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
  const double rating_z = nerfed_player->rating_z;
  const double features[13] = {
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
      rating_z * min_logplay,
      rating_z * min_loglit,
      rating_z * uses_blank,
  };
  double logit = 0.0;
  for (int coeff_idx = 0; coeff_idx < 13; coeff_idx++) {
    logit += NERFED_MISS_COEFFS[coeff_idx] * features[coeff_idx];
  }
  return 1.0 / (1.0 + exp(-logit));
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
  const int move_count = move_list_get_count(move_list);
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
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      const double miss_probability =
          nerfed_player_miss_probability(nerfed_player, game, move);
      if (nerfed_player_uniform(prng) < miss_probability) {
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
