#include "game_history.h"

#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"

#include "board.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

struct GameEvent {
  game_event_t event_type;
  int player_index;
  int cumulative_score;
  Rack *rack;
  Move *move;
  char *note;
};

GameEvent *game_event_create() {
  GameEvent *game_event = malloc_or_die(sizeof(GameEvent));
  game_event->event_type = GAME_EVENT_UNKNOWN;
  game_event->player_index = -1;
  game_event->cumulative_score = 0;
  game_event->move = NULL;
  game_event->rack = NULL;
  game_event->note = NULL;
  return game_event;
}

void game_event_destroy(GameEvent *game_event) {
  if (!game_event) {
    return;
  }
  move_destroy(game_event->move);
  rack_destroy(game_event->rack);
  free(game_event->note);
  free(game_event);
}

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

void game_event_set_note(GameEvent *event, const char *note) {
  free(event->note);
  event->note = string_duplicate(note);
}

const char *game_event_get_note(const GameEvent *event) { return event->note; }

struct GameHistoryPlayer {
  char *name;
  char *nickname;
  int score;
  Rack *last_known_rack;
};

GameHistoryPlayer *game_history_player_create(const char *name,
                                              const char *nickname) {
  GameHistoryPlayer *player = malloc_or_die(sizeof(GameHistoryPlayer));
  player->name = string_duplicate(name);
  player->nickname = string_duplicate(nickname);
  player->score = 0;
  player->last_known_rack = NULL;
  return player;
}

void game_history_player_destroy(GameHistoryPlayer *player) {
  if (!player) {
    return;
  }
  free(player->name);
  free(player->nickname);
  rack_destroy(player->last_known_rack);
  free(player);
}

void game_history_player_set_name(GameHistoryPlayer *player, const char *name) {
  free(player->name);
  player->name = string_duplicate(name);
}

const char *game_history_player_get_name(const GameHistoryPlayer *player) {
  return player->name;
}

void game_history_player_set_nickname(GameHistoryPlayer *player,
                                      const char *nickname) {
  free(player->nickname);
  player->nickname = string_duplicate(nickname);
}

const char *game_history_player_get_nickname(const GameHistoryPlayer *player) {
  return player->nickname;
}

int game_history_player_get_score(const GameHistoryPlayer *player) {
  return player->score;
}

void game_history_player_set_last_known_rack(GameHistoryPlayer *player,
                                             Rack *rack) {
  player->last_known_rack = rack;
}

Rack *game_history_player_get_last_known_rack(const GameHistoryPlayer *player) {
  return player->last_known_rack;
}

struct GameHistory {
  char *title;
  char *description;
  char *id_auth;
  char *uid;
  char *lexicon_name;
  char *ld_name;
  game_variant_t game_variant;
  Board *board;
  GameHistoryPlayer *players[2];
  int number_of_events;
  LetterDistribution *ld;
  GameEvent **events;
};

void game_history_set_title(GameHistory *history, const char *title) {
  free(history->title);
  history->title = string_duplicate(title);
}

const char *game_history_get_title(const GameHistory *history) {
  return history->title;
}

void game_history_set_description(GameHistory *history,
                                  const char *description) {
  free(history->description);
  history->description = string_duplicate(description);
}

const char *game_history_get_description(const GameHistory *history) {
  return history->description;
}

void game_history_set_id_auth(GameHistory *history, const char *id_auth) {
  free(history->id_auth);
  history->id_auth = string_duplicate(id_auth);
}

const char *game_history_get_id_auth(const GameHistory *history) {
  return history->id_auth;
}

void game_history_set_uid(GameHistory *history, const char *uid) {
  free(history->uid);
  history->uid = string_duplicate(uid);
}

const char *game_history_get_uid(const GameHistory *history) {
  return history->uid;
}

void game_history_set_lexicon_name(GameHistory *history,
                                   const char *lexicon_name) {
  free(history->lexicon_name);
  history->lexicon_name = string_duplicate(lexicon_name);
}

const char *game_history_get_lexicon_name(const GameHistory *history) {
  return history->lexicon_name;
}

void game_history_set_ld_name(GameHistory *history, const char *ld_name) {
  free(history->ld_name);
  history->ld_name = string_duplicate(ld_name);
}

const char *game_history_get_ld_name(const GameHistory *history) {
  return history->ld_name;
}

void game_history_set_game_variant(GameHistory *history,
                                   game_variant_t game_variant) {
  history->game_variant = game_variant;
}

game_variant_t game_history_get_game_variant(const GameHistory *history) {
  return history->game_variant;
}

void game_history_set_board(GameHistory *history, Board *board) {
  history->board = board;
}

const Board *game_history_get_board(const GameHistory *history) {
  return history->board;
}

void game_history_set_player(GameHistory *history, int player_index,
                             GameHistoryPlayer *player) {
  history->players[player_index] = player;
}

GameHistoryPlayer *game_history_get_player(const GameHistory *history,
                                           int player_index) {
  return history->players[player_index];
}

int game_history_get_number_of_events(const GameHistory *history) {
  return history->number_of_events;
}

void game_history_set_ld(GameHistory *history, LetterDistribution *ld) {
  history->ld = ld;
}

LetterDistribution *game_history_get_ld(const GameHistory *history) {
  return history->ld;
}

GameEvent *game_history_get_event(const GameHistory *history, int event_index) {
  return history->events[event_index];
}

GameHistory *game_history_create() {
  GameHistory *game_history = malloc_or_die(sizeof(GameHistory));
  game_history->title = NULL;
  game_history->description = NULL;
  game_history->id_auth = NULL;
  game_history->uid = NULL;
  game_history->lexicon_name = NULL;
  game_history->ld_name = NULL;
  game_history->game_variant = GAME_VARIANT_UNKNOWN;
  game_history->board = NULL;
  game_history->players[0] = NULL;
  game_history->players[1] = NULL;
  game_history->ld = NULL;
  game_history->number_of_events = 0;
  game_history->events = malloc_or_die(sizeof(GameEvent) * (MAX_GAME_EVENTS));
  return game_history;
}

void game_history_destroy(GameHistory *game_history) {
  if (!game_history) {
    return;
  }
  free(game_history->title);
  free(game_history->description);
  free(game_history->id_auth);
  free(game_history->uid);
  free(game_history->lexicon_name);
  free(game_history->ld_name);

  ld_destroy(game_history->ld);

  for (int i = 0; i < 2; i++) {
    game_history_player_destroy(game_history->players[i]);
  }
  for (int i = 0; i < game_history->number_of_events; i++) {
    game_event_destroy(game_history->events[i]);
  }
  free(game_history->events);
  free(game_history);
}

GameEvent *game_history_create_and_add_game_event(GameHistory *game_history) {
  if (game_history->number_of_events == MAX_GAME_EVENTS) {
    log_fatal("game events overflow");
  }
  GameEvent *game_event = game_event_create();
  game_history->events[game_history->number_of_events++] = game_event;
  return game_event;
}

void game_history_set_cumulative_scores(GameHistory *game_history) {
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