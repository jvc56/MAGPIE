
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "game.h"
#include "game_event.h"
#include "game_history.h"
#include "log.h"
#include "move.h"
#include "rack.h"
#include "string_util.h"
#include "util.h"

GameHistoryPlayer *create_game_history_player(const char *name,
                                              const char *nickname) {
  GameHistoryPlayer *player = malloc_or_die(sizeof(GameHistoryPlayer));
  player->name = string_duplicate(name);
  player->nickname = string_duplicate(nickname);
  player->score = 0;
  player->last_known_rack = NULL;
  return player;
}

void destroy_game_history_player(GameHistoryPlayer *player) {
  free(player->name);
  free(player->nickname);
  if (player->last_known_rack) {
    destroy_rack(player->last_known_rack);
  }
  free(player);
}

GameEvent *create_game_event_and_add_to_history(GameHistory *game_history) {
  if (game_history->number_of_events == MAX_GAME_EVENTS) {
    log_fatal("game events overflow");
  }
  GameEvent *game_event = create_game_event();
  game_history->events[game_history->number_of_events++] = game_event;
  return game_event;
}

GameHistory *create_game_history() {
  GameHistory *game_history = malloc_or_die(sizeof(GameHistory));
  game_history->title = NULL;
  game_history->description = NULL;
  game_history->id_auth = NULL;
  game_history->uid = NULL;
  game_history->lexicon_name = NULL;
  game_history->letter_distribution_name = NULL;
  game_history->game_variant = GAME_VARIANT_UNKNOWN;
  game_history->board_layout = BOARD_LAYOUT_UNKNOWN;
  game_history->players[0] = NULL;
  game_history->players[1] = NULL;
  game_history->letter_distribution = NULL;
  game_history->number_of_events = 0;
  game_history->events = malloc_or_die(sizeof(GameEvent) * (MAX_GAME_EVENTS));
  return game_history;
}

void destroy_game_history(GameHistory *game_history) {
  free(game_history->title);
  free(game_history->description);
  free(game_history->id_auth);
  free(game_history->uid);
  free(game_history->lexicon_name);
  free(game_history->letter_distribution_name);

  if (game_history->letter_distribution) {
    destroy_letter_distribution(game_history->letter_distribution);
  }

  for (int i = 0; i < 2; i++) {
    if (game_history->players[i]) {
      destroy_game_history_player(game_history->players[i]);
    }
  }
  for (int i = 0; i < game_history->number_of_events; i++) {
    destroy_game_event(game_history->events[i]);
  }
  free(game_history->events);
  free(game_history);
}

Game *play_to_turn(const GameHistory *game_history, int turn_number) {
  log_fatal("unimplemented: %p, %d\n", game_history, turn_number);
  return NULL;
}

void set_cumulative_scores(GameHistory *game_history) {
  for (int i = 0; i < 2; i++) {
    for (int j = game_history->number_of_events - 1; j >= 0; j--) {
      if (game_history->events[j]->player_index == i) {
        game_history->players[i]->score =
            game_history->events[j]->cumulative_score;
        break;
      }
    }
  }
}