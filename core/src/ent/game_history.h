#ifndef GAME_HISTORY_H
#define GAME_HISTORY_H

struct GameEvent;
typedef struct GameEvent GameEvent;

GameEvent *create_game_event();
void destroy_game_event(GameEvent *game_event);

struct GameHistoryPlayer;
typedef struct GameHistoryPlayer GameHistoryPlayer;

struct GameHistory;
typedef struct GameHistory GameHistory;

GameHistory *create_game_history();
void destroy_game_history(GameHistory *game_history);
GameHistoryPlayer *create_game_history_player(const char *name,
                                              const char *nickname);
void destroy_game_history_player(GameHistoryPlayer *player);
void set_cumulative_scores(GameHistory *game_history);

#endif