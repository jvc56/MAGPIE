#ifndef GCG_H
#define GCG_H

#include <stdint.h>

#include "constants.h"

typedef struct GameEvent {
  int event_type;
} GameEvent;

typedef struct GameHistory {
  GameEvent **events;
} GameHistory;

#endif