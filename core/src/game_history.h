#ifndef GAME_HISTORY_H
#define GAME_HISTORY_H

#include <stdint.h>

#include "board.h"
#include "constants.h"
#include "game.h"
#include "game_event.h"
#include "move.h"
#include "player.h"

#define MAX_GAME_EVENTS 200

typedef struct GameHistoryPlayer {
  char *name;
  char *nickname;
  int score;
  Rack *last_known_rack;
} GameHistoryPlayer;

typedef struct GameHistory {
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
} GameHistory;

GameEvent *create_game_event_and_add_to_history(GameHistory *game_history);
GameHistory *create_game_history();
void destroy_game_history(GameHistory *game_history);
GameHistoryPlayer *create_game_history_player(const char *name,
                                              const char *nickname);
void destroy_game_history_player(GameHistoryPlayer *player);
void set_cumulative_scores(GameHistory *game_history);
Game *play_to_turn(const GameHistory *game_history, int turn_number);

#endif