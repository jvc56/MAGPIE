#include "game_history.h"

#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../ent/board_layout.h"
#include "../ent/data_filepaths.h"
#include "../ent/equity.h"
#include "../ent/players_data.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "rack.h"
#include "validated_move.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
  MAX_GCG_FILENAME_ATTEMPTS = 10000,
};

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
  Rack rack;
  Rack after_event_player_on_turn_rack;
  Rack after_event_player_off_turn_rack;
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
  memset(&game_event->rack, 0, sizeof(Rack));
  memset(&game_event->after_event_player_on_turn_rack, 0, sizeof(Rack));
  memset(&game_event->after_event_player_off_turn_rack, 0, sizeof(Rack));
  validated_moves_destroy(game_event->vms);
  game_event->vms = NULL;
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

Rack *game_event_get_rack(GameEvent *event) { return &event->rack; }

const Rack *game_event_get_const_rack(const GameEvent *event) {
  return &event->rack;
}

Rack *game_event_get_after_event_player_on_turn_rack(GameEvent *event) {
  return &event->after_event_player_on_turn_rack;
}

Rack *game_event_get_after_event_player_off_turn_rack(GameEvent *event) {
  return &event->after_event_player_off_turn_rack;
}

void game_event_set_vms(GameEvent *event, ValidatedMoves *vms) {
  validated_moves_destroy(event->vms);
  event->vms = vms;
}

ValidatedMoves *game_event_get_vms(const GameEvent *event) {
  return event->vms;
}

void game_event_set_note(GameEvent *event, const char *note) {
  free(event->note);
  if (is_string_empty_or_null(note)) {
    event->note = NULL;
  } else {
    event->note = string_duplicate(note);
  }
}

const char *game_event_get_note(const GameEvent *event) { return event->note; }

bool game_event_is_move_type(const GameEvent *event) {
  return event->event_type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
         event->event_type == GAME_EVENT_EXCHANGE ||
         event->event_type == GAME_EVENT_PASS;
}

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
  // A dist size of 0 indicates that the rack has not been set
  Rack last_rack;
  // A dist size of 0 indicates that the rack has not been set
  Rack rack_to_draw_before_pass_out_game_end;
} GameHistoryPlayer;

struct GameHistory {
  char *title;
  char *description;
  char *id_auth;
  char *uid;
  char *lexicon_name;
  char *ld_name;
  char *board_layout_name;
  char *gcg_filename;
  bool user_provided_gcg_filename;
  int num_played_events;
  game_variant_t game_variant;
  GameHistoryPlayer *players[2];
  bool waiting_for_final_pass_or_challenge;
  int num_events;
  GameEvent *events;
};

GameHistoryPlayer *game_history_player_create(void) {
  return calloc_or_die(1, sizeof(GameHistoryPlayer));
}

void game_history_player_destroy(GameHistoryPlayer *player) {
  if (!player) {
    return;
  }
  free(player->name);
  free(player->nickname);
  free(player);
}

const char *game_history_player_get_name(const GameHistory *game_history,
                                         int player_index) {
  return game_history->players[player_index]->name;
}

const char *game_history_player_get_nickname(const GameHistory *game_history,
                                             int player_index) {
  return game_history->players[player_index]->nickname;
}

Rack *game_history_player_get_last_rack(GameHistory *game_history,
                                        int player_index) {
  return &game_history->players[player_index]->last_rack;
}

const Rack *
game_history_player_get_last_rack_const(const GameHistory *game_history,
                                        int player_index) {
  return &game_history->players[player_index]->last_rack;
}

