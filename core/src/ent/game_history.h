#ifndef GAME_HISTORY_H
#define GAME_HISTORY_H

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"

#include "letter_distribution.h"
#include "move.h"
#include "rack.h"

struct GameEvent;
typedef struct GameEvent GameEvent;

void game_event_set_type(GameEvent *event, game_event_t event_type);
game_event_t game_event_get_type(const GameEvent *event);

void game_event_set_player_index(GameEvent *event, int player_index);
int game_event_get_player_index(const GameEvent *event);

void game_event_set_cumulative_score(GameEvent *event, int cumulative_score);
int game_event_get_cumulative_score(const GameEvent *event);

void game_event_set_rack(GameEvent *event, Rack *rack);
Rack *game_event_get_rack(const GameEvent *event);

void game_event_set_move(GameEvent *event, Move *move);
Move *game_event_get_move(const GameEvent *event);

void game_event_set_note(GameEvent *event, const char *note);
const char *game_event_get_note(const GameEvent *event);

void game_event_set_score(GameEvent *event, int score);

GameEvent *create_game_event();
void destroy_game_event(GameEvent *game_event);

struct GameHistoryPlayer;
typedef struct GameHistoryPlayer GameHistoryPlayer;

void set_game_history_player_name(GameHistoryPlayer *player, const char *name);
const char *get_game_history_player_name(const GameHistoryPlayer *player);

void set_game_history_player_nickname(GameHistoryPlayer *player,
                                      const char *nickname);
const char *get_game_history_player_nickname(const GameHistoryPlayer *player);

void set_game_history_player_score(GameHistoryPlayer *player, int score);
int get_game_history_player_score(const GameHistoryPlayer *player);

void set_game_history_player_last_known_rack(GameHistoryPlayer *player,
                                             Rack *rack);
Rack *get_game_history_player_last_known_rack(const GameHistoryPlayer *player);

struct GameHistory;
typedef struct GameHistory GameHistory;

void set_game_history_title(GameHistory *history, const char *title);
const char *get_game_history_title(const GameHistory *history);

void set_game_history_description(GameHistory *history,
                                  const char *description);
const char *get_game_history_description(const GameHistory *history);

void set_game_history_id_auth(GameHistory *history, const char *id_auth);
const char *get_game_history_id_auth(const GameHistory *history);

void set_game_history_uid(GameHistory *history, const char *uid);
const char *get_game_history_uid(const GameHistory *history);

void set_game_history_lexicon_name(GameHistory *history,
                                   const char *lexicon_name);
const char *get_game_history_lexicon_name(const GameHistory *history);

void set_game_history_letter_distribution_name(
    GameHistory *history, const char *letter_distribution_name);
const char *
get_game_history_letter_distribution_name(const GameHistory *history);

void set_game_history_game_variant(GameHistory *history,
                                   game_variant_t game_variant);
game_variant_t get_game_history_game_variant(const GameHistory *history);

void set_game_history_board_layout(GameHistory *history,
                                   board_layout_t board_layout);
board_layout_t get_game_history_board_layout(const GameHistory *history);

void set_game_history_player(GameHistory *history, int player_index,
                             GameHistoryPlayer *player);
GameHistoryPlayer *get_game_history_player(const GameHistory *history,
                                           int player_index);

void set_game_history_number_of_events(GameHistory *history,
                                       int number_of_events);
int get_game_history_number_of_events(const GameHistory *history);

void set_game_history_letter_distribution(
    GameHistory *history, LetterDistribution *letter_distribution);
LetterDistribution *
get_game_history_letter_distribution(const GameHistory *history);

GameEvent *get_game_history_event(const GameHistory *history, int event_index);

GameHistory *create_game_history();
void destroy_game_history(GameHistory *game_history);
GameHistoryPlayer *create_game_history_player(const char *name,
                                              const char *nickname);
void destroy_game_history_player(GameHistoryPlayer *player);
void set_cumulative_scores(GameHistory *game_history);
GameEvent *create_game_event_and_add_to_history(GameHistory *game_history);

#endif