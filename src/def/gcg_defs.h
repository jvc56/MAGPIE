#ifndef GCG_DEFS_H
#define GCG_DEFS_H

typedef enum {
  GCG_PARSE_STATUS_SUCCESS,
  GCG_PARSE_STATUS_LEXICON_NOT_FOUND,
  GCG_PARSE_STATUS_DUPLICATE_NAMES,
  GCG_PARSE_STATUS_DUPLICATE_NICKNAMES,
  GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT,
  GCG_PARSE_STATUS_ENCODING_WRONG_PLACE,
  GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST,
  GCG_PARSE_STATUS_PLAYER_NUMBER_REDUNDANT,
  GCG_PARSE_STATUS_UNSUPPORTED_CHARACTER_ENCODING,
  GCG_PARSE_STATUS_GCG_EMPTY,
  GCG_PARSE_STATUS_NOTE_PRECEDENT_EVENT,
  GCG_PARSE_STATUS_NO_MATCHING_TOKEN,
  GCG_PARSE_STATUS_PHONY_TILES_RETURNED_WITHOUT_PLAY,
  GCG_PARSE_STATUS_PLAYED_LETTERS_NOT_IN_RACK,
  GCG_PARSE_STATUS_RACK_MALFORMED,
  GCG_PARSE_STATUS_PLAY_MALFORMED,
  GCG_PARSE_STATUS_INVALID_TILE_PLACEMENT_POSITION,
  GCG_PARSE_STATUS_MOVE_BEFORE_PLAYER,
  GCG_PARSE_STATUS_PLAY_OUT_OF_BOUNDS,
  GCG_PARSE_STATUS_REDUNDANT_PRAGMA,
  GCG_PARSE_STATUS_GAME_EVENTS_OVERFLOW,
} gcg_parse_status_t;

#endif
