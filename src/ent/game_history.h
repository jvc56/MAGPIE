#ifndef GAME_HISTORY_H
#define GAME_HISTORY_H

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"

#include "letter_distribution.h"
#include "rack.h"
#include "validated_move.h"

typedef struct GameEvent GameEvent;

GameEvent *game_event_create();
void game_event_destroy(GameEvent *game_event);

void game_event_set_type(GameEvent *event, game_event_t event_type);
game_event_t game_event_get_type(const GameEvent *event);

void game_event_set_player_index(GameEvent *event, int player_index);
int game_event_get_player_index(const GameEvent *event);

void game_event_set_cumulative_score(GameEvent *event, int cumulative_score);
int game_event_get_cumulative_score(const GameEvent *event);

void game_event_set_rack(GameEvent *event, Rack *rack);
Rack *game_event_get_rack(const GameEvent *event);

void game_event_set_vms(GameEvent *event, ValidatedMoves *vms);
ValidatedMoves *game_event_get_vms(const GameEvent *event);

void game_event_set_note(GameEvent *event, const char *note);
const char *game_event_get_note(const GameEvent *event);

typedef struct GameHistoryPlayer GameHistoryPlayer;

GameHistoryPlayer *game_history_player_create(const char *name,
                                              const char *nickname);
void game_history_player_destroy(GameHistoryPlayer *player);

void game_history_player_set_name(GameHistoryPlayer *player, const char *name);
const char *game_history_player_get_name(const GameHistoryPlayer *player);

void game_history_player_set_nickname(GameHistoryPlayer *player,
                                      const char *nickname);
const char *game_history_player_get_nickname(const GameHistoryPlayer *player);

void game_history_player_set_score(GameHistoryPlayer *player, int score);
int game_history_player_get_score(const GameHistoryPlayer *player);

void game_history_player_set_last_known_rack(GameHistoryPlayer *player,
                                             const Rack *rack);
Rack *game_history_player_get_last_known_rack(const GameHistoryPlayer *player);

typedef struct GameHistory GameHistory;

GameHistory *game_history_create();
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

void game_history_set_player(GameHistory *history, int player_index,
                             GameHistoryPlayer *player);
GameHistoryPlayer *game_history_get_player(const GameHistory *history,
                                           int player_index);

int game_history_get_number_of_events(const GameHistory *history);

int game_history_get_number_of_turns(const GameHistory *history);

const char *game_history_get_cgp_snapshot(const GameHistory *history,
                                          int cgp_snapshot_index);
void game_history_add_cgp_snapshot(GameHistory *history, char *cgp_snapshot);

GameEvent *game_history_get_event(const GameHistory *history, int event_index);

void game_history_set_cumulative_scores(GameHistory *game_history);
GameEvent *game_history_create_and_add_game_event(GameHistory *game_history);

#endif