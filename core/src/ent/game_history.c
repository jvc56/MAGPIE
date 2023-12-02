#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"

#include "game_history.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
struct GameEvent {
  game_event_t event_type;
  int player_index;
  int cumulative_score;
  Rack *rack;
  Move *move;
  char *note;
};

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
  free(game_event->note);
  free(game_event);
}

struct GameHistoryPlayer {
  char *name;
  char *nickname;
  int score;
  Rack *last_known_rack;
};

struct GameHistory {
  char *title;
  char *description;
  char *id_auth;
  char *uid;
  char *lexicon_name;
  char *letter_distribution_name;
  game_variant_t game_variant;
  board_layout_t board_layout;
  GameHistoryPlayer *players[2];
  int number_of_events;
  LetterDistribution *letter_distribution;
  GameEvent **events;
};

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