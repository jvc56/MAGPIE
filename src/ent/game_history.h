#ifndef GAME_HISTORY_H
#define GAME_HISTORY_H

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../util/io_util.h"
#include "letter_distribution.h"
#include "rack.h"
#include "validated_move.h"

typedef struct GameEvent GameEvent;

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

Rack *game_event_get_rack(GameEvent *event);
const Rack *game_event_get_const_rack(const GameEvent *event);

Rack *game_event_get_after_event_player_on_turn_rack(GameEvent *event);
Rack *game_event_get_after_event_player_off_turn_rack(GameEvent *event);

void game_event_set_vms(GameEvent *event, ValidatedMoves *vms);
ValidatedMoves *game_event_get_vms(const GameEvent *event);

void game_event_set_note(GameEvent *event, const char *note);
const char *game_event_get_note(const GameEvent *event);
int game_event_get_turn_value(const GameEvent *event);

typedef struct GameHistory GameHistory;

GameHistory *game_history_create(void);
GameHistory *game_history_duplicate(const GameHistory *gh_orig);
void game_history_destroy(GameHistory *game_history);
void game_history_reset(GameHistory *game_history);

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

void game_history_set_waiting_for_final_pass_or_challenge(
    GameHistory *game_history, bool waiting_for_final_pass_or_challenge);
bool game_history_get_waiting_for_final_pass_or_challenge(
    const GameHistory *game_history);

int game_history_get_num_events(const GameHistory *history);
int game_history_get_num_played_events(const GameHistory *game_history);

GameEvent *game_history_get_event(const GameHistory *history, int event_index);

GameEvent *game_history_add_game_event(GameHistory *game_history,
                                       ErrorStack *error_stack);

void game_history_truncate_to_played_events(GameHistory *game_history);

void game_history_player_set_name(GameHistory *game_history, int player_index,
                                  const char *name);
const char *game_history_player_get_name(const GameHistory *game_history,
                                         int player_index);

void game_history_player_set_nickname(GameHistory *game_history,
                                      int player_index, const char *nickname);
const char *game_history_player_get_nickname(const GameHistory *game_history,
                                             int player_index);
Rack *game_history_player_get_last_rack(GameHistory *game_history,
                                        int player_index);
const Rack *
game_history_player_get_last_rack_const(const GameHistory *game_history,
                                        int player_index);
Rack *game_history_player_get_rack_to_draw_before_pass_out_game_end(
    GameHistory *game_history, int player_index);
void game_history_player_reset(GameHistory *history, int player_index,
                               const char *player_name,
                               const char *player_nickname);
int game_history_get_most_recent_move_event_index(
    const GameHistory *game_history);

void game_history_next(GameHistory *game_history, ErrorStack *error_stack);
void game_history_previous(GameHistory *game_history, ErrorStack *error_stack);
void game_history_goto(GameHistory *game_history, int num_events_to_play,
                       ErrorStack *error_stack);

void game_history_insert_challenge_bonus_game_event(GameHistory *game_history,
                                                    int player_index,
                                                    Equity score_adjustment,
                                                    ErrorStack *error_stack);
void game_history_remove_challenge_bonus_game_event(GameHistory *game_history);

void game_history_set_gcg_filename(GameHistory *game_history,
                                   const char *user_provided_gcg_filename);
const char *game_history_get_gcg_filename(const GameHistory *game_history);

#endif