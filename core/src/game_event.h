#ifndef GAME_EVENT_H
#define GAME_EVENT_H

#include <stdint.h>

#include "move.h"
#include "rack.h"

typedef struct GameEvent {
  game_event_t event_type;
  int player_index;
  int cumulative_score;
  Rack *rack;
  Move *move;
  char *note;
} GameEvent;

GameEvent *create_game_event();
void destroy_game_event(GameEvent *game_event);

#endif