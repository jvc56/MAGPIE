
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "game_event.h"
#include "move.h"
#include "rack.h"
#include "util.h"

GameEvent *create_game_event() {
  GameEvent *game_event = malloc_or_die(sizeof(GameEvent));
  game_event->event_type = GAME_EVENT_UNKNOWN;
  game_event->player_index = -1;
  game_event->cumulative_score = 0;
  game_event->move = NULL;
  game_event->rack = NULL;
  game_event->note = NULL;
  return game_event;
}

void destroy_game_event(GameEvent *game_event) {
  if (game_event->move) {
    destroy_move(game_event->move);
  }
  if (game_event->rack) {
    destroy_rack(game_event->rack);
  }
  if (game_event->note) {
    free(game_event->note);
  }
  free(game_event);
}
