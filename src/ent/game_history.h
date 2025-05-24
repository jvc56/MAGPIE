#ifndef GAME_HISTORY_H
#define GAME_HISTORY_H

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"

#include "letter_distribution.h"
#include "rack.h"
#include "validated_move.h"

typedef struct GameEvent GameEvent;

GameEvent *game_event_create(void);
void game_event_destroy(GameEvent *game_event);

void game_event_set_type(GameEvent *event, game_event_t event_type);
game_event_t game_event_get_type(const GameEvent *event);

void game_event_set_player_index(GameEvent *event, int player_index);
int game_event_get_player_index(const GameEvent *event);

void game_event_set_cumulative_score(GameEvent *event, Equity cumulative_score);
Equity game_event_get_cumulative_score(const GameEvent *event);

void game_event_set_score_adjustment(GameEvent *event, Equity score_adjustment);
Equity game_event_get_score_adjustment(const GameEvent *event);

void game_event_set_move_score(GameEvent *event, Equity move_score);
Equity game_event_get_move_score(const GameEvent *event);

void game_event_set_cgp_move_string(GameEvent *event, char *cgp_move_string);
const char *game_event_get_cgp_move_string(const GameEvent *event);

void game_event_set_rack(GameEvent *event, Rack *rack);
Rack *game_event_get_rack(const GameEvent *event);

void game_event_set_vms(GameEvent *event, ValidatedMoves *vms);
ValidatedMoves *game_event_get_vms(const GameEvent *event);

void game_event_set_note(GameEvent *event, const char *note);
const char *game_event_get_note(const GameEvent *event);
int game_event_get_turn_value(const GameEvent *event);

typedef struct GameHistory GameHistory;

GameHistory *game_history_create(void);
void game_history_destroy(GameHistory *game_history);

void game_history_set_title(GameHistory *history, const char *title);
const char *game_history_get_title(const GameHistory *history);

void game_history_set_description(GameHistory *history,
                                  const char *description);
const char *game_history_get_description(const GameHistory *history);

void game_history_set_id_auth(GameHistory *history, const char *id_auth);
const char *game_history_get_id_auth(const GameHistory *history);

void game_history_set_uid(GameHistory *history, const char *uid);
const char *game_history_get_uid(const GameHistory *history);

void game_history_set_lexicon_name(GameHistory *history,
                                   const char *lexicon_name);
const char *game_history_get_lexicon_name(const GameHistory *history);

void game_history_set_ld_name(GameHistory *history, const char *ld_name);
const char *game_history_get_ld_name(const GameHistory *history);

void game_history_set_game_variant(GameHistory *history,
                                   game_variant_t game_variant);
game_variant_t game_history_get_game_variant(const GameHistory *history);

void game_history_set_board_layout_name(GameHistory *history,
                                        const char *board_layout);
const char *game_history_get_board_layout_name(const GameHistory *history);

int game_history_get_number_of_events(const GameHistory *history);

GameEvent *game_history_get_event(const GameHistory *history, int event_index);

void game_history_set_cumulative_scores(GameHistory *game_history);
GameEvent *game_history_create_and_add_game_event(GameHistory *game_history,
                                                  ErrorStack *error_stack);

Game *game_history_get_game(const GameHistory *game_history);
void game_history_set_game(GameHistory *game_history, Game *game);

void game_history_player_set_name(GameHistory *game_history, int player_index,
                                  const char *name);
const char *game_history_player_get_name(const GameHistory *game_history,
                                         int player_index);

void game_history_player_set_nickname(GameHistory *game_history,
                                      int player_index, const char *nickname);
const char *game_history_player_get_nickname(const GameHistory *game_history,
                                             int player_index);
void game_history_player_set_score(GameHistory *game_history, int player_index,
                                   int score);
Equity game_history_player_get_score(const GameHistory *game_history,
                                     int player_index);

void game_history_player_set_next_rack_set(GameHistory *game_history,
                                           int player_index,
                                           bool next_rack_set);
bool game_history_player_get_next_rack_set(const GameHistory *game_history,
                                           int player_index);

void game_history_player_set_last_known_rack(GameHistory *game_history,
                                             int player_index,
                                             const Rack *rack);
Rack *game_history_player_get_last_known_rack(const GameHistory *game_history,
                                              int player_index);
void game_history_set_player(GameHistory *history, int player_index,
                             const char *player_name,
                             const char *player_nickname);
bool game_history_player_is_set(const GameHistory *game_history,
                                int player_index);
bool game_history_both_players_are_set(const GameHistory *game_history);

#endif