Rack *game_history_player_get_rack_to_draw_before_pass_out_game_end(
    GameHistory *game_history, int player_index) {
  return &game_history->players[player_index]
              ->rack_to_draw_before_pass_out_game_end;
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

void game_history_set_waiting_for_final_pass_or_challenge(
    GameHistory *game_history, const bool waiting_for_final_pass_or_challenge) {
  game_history->waiting_for_final_pass_or_challenge =
      waiting_for_final_pass_or_challenge;
}

bool game_history_get_waiting_for_final_pass_or_challenge(
    const GameHistory *game_history) {
  return game_history->waiting_for_final_pass_or_challenge;
}

void game_history_player_reset_last_rack(GameHistory *history,
                                         int player_index) {
  GameHistoryPlayer *player = history->players[player_index];
  memset(&player->last_rack, 0, sizeof(Rack));
}

void game_history_player_reset_names(GameHistory *history, int player_index,
                                     const char *name, const char *nickname) {
  GameHistoryPlayer *player = history->players[player_index];
  if (player_index != 0 && player_index != 1) {
    log_fatal(
        "attempted to created game history player with invalid index '%d'",
        player_index);
  }
  free(player->name);
  if (name) {
    player->name = string_duplicate(name);
  } else if (player_index == 0) {
    player->name = string_duplicate(PLAYER_ONE_DEFAULT_NAME);
  } else {
    player->name = string_duplicate(PLAYER_TWO_DEFAULT_NAME);
  }
  // Nicknames must not have any whitespace, otherwise the GCG output will be
  // invalid
  free(player->nickname);
  if (nickname) {
    player->nickname = string_duplicate(nickname);
  } else {
    player->nickname = replace_whitespace_with_underscore(player->name);
  }
}

void game_history_player_reset(GameHistory *history, int player_index,
                               const char *name, const char *nickname) {
  game_history_player_reset_names(history, player_index, name, nickname);
  game_history_player_reset_last_rack(history, player_index);
}

void game_history_switch_names(GameHistory *history) {
  char *temp = history->players[0]->name;
  history->players[0]->name = history->players[1]->name;
  history->players[1]->name = temp;
  temp = history->players[0]->nickname;
  history->players[0]->nickname = history->players[1]->nickname;
  history->players[1]->nickname = temp;
}

int game_history_get_num_events(const GameHistory *history) {
  return history->num_events;
}

int game_history_get_num_played_events(const GameHistory *game_history) {
  return game_history->num_played_events;
}

GameEvent *game_history_get_event(const GameHistory *history, int event_index) {
  return &history->events[event_index];
}

void string_builder_add_gcg_filename(StringBuilder *sb,
                                     const GameHistory *game_history,
                                     const int i) {
  if (i == 0) {
    string_builder_add_formatted_string(
        sb, "%s-vs-%s%s", game_history_player_get_nickname(game_history, 0),
        game_history_player_get_nickname(game_history, 1), GCG_EXTENSION);
  } else {
    string_builder_add_formatted_string(
        sb, "%s-vs-%s-%d%s", game_history_player_get_nickname(game_history, 0),
        game_history_player_get_nickname(game_history, 1), i, GCG_EXTENSION);
  }
}

void game_history_set_gcg_filename(GameHistory *game_history,
                                   const char *user_provided_gcg_filename) {
  if (user_provided_gcg_filename) {
    // The user has explicitly passed in a GCG filename
    game_history->user_provided_gcg_filename = true;
    free(game_history->gcg_filename);
    game_history->gcg_filename = string_duplicate(user_provided_gcg_filename);
    return;
  }
  if (game_history->user_provided_gcg_filename) {
    // The user has not passed in a GCG filename, but they have already done so
    // before, so do not overwrite the current user provided GCG filename
    // with the default GCG filename.
    return;
  }
  StringBuilder *sb = string_builder_create();
  string_builder_add_gcg_filename(sb, game_history, 0);
  for (int i = 0; access(string_builder_peek(sb), F_OK) == 0 &&
                  i < MAX_GCG_FILENAME_ATTEMPTS;
       i++) {
    string_builder_clear(sb);
    string_builder_add_gcg_filename(sb, game_history, i + 1);
  }
  free(game_history->gcg_filename);
  game_history->gcg_filename = string_duplicate(string_builder_peek(sb));
  string_builder_destroy(sb);
}

const char *game_history_get_gcg_filename(const GameHistory *game_history) {
  return game_history->gcg_filename;
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
  free(game_history->gcg_filename);
  game_history->gcg_filename = NULL;
  game_history->user_provided_gcg_filename = false;
  for (int i = 0; i < 2; i++) {
    game_history_player_reset(game_history, i, NULL, NULL);
  }
  game_history->waiting_for_final_pass_or_challenge = false;
  game_history->game_variant = GAME_VARIANT_CLASSIC;
  game_history->num_events = 0;
  game_history->num_played_events = 0;
}

GameHistory *game_history_create(void) {
  GameHistory *game_history = calloc_or_die(sizeof(GameHistory), 1);
  game_history->events = calloc_or_die(sizeof(GameEvent), (MAX_GAME_EVENTS));
  game_history->players[0] = game_history_player_create();
  game_history->players[1] = game_history_player_create();
  game_history_reset(game_history);
  return game_history;
}

GameHistory *game_history_duplicate(const GameHistory *gh_orig) {
  GameHistory *gh_copy = malloc_or_die(sizeof(GameHistory));
  gh_copy->events = malloc_or_die(sizeof(GameEvent) * (MAX_GAME_EVENTS));
  gh_copy->title = string_duplicate_allow_null(gh_orig->title);
  gh_copy->description = string_duplicate_allow_null(gh_orig->description);
  gh_copy->id_auth = string_duplicate_allow_null(gh_orig->id_auth);
  gh_copy->uid = string_duplicate_allow_null(gh_orig->uid);
  gh_copy->lexicon_name = string_duplicate_allow_null(gh_orig->lexicon_name);
  gh_copy->ld_name = string_duplicate_allow_null(gh_orig->ld_name);
  gh_copy->board_layout_name =
      string_duplicate_allow_null(gh_orig->board_layout_name);
  gh_copy->gcg_filename = string_duplicate_allow_null(gh_orig->gcg_filename);
  gh_copy->user_provided_gcg_filename = gh_orig->user_provided_gcg_filename;
  gh_copy->game_variant = gh_orig->game_variant;
  gh_copy->num_events = gh_orig->num_events;
  gh_copy->num_played_events = gh_orig->num_played_events;
  gh_copy->waiting_for_final_pass_or_challenge =
      gh_orig->waiting_for_final_pass_or_challenge;
  for (int i = 0; i < 2; i++) {
    gh_copy->players[i] = malloc_or_die(sizeof(GameHistoryPlayer));
    gh_copy->players[i]->name =
        string_duplicate_allow_null(gh_orig->players[i]->name);
    gh_copy->players[i]->nickname =
        string_duplicate_allow_null(gh_orig->players[i]->nickname);
    rack_copy(&gh_copy->players[i]->last_rack, &gh_orig->players[i]->last_rack);
    rack_copy(&gh_copy->players[i]->rack_to_draw_before_pass_out_game_end,
              &gh_orig->players[i]->rack_to_draw_before_pass_out_game_end);
  }
  for (int i = 0; i < MAX_GAME_EVENTS; i++) {
    GameEvent *ge_copy = &gh_copy->events[i];
    const GameEvent *ge_orig = &gh_orig->events[i];
    ge_copy->event_type = ge_orig->event_type;
    ge_copy->player_index = ge_orig->player_index;
    ge_copy->cumulative_score = ge_orig->cumulative_score;
    ge_copy->cgp_move_string =
        string_duplicate_allow_null(ge_orig->cgp_move_string);
    ge_copy->move_score = ge_orig->move_score;
    ge_copy->score_adjustment = ge_orig->score_adjustment;
    rack_copy(&ge_copy->rack, &ge_orig->rack);
    rack_copy(&ge_copy->after_event_player_on_turn_rack,
              &ge_orig->after_event_player_on_turn_rack);
    rack_copy(&ge_copy->after_event_player_off_turn_rack,
              &ge_orig->after_event_player_off_turn_rack);
    ge_copy->vms = validated_moves_duplicate(ge_orig->vms);
    ge_copy->note = string_duplicate_allow_null(ge_orig->note);
  }
  return gh_copy;
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
  free(game_history->gcg_filename);
  for (int i = 0; i < 2; i++) {
    game_history_player_destroy(game_history->players[i]);
  }
  for (int i = 0; i < MAX_GAME_EVENTS; i++) {
    game_event_reset(&game_history->events[i]);
  }
  free(game_history->events);
  free(game_history);
}

void game_history_truncate_to_played_events(GameHistory *game_history) {
  game_history->num_events = game_history->num_played_events;
}

void game_history_next(GameHistory *game_history, ErrorStack *error_stack) {
  if (game_history->num_played_events >= game_history->num_events) {
    error_stack_push(
        error_stack, ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE,
        string_duplicate(
            "already at latest position; there is no next position"));
    return;
  }
  game_history->num_played_events++;

  if (game_history->num_played_events < game_history->num_events &&
      game_event_get_type(
          &game_history->events[game_history->num_played_events]) ==
          GAME_EVENT_CHALLENGE_BONUS) {
    game_history->num_played_events++;
  }
}

void game_history_previous(GameHistory *game_history, ErrorStack *error_stack) {
  if (game_history->num_played_events <= 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE,
        string_duplicate(
            "already at earliest position; there is no previous position"));
    return;
  }
  if (game_event_get_type(
          &game_history->events[game_history->num_played_events - 1]) ==
      GAME_EVENT_CHALLENGE_BONUS) {
    game_history->num_played_events -= 2;
  } else {
    game_history->num_played_events -= 1;
  }
}

