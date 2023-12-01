#ifndef GAME_EVENT_H
#define GAME_EVENT_H

#include <stdint.h>

#include "move.h"
#include "rack.h"

struct GameEvent;
typedef struct GameEvent GameEvent;

GameEvent *create_game_event();
void destroy_game_event(GameEvent *game_event);

#endif