#ifndef GCG_TOKENIZER_H
#define GCG_TOKENIZER_H

#include <stdbool.h>

// Splits a GCG line into its token kind and the fields of that token. The
// grammar of each kind is documented in gcg_tokenizer.c.

#define GCG_TITLE_STRING "title"
#define GCG_DESCRIPTION_STRING "description"
#define GCG_ID_STRING "id"
#define GCG_NOTE_STRING "note"
#define GCG_LEXICON_STRING "lexicon"
#define GCG_CHAR_ENCODING_STRING "character-encoding"
#define GCG_GAME_TYPE_STRING "game-type"
#define GCG_TILE_SET_STRING "tile-set"
#define GCG_BOARD_LAYOUT_STRING "board-layout"
#define GCG_TILE_DISTRIBUTION_STRING "tile-distribution"
#define GCG_PLAYER_STRING "player"
#define GCG_RACK_STRING "rack"
#define GCG_CGP_STRING "cgp"

// Capture 0 is the whole match; captures 1 and up are the token's fields.
enum { GCG_MAX_CAPTURES = 7 };

// A Token is an event in a gcg_string file.
typedef enum {
  GCG_UNKNOWN_TOKEN,
  GCG_PLAYER_TOKEN,
  GCG_TITLE_TOKEN,
  GCG_DESCRIPTION_TOKEN,
  GCG_ID_TOKEN,
  GCG_RACK1_TOKEN,
  GCG_RACK2_TOKEN,
  GCG_ENCODING_TOKEN,
  GCG_MOVE_TOKEN,
  GCG_NOTE_TOKEN,
  GCG_LEXICON_TOKEN,
  GCG_CGP_TOKEN,
  GCG_PHONY_TILES_RETURNED_TOKEN,
  GCG_PASS_TOKEN,
  GCG_CHALLENGE_BONUS_TOKEN,
  GCG_EXCHANGE_TOKEN,
  GCG_END_RACK_POINTS_TOKEN,
  GCG_TIME_PENALTY_TOKEN,
  GCG_END_RACK_PENALTY_TOKEN,
  GCG_GAME_TYPE_TOKEN,
  GCG_TILE_SET_TOKEN,
  GCG_BOARD_LAYOUT_TOKEN,
  GCG_TILE_DISTRIBUTION_NAME_TOKEN,
  NUMBER_OF_GCG_TOKENS,
} gcg_token_t;

// The bytes [start, end) of the line, or start = end = -1 for a capture the
// token does not have.
typedef struct GCGCapture {
  int start;
  int end;
} GCGCapture;

// Whether `line` holds a token of kind `token`. On a match, fills
// captures[0] with the whole match, captures[1..] with the token's fields and
// the unused ones with -1. On no match the captures are unspecified.
bool gcg_line_matches_token(const char *line, gcg_token_t token,
                            GCGCapture captures[GCG_MAX_CAPTURES]);

// The first token kind, in priority order, that `line` holds, with its
// captures as for gcg_line_matches_token, or GCG_UNKNOWN_TOKEN if none.
gcg_token_t gcg_tokenize_line(const char *line,
                              GCGCapture captures[GCG_MAX_CAPTURES]);

#endif
