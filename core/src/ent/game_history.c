#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"

#include "../util/string_util.h"

#include "../util/log.h"
#include "../util/util.h"

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

// Setter and getter functions for GameEvent struct
void game_event_set_type(GameEvent *event, game_event_t event_type) {
  event->event_type = event_type;
}

game_event_t game_event_get_type(const GameEvent *event) {
  return event->event_type;
}

void game_event_set_player_index(GameEvent *event, int player_index) {
  event->player_index = player_index;
}

int game_event_get_player_index(const GameEvent *event) {
  return event->player_index;
}

void game_event_set_cumulative_score(GameEvent *event, int cumulative_score) {
  event->cumulative_score = cumulative_score;
}

int game_event_get_cumulative_score(const GameEvent *event) {
  return event->cumulative_score;
}

void game_event_set_rack(GameEvent *event, Rack *rack) { event->rack = rack; }

Rack *game_event_get_rack(const GameEvent *event) { return event->rack; }

void game_event_set_move(GameEvent *event, Move *move) { event->move = move; }

Move *game_event_get_move(const GameEvent *event) { return event->move; }

void game_event_set_score(GameEvent *event, int score) {
  move_set_score(event->move, score);
}

Move *game_event_get_score(const GameEvent *event) { return event->move; }

void game_event_set_note(GameEvent *event, const char *note) {
  free(event->note);
  event->note = string_duplicate(note);
}

const char *game_event_get_note(const GameEvent *event) { return event->note; }

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

void set_game_history_player_name(GameHistoryPlayer *player, const char *name) {
  free(player->name);
  player->name = string_duplicate(name);
}

const char *get_game_history_player_name(const GameHistoryPlayer *player) {
  return player->name;
}

void set_game_history_player_nickname(GameHistoryPlayer *player,
                                      const char *nickname) {
  free(player->nickname);
  player->nickname = string_duplicate(nickname);
}

const char *get_game_history_player_nickname(const GameHistoryPlayer *player) {
  return player->nickname;
}

void set_game_history_player_score(GameHistoryPlayer *player, int score) {
  player->score = score;
}

int get_game_history_player_score(const GameHistoryPlayer *player) {
  return player->score;
}

void set_game_history_player_last_known_rack(GameHistoryPlayer *player,
                                             Rack *rack) {
  player->last_known_rack = rack;
}

Rack *get_game_history_player_last_known_rack(const GameHistoryPlayer *player) {
  return player->last_known_rack;
}

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

void set_game_history_title(GameHistory *history, const char *title) {
  free(history->title);
  history->title = string_duplicate(title);
}

const char *get_game_history_title(const GameHistory *history) {
  return history->title;
}

void set_game_history_description(GameHistory *history,
                                  const char *description) {
  free(history->description);
  history->description = string_duplicate(description);
}

const char *get_game_history_description(const GameHistory *history) {
  return history->description;
}

void set_game_history_id_auth(GameHistory *history, const char *id_auth) {
  free(history->id_auth);
  history->id_auth = string_duplicate(id_auth);
}

const char *get_game_history_id_auth(const GameHistory *history) {
  return history->id_auth;
}

void set_game_history_uid(GameHistory *history, const char *uid) {
  free(history->uid);
  history->uid = string_duplicate(uid);
}

const char *get_game_history_uid(const GameHistory *history) {
  return history->uid;
}

void set_game_history_lexicon_name(GameHistory *history,
                                   const char *lexicon_name) {
  free(history->lexicon_name);
  history->lexicon_name = string_duplicate(lexicon_name);
}

const char *get_game_history_lexicon_name(const GameHistory *history) {
  return history->lexicon_name;
}

void set_game_history_letter_distribution_name(
    GameHistory *history, const char *letter_distribution_name) {
  free(history->letter_distribution_name);
  history->letter_distribution_name =
      string_duplicate(letter_distribution_name);
}

const char *
get_game_history_letter_distribution_name(const GameHistory *history) {
  return history->letter_distribution_name;
}

void set_game_history_game_variant(GameHistory *history,
                                   game_variant_t game_variant) {
  history->game_variant = game_variant;
}

game_variant_t get_game_history_game_variant(const GameHistory *history) {
  return history->game_variant;
}

void set_game_history_board_layout(GameHistory *history,
                                   board_layout_t board_layout) {
  history->board_layout = board_layout;
}

board_layout_t get_game_history_board_layout(const GameHistory *history) {
  return history->board_layout;
}

void set_game_history_player(GameHistory *history, int player_index,
                             GameHistoryPlayer *player) {
  history->players[player_index] = player;
}

GameHistoryPlayer *get_game_history_player(const GameHistory *history,
                                           int player_index) {
  return history->players[player_index];
}

void set_game_history_number_of_events(GameHistory *history,
                                       int number_of_events) {
  history->number_of_events = number_of_events;
}

int get_game_history_number_of_events(const GameHistory *history) {
  return history->number_of_events;
}

void set_game_history_letter_distribution(
    GameHistory *history, LetterDistribution *letter_distribution) {
  history->letter_distribution = letter_distribution;
}

LetterDistribution *
get_game_history_letter_distribution(const GameHistory *history) {
  return history->letter_distribution;
}

void set_game_history_events(GameHistory *history, struct GameEvent **events) {
  history->events = events;
}

GameEvent *get_game_history_event(const GameHistory *history, int event_index) {
  return history->events[event_index];
}

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

GameEvent *create_game_event_and_add_to_history(GameHistory *game_history) {
  if (game_history->number_of_events == MAX_GAME_EVENTS) {
    log_fatal("game events overflow");
  }
  GameEvent *game_event = create_game_event();
  game_history->events[game_history->number_of_events++] = game_event;
  return game_event;
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