void game_history_goto(GameHistory *game_history, int num_events_to_play,
                       ErrorStack *error_stack) {
  if (num_events_to_play < 0 || num_events_to_play > game_history->num_events) {
    error_stack_push(
        error_stack, ERROR_STATUS_GAME_HISTORY_INDEX_OUT_OF_RANGE,
        get_formatted_string(
            "position %d is out of range; the latest position is %d",
            num_events_to_play, game_history->num_events));
    return;
  }
  game_history->num_played_events = num_events_to_play;
  if (game_history->num_played_events == 0) {
    return;
  }

  if (num_events_to_play < game_history->num_events &&
      game_event_get_type(&game_history->events[num_events_to_play]) ==
          GAME_EVENT_CHALLENGE_BONUS) {
    game_history->num_played_events++;
  }
}

int game_history_get_most_recent_move_event_index(
    const GameHistory *game_history) {
  if (game_history->num_events == 0) {
    return -1;
  }
  for (int i = game_history->num_played_events - 1; i >= 0; i--) {
    if (game_event_is_move_type(&game_history->events[i])) {
      return i;
    }
  }
  return -1;
}

GameEvent *game_history_add_game_event(GameHistory *game_history,
                                       ErrorStack *error_stack) {
  if (game_history->num_events == MAX_GAME_EVENTS) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_GAME_EVENT_OVERFLOW,
        get_formatted_string("exceeded the maximum number of game events (%d)",
                             MAX_GAME_EVENTS));
    return NULL;
  }
  GameEvent *game_event = &game_history->events[game_history->num_events++];
  game_event_reset(game_event);
  return game_event;
}

