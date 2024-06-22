#include "game_history.h"

#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"

#include "letter_distribution.h"
#include "rack.h"
#include "validated_move.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

struct GameEvent {
  game_event_t event_type;
  int player_index;
  int cumulative_score;
  char *cgp_move_string;
  int move_score;
  // Adjustment for
  // - Challenge points
  // - Time penalty
  // - End rack points
  // - End rack penalty
  int score_adjustment;
  Rack *rack;
  ValidatedMoves *vms;
  char *note;
};

GameEvent *game_event_create() {
  GameEvent *game_event = malloc_or_die(sizeof(GameEvent));
  game_event->event_type = GAME_EVENT_UNKNOWN;
  game_event->player_index = -1;
  game_event->cumulative_score = 0;
  game_event->score_adjustment = 0;
  game_event->cgp_move_string = NULL;
  game_event->move_score = 0;
  game_event->vms = NULL;
  game_event->rack = NULL;
  game_event->note = NULL;
  return game_event;
}

void game_event_destroy(GameEvent *game_event) {
  if (!game_event) {
    return;
  }
  validated_moves_destroy(game_event->vms);
  rack_destroy(game_event->rack);
  free(game_event->cgp_move_string);
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

void game_event_set_move_score(GameEvent *event, int move_score) {
  event->move_score = move_score;
}

int game_event_get_move_score(const GameEvent *event) {
  return event->move_score;
}

// Takes ownership of the cgp_move_string
void game_event_set_cgp_move_string(GameEvent *event, char *cgp_move_string) {
  event->cgp_move_string = cgp_move_string;
}

const char *game_event_get_cgp_move_string(const GameEvent *event) {
  return event->cgp_move_string;
}

void game_event_set_score_adjustment(GameEvent *event, int score_adjustment) {
  event->score_adjustment = score_adjustment;
}

int game_event_get_score_adjustment(const GameEvent *event) {
  return event->score_adjustment;
}

void game_event_set_rack(GameEvent *event, Rack *rack) { event->rack = rack; }

Rack *game_event_get_rack(const GameEvent *event) { return event->rack; }

void game_event_set_vms(GameEvent *event, ValidatedMoves *vms) {
  event->vms = vms;
}

ValidatedMoves *game_event_get_vms(const GameEvent *event) {
  return event->vms;
}

void game_event_set_note(GameEvent *event, const char *note) {
  free(event->note);
  event->note = string_duplicate(note);
}

const char *game_event_get_note(const GameEvent *event) { return event->note; }

// Returns 1 if the game event counts as a turn
// Returns 0 otherwise
int game_event_get_turn_value(const GameEvent *event) {
  if (event->event_type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
      event->event_type == GAME_EVENT_EXCHANGE ||
      event->event_type == GAME_EVENT_PASS ||
      event->event_type == GAME_EVENT_PHONY_TILES_RETURNED) {
    return 1;
  }
  return 0;
}

typedef struct GameHistoryPlayer {
  char *name;
  char *nickname;
  int score;
  bool next_rack_set;
  Rack *last_known_rack;
} GameHistoryPlayer;

struct GameHistory {
  char *title;
  char *description;
  char *id_auth;
  char *uid;
  char *lexicon_name;
  char *ld_name;
  char *board_layout_name;
  game_variant_t game_variant;
  GameHistoryPlayer *players[2];
  int number_of_events;
  GameEvent **events;
};

GameHistoryPlayer *game_history_player_create(const char *name,
                                              const char *nickname) {
  GameHistoryPlayer *player = malloc_or_die(sizeof(GameHistoryPlayer));
  player->name = string_duplicate(name);
  player->nickname = string_duplicate(nickname);
  player->score = 0;
  player->next_rack_set = false;
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

void game_history_player_set_name(GameHistory *game_history, int player_index,
                                  const char *name) {
  GameHistoryPlayer *player = game_history->players[player_index];
  free(player->name);
  player->name = string_duplicate(name);
}

const char *game_history_player_get_name(const GameHistory *game_history,
                                         int player_index) {
  GameHistoryPlayer *player = game_history->players[player_index];
  return player->name;
}

void game_history_player_set_nickname(GameHistory *game_history,
                                      int player_index, const char *nickname) {
  GameHistoryPlayer *player = game_history->players[player_index];
  free(player->nickname);
  player->nickname = string_duplicate(nickname);
}

const char *game_history_player_get_nickname(const GameHistory *game_history,
                                             int player_index) {
  GameHistoryPlayer *player = game_history->players[player_index];
  return player->nickname;
}

void game_history_player_set_score(GameHistory *game_history, int player_index,
                                   int score) {
  GameHistoryPlayer *player = game_history->players[player_index];
  player->score = score;
}

int game_history_player_get_score(const GameHistory *game_history,
                                  int player_index) {
  GameHistoryPlayer *player = game_history->players[player_index];
  return player->score;
}

void game_history_player_set_next_rack_set(GameHistory *game_history,
                                           int player_index,
                                           bool next_rack_set) {
  GameHistoryPlayer *player = game_history->players[player_index];
  player->next_rack_set = next_rack_set;
}

bool game_history_player_get_next_rack_set(const GameHistory *game_history,
                                           int player_index) {
  GameHistoryPlayer *player = game_history->players[player_index];
  return player->next_rack_set;
}

void game_history_player_set_last_known_rack(GameHistory *game_history,
                                             int player_index,
                                             const Rack *rack) {
  GameHistoryPlayer *player = game_history->players[player_index];
  if (!player->last_known_rack) {
    player->last_known_rack = rack_duplicate(rack);
  } else {
    rack_copy(player->last_known_rack, rack);
  }
}

Rack *game_history_player_get_last_known_rack(const GameHistory *game_history,
                                              int player_index) {
  GameHistoryPlayer *player = game_history->players[player_index];
  return player->last_known_rack;
}

bool game_history_player_is_set(const GameHistory *game_history,
                                int player_index) {
  return game_history->players[player_index];
}

bool game_history_both_players_are_set(const GameHistory *game_history) {
  return game_history->players[0] && game_history->players[1];
}

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

void game_history_set_board_layout_name(GameHistory *history,
                                        const char *board_layout_name) {
  free(history->board_layout_name);
  history->board_layout_name = string_duplicate(board_layout_name);
}

const char *game_history_get_board_layout_name(const GameHistory *history) {
  return history->board_layout_name;
}

void game_history_set_player(GameHistory *history, int player_index,
                             const char *player_name,
                             const char *player_nickname) {
  history->players[player_index] =
      game_history_player_create(player_name, player_nickname);
}

GameHistoryPlayer *game_history_get_player(const GameHistory *history,
                                           int player_index) {
  return history->players[player_index];
}

int game_history_get_number_of_events(const GameHistory *history) {
  return history->number_of_events;
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
  game_history->game_variant = GAME_VARIANT_CLASSIC;
  game_history->board_layout_name = board_layout_get_default_name();
  game_history->players[0] = NULL;
  game_history->players[1] = NULL;
  game_history->number_of_events = 0;
  game_history->events = malloc_or_die(sizeof(GameEvent *) * (MAX_GAME_EVENTS));
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
  free(game_history->board_layout_name);

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
