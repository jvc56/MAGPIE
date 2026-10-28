#ifndef GAME_DEFS_H
#define GAME_DEFS_H

#include <stdbool.h>

#define GAME_VARIANT_UNKNOWN_NAME "unknown"
#define GAME_VARIANT_CLASSIC_NAME "classic"
#define GAME_VARIANT_WORDSMOG_NAME "wordsmog"
#define GAME_VARIANT_WOADWAGE_NAME "woadwage"
#ifdef IF_ONLY
#define GAME_VARIANT_IF_ONLY_NAME "ifonly"
#define GAME_VARIANT_LONIFY_NAME "lonify"
#endif

enum {
  MAX_SEARCH_DEPTH = 25,
  MAX_SCORELESS_TURNS = 6,
  ASCII_UPPERCASE_A = 65,
};

typedef enum {
  GAME_VARIANT_UNKNOWN,
  GAME_VARIANT_CLASSIC,
  GAME_VARIANT_WORDSMOG,
  // Classic play, where every word a play forms may also be two words of the
  // lexicon run together.
  GAME_VARIANT_WOADWAGE,
#ifdef IF_ONLY
  // Classic words, plus fake blanks and swaps (see game_variant_is_if_only).
  GAME_VARIANT_IF_ONLY,
  // WordSmog words, plus fake blanks and swaps.
  GAME_VARIANT_LONIFY,
#endif
} game_variant_t;

// Whether a word may be any anagram of a lexicon word: WordSmog, and Lonify,
// which is WordSmog with If Only's fake blanks and swaps.
static inline bool game_variant_is_wordsmog(game_variant_t game_variant) {
#ifdef IF_ONLY
  return game_variant == GAME_VARIANT_WORDSMOG ||
         game_variant == GAME_VARIANT_LONIFY;
#else
  return game_variant == GAME_VARIANT_WORDSMOG;
#endif
}

// Whether a turn may swap a fake blank off the board and a placement may play
// a tile upside down as a fake blank: If Only and Lonify. Always false in a
// build without IF_ONLY.
static inline bool game_variant_is_if_only(game_variant_t game_variant) {
#ifdef IF_ONLY
  return game_variant == GAME_VARIANT_IF_ONLY ||
         game_variant == GAME_VARIANT_LONIFY;
#else
  (void)game_variant;
  return false;
#endif
}

// The variant whose rules for words the given one plays by: If Only plays
// classic words and Lonify WordSmog's, and every other variant its own.
static inline game_variant_t
game_variant_word_rules(game_variant_t game_variant) {
#ifdef IF_ONLY
  if (game_variant == GAME_VARIANT_IF_ONLY) {
    return GAME_VARIANT_CLASSIC;
  }
  if (game_variant == GAME_VARIANT_LONIFY) {
    return GAME_VARIANT_WORDSMOG;
  }
#endif
  return game_variant;
}

typedef enum {
  BACKUP_MODE_OFF,
  BACKUP_MODE_SIMULATION,
  BACKUP_MODE_GCG,
} backup_mode_t;

typedef enum {
  GAME_END_REASON_NONE,
  GAME_END_REASON_STANDARD,
  GAME_END_REASON_CONSECUTIVE_ZEROS,
  NUMBER_OF_GAME_END_REASONS,
} game_end_reason_t;

// Dual-lexicon mode for endgame solving (when players have different word
// lists):
// IGNORANT: each player does not know their opponent's word list, so they
//           model the opponent as using their own (shared cross-sets)
// INFORMED: each player knows their opponent's word list (separate cross-sets
//           per player)
typedef enum {
  DUAL_LEXICON_MODE_IGNORANT,
  DUAL_LEXICON_MODE_INFORMED,
} dual_lexicon_mode_t;

#endif