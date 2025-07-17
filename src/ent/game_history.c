#include "game_history.h"

#include <stdlib.h>

#include "../def/game_defs.h"
#include "../def/game_history_defs.h"

#include "../ent/equity.h"

#include "rack.h"
#include "validated_move.h"

#include "../util/io_util.h"
#include "../util/string_util.h"

struct GameEvent {
  game_event_t event_type;
  int player_index;
  Equity cumulative_score;
  char *cgp_move_string;
  Equity move_score;
  // Adjustment for
  // - Challenge points
  // - Time penalty
  // - End rack points
  // - End rack penalty
  Equity score_adjustment;
  Rack *rack;
  ValidatedMoves *vms;
  char *note;
};

void game_event_reset(GameEvent *game_event) {
  game_event->event_type = GAME_EVENT_UNKNOWN;
  game_event->player_index = -1;
  game_event->cumulative_score = 0;
  game_event->score_adjustment = 0;
  game_event->move_score = 0;
  free(game_event->cgp_move_string);
  game_event->cgp_move_string = NULL;
  validated_moves_destroy(game_event->vms);
  game_event->vms = NULL;
  rack_destroy(game_event->rack);
  game_event->rack = NULL;
  free(game_event->note);
  game_event->note = NULL;
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

void game_event_set_cumulative_score(GameEvent *event,
                                     Equity cumulative_score) {
  event->cumulative_score = cumulative_score;
}

Equity game_event_get_cumulative_score(const GameEvent *event) {
  return event->cumulative_score;
}

void game_event_set_move_score(GameEvent *event, Equity move_score) {
  event->move_score = move_score;
}

Equity game_event_get_move_score(const GameEvent *event) {
  return event->move_score;
}

// Takes ownership of the cgp_move_string
void game_event_set_cgp_move_string(GameEvent *event, char *cgp_move_string) {
  free(event->cgp_move_string);
  event->cgp_move_string = cgp_move_string;
}

const char *game_event_get_cgp_move_string(const GameEvent *event) {
  return event->cgp_move_string;
}

void game_event_set_score_adjustment(GameEvent *event,
                                     Equity score_adjustment) {
  event->score_adjustment = score_adjustment;
}

Equity game_event_get_score_adjustment(const GameEvent *event) {
  return event->score_adjustment;
}

void game_event_set_rack(GameEvent *event, Rack *rack) {
  rack_destroy(event->rack);
  event->rack = rack;
}

Rack *game_event_get_rack(const GameEvent *event) { return event->rack; }

void game_event_set_vms(GameEvent *event, ValidatedMoves *vms) {
  validated_moves_destroy(event->vms);
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
  Equity score;
  bool next_rack_set;
  Rack *last_known_rack;
  Rack *known_rack_from_phonies;
  Rack *previous_played_tiles;
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
  GameEvent *events;
};

GameHistoryPlayer *game_history_player_create(const char *name,
                                              const char *nickname) {
  GameHistoryPlayer *player = malloc_or_die(sizeof(GameHistoryPlayer));
  player->name = string_duplicate(name);
  player->nickname = string_duplicate(nickname);
  player->score = 0;
  player->next_rack_set = false;
  player->last_known_rack = NULL;
  player->known_rack_from_phonies = NULL;
  player->previous_played_tiles = NULL;
  return player;
}

void game_history_player_destroy(GameHistoryPlayer *player) {
  if (!player) {
    return;
  }
  free(player->name);
  free(player->nickname);
  rack_destroy(player->last_known_rack);
  rack_destroy(player->known_rack_from_phonies);
  rack_destroy(player->previous_played_tiles);
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
  return game_history->players[player_index]->name;
}

void game_history_player_set_nickname(GameHistory *game_history,
                                      int player_index, const char *nickname) {
  GameHistoryPlayer *player = game_history->players[player_index];
  free(player->nickname);
  player->nickname = string_duplicate(nickname);
}

const char *game_history_player_get_nickname(const GameHistory *game_history,
                                             int player_index) {
  return game_history->players[player_index]->nickname;
}

void game_history_player_set_score(GameHistory *game_history, int player_index,
                                   Equity score) {
  GameHistoryPlayer *player = game_history->players[player_index];
  player->score = score;
}

int game_history_player_get_score(const GameHistory *game_history,
                                  int player_index) {
  return game_history->players[player_index]->score;
}

void game_history_player_set_next_rack_set(GameHistory *game_history,
                                           int player_index,
                                           bool next_rack_set) {
  GameHistoryPlayer *player = game_history->players[player_index];
  player->next_rack_set = next_rack_set;
}

bool game_history_player_get_next_rack_set(const GameHistory *game_history,
                                           int player_index) {
  return game_history->players[player_index]->next_rack_set;
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
  return game_history->players[player_index]->last_known_rack;
}

void game_history_init_player_phony_calc_racks(GameHistory *game_history,
                                               const int ld_size) {
  for (int player_index = 0; player_index < 2; player_index++) {
    GameHistoryPlayer *player = game_history->players[player_index];
    if (!player->known_rack_from_phonies) {
      player->known_rack_from_phonies = rack_create(ld_size);
      player->previous_played_tiles = rack_create(ld_size);
    } else {
      rack_reset(player->known_rack_from_phonies);
      rack_reset(player->previous_played_tiles);
    }
  }
}

Rack *
game_history_player_get_known_rack_from_phonies(const GameHistory *game_history,
                                                const int player_index) {
  return game_history->players[player_index]->known_rack_from_phonies;
}

Rack *
game_history_player_get_previous_played_tiles(const GameHistory *game_history,
                                              const int player_index) {
  return game_history->players[player_index]->previous_played_tiles;
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

int game_history_get_number_of_events(const GameHistory *history) {
  return history->number_of_events;
}

GameEvent *game_history_get_event(const GameHistory *history, int event_index) {
  return &history->events[event_index];
}

void game_history_reset(GameHistory *game_history) {
  free(game_history->title);
  game_history->title = NULL;
  free(game_history->description);
  game_history->description = NULL;
  free(game_history->id_auth);
  game_history->id_auth = NULL;
  free(game_history->uid);
  game_history->uid = NULL;
  free(game_history->lexicon_name);
  game_history->lexicon_name = NULL;
  free(game_history->ld_name);
  game_history->ld_name = NULL;
  free(game_history->board_layout_name);
  game_history->board_layout_name = board_layout_get_default_name();
  for (int i = 0; i < 2; i++) {
    game_history_player_destroy(game_history->players[i]);
    game_history->players[i] = NULL;
  }
  game_history->game_variant = GAME_VARIANT_CLASSIC;
  game_history->number_of_events = 0;
}

GameHistory *game_history_create(void) {
  GameHistory *game_history = calloc_or_die(sizeof(GameHistory), 1);
  game_history->events = calloc_or_die(sizeof(GameEvent), (MAX_GAME_EVENTS));
  game_history_reset(game_history);
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
  for (int i = 0; i < MAX_GAME_EVENTS; i++) {
    game_event_reset(&game_history->events[i]);
  }
  free(game_history->events);
  free(game_history);
}

GameEvent *game_history_create_and_add_game_event(GameHistory *game_history,
                                                  ErrorStack *error_stack) {
  if (game_history->number_of_events == MAX_GAME_EVENTS) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_GAME_EVENT_OVERFLOW,
        get_formatted_string("exceeded the maximum number of game events: %d",
                             MAX_GAME_EVENTS));
    return NULL;
  }
  GameEvent *game_event =
      &game_history->events[game_history->number_of_events++];
  game_event_reset(game_event);
  return game_event;
}