// Assumes that
//  - there are a nonzero number of events and played events
//  - the most recent played event is a tile placement move
void game_history_insert_challenge_bonus_game_event(
    GameHistory *game_history, const int game_event_player_index,
    const Equity score_adjustment, ErrorStack *error_stack) {
  if (game_history->num_events == MAX_GAME_EVENTS) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_GAME_EVENT_OVERFLOW,
        get_formatted_string("exceeded the maximum number of game events (%d)",
                             MAX_GAME_EVENTS));
    return;
  }

  game_history->num_events++;
  // Shift the game events over by one to make room for the new game event
  for (int i = game_history->num_played_events; i < game_history->num_events;
       i++) {
    memcpy(&game_history->events[i + 1], &game_history->events[i],
           sizeof(GameEvent));
  }
  const GameEvent *prev_tile_placement_event =
      game_history_get_event(game_history, game_history->num_played_events - 1);
  GameEvent *game_event =
      &game_history->events[game_history->num_played_events];
  game_event_reset(game_event);
  game_event_set_player_index(game_event, game_event_player_index);
  game_event_set_type(game_event, GAME_EVENT_CHALLENGE_BONUS);
  game_event_set_score_adjustment(game_event, score_adjustment);
  game_event_set_cumulative_score(
      game_event, game_event_get_cumulative_score(prev_tile_placement_event) +
                      game_event_get_score_adjustment(game_event));

  // Update the cumulative score for every event after the inserted challenge
  // bonus
  for (int i = game_history->num_played_events + 1;
       i < game_history->num_events; i++) {
    GameEvent *game_event_i = &game_history->events[i];
    if (game_event_get_player_index(game_event_i) == game_event_player_index) {
      game_event_set_cumulative_score(
          game_event_i, game_event_get_cumulative_score(game_event_i) +
                            game_event_get_score_adjustment(game_event_i));
    }
  }
}

// Assumes that
//  - there are a nonzero number of events and played events
//  - the most recent played event is a challenge bonus
void game_history_remove_challenge_bonus_game_event(GameHistory *game_history) {
  const int challenge_bonus_event_index = game_history->num_played_events - 1;
  const GameEvent *challenge_bonus_event =
      game_history_get_event(game_history, challenge_bonus_event_index);
  const int player_index = game_event_get_player_index(challenge_bonus_event);
  const Equity score_adjustment =
      game_event_get_score_adjustment(challenge_bonus_event);

  GameEvent tmp_game_event;
  memcpy(&tmp_game_event, challenge_bonus_event, sizeof(GameEvent));
  // Shift the game events over by one to subtract the challenge bonus event
  for (int i = challenge_bonus_event_index; i < game_history->num_events - 1;
       i++) {
    GameEvent *next_game_event = &game_history->events[i + 1];
    if (game_event_get_player_index(next_game_event) == player_index) {
      game_event_set_cumulative_score(
          next_game_event,
          game_event_get_cumulative_score(next_game_event) - score_adjustment);
    }
    memcpy(&game_history->events[i], next_game_event, sizeof(GameEvent));
  }

  memcpy(&game_history->events[game_history->num_events - 1], &tmp_game_event,
         sizeof(GameEvent));

  game_history->num_events--;
}

bool game_history_contains_end_rack_penalty_event(
    const GameHistory *game_history) {
  for (int i = 0; i < game_history_get_num_events(game_history); i++) {
    if (game_event_get_type(game_history_get_event(game_history, i)) ==
        GAME_EVENT_END_RACK_PENALTY) {
      return true;
    }
  }
  return false;
}

// Assumes the history is nonempty
void game_history_set_note_for_most_recent_event(
    const GameHistory *game_history, const char *note) {
  game_event_set_note(
      game_history_get_event(
          game_history, game_history_get_num_played_events(game_history) - 1),
      note);
}

const char *
game_history_get_note_for_most_recent_event(const GameHistory *game_history) {
  return game_event_get_note(game_history_get_event(
      game_history, game_history_get_num_played_events(game_history) - 1));
}