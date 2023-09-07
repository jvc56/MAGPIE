#ifndef GCG_H
#define GCG_H

#include <stdint.h>

#include "player.h"
#define MAX_GAME_EVENTS 100

typedef enum {
  GAME_EVENT_TILE_PLACEMENT_MOVE,
  GAME_EVENT_PHONY_TILES_RETURNED,
  GAME_EVENT_PASS,
  GAME_EVENT_CHALLENGE_BONUS,
  GAME_EVENT_EXCHANGE,
  GAME_EVENT_END_RACK_PTS,
  GAME_EVENT_TIME_PENALTY,
  GAME_EVENT_END_RACK_PENALTY,
  GAME_EVENT_UNSUCCESSFUL_CHALLENGE_TURN_LOSS,
  GAME_EVENT_CHALLENGE
} game_event_t;

typedef struct GameEvent {
  game_event_t event_type;
  int cumulative_score;
  Rack *rack;
  Move *move;
  char *note;
} GameEvent;

typedef struct GameHistoryPlayer {
  char *name;
  char *nickname;
  int score;
  Rack *last_known_rack;
} GameHistoryPlayer;

typedef struct GameHistory {
  char *title;
  char *description;
  char *id_auth;
  char *uid;
  char *lexicon_name;
  char *letter_distribution_name;
  char *variant;
  board_layout_t board_layout;
  GameHistoryPlayer *players[2];
  int number_of_events;
  LetterDistribution *letter_distribution;
  GameEvent **events;
} GameHistory;

GameEvent *create_game_event(GameHistory *game_history);
GameHistory *create_game_history();
void destroy_game_history(GameHistory *game_history);
GameHistoryPlayer *create_game_history_player(const char *name,
                                              const char *nickname);
void destroy_game_history_player(GameHistoryPlayer *player);

#endif