#ifndef GAME_DEFS_H
#define GAME_DEFS_H

#define GAME_VARIANT_UNKNOWN_NAME "unknown"
#define GAME_VARIANT_CLASSIC_NAME "classic"
#define GAME_VARIANT_WORDSMOG_NAME "wordsmog"

enum {
  MAX_SEARCH_DEPTH = 25,
  MAX_SCORELESS_TURNS = 6,
  ASCII_UPPERCASE_A = 65,
};

typedef enum {
  GAME_VARIANT_UNKNOWN,
  GAME_VARIANT_CLASSIC,
  GAME_VARIANT_WORDSMOG,
} game_variant_t;

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