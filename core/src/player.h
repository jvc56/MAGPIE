#ifndef PLAYER_H
#define PLAYER_H

#include "config.h"
#include "rack.h"

typedef struct Player {
  int index;
  char *name;
  Rack *rack;
  int score;
  StrategyParams *strategy_params;
} Player;

Player *create_player(int index, const char *name, int array_size);
Player *copy_player(Player *player);
void destroy_player(Player *player);
void reset_player(Player *player);

#endif