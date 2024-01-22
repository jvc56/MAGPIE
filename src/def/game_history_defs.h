#ifndef GAME_HISTORY_DEFS_H
#define GAME_HISTORY_DEFS_H

typedef enum {
  GAME_EVENT_UNKNOWN,
  GAME_EVENT_TILE_PLACEMENT_MOVE,
  GAME_EVENT_PHONY_TILES_RETURNED,
  GAME_EVENT_PASS,
  GAME_EVENT_CHALLENGE_BONUS,
  GAME_EVENT_EXCHANGE,
  GAME_EVENT_END_RACK_POINTS,
  GAME_EVENT_TIME_PENALTY,
  GAME_EVENT_END_RACK_PENALTY,
} game_event_t;

#define MAX_GAME_EVENTS 200

#endif