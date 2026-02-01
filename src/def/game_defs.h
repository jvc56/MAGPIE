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

// Mode for cross set computation with overridden KWGs.
// SHARED: Both players use the same KWG (e.g., single pruned KWG in endgame).
//         Each player assumes the opponent knows their lexicon.
// PER_PLAYER: Each player uses their own KWG (potentially different pruned KWGs).
//             Each player knows what words the opponent can actually play.
typedef enum {
  ENDGAME_LEXICON_SHARED,
  ENDGAME_LEXICON_PER_PLAYER,
} endgame_lexicon_mode_t;

#endif