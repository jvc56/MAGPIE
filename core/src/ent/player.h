#ifndef PLAYER_H
#define PLAYER_H

#include "config.h"
#include "rack.h"

struct Player;
typedef struct Player Player;

Player *create_player(const Config *config, int player_index);
void update_player(const Config *config, Player *player);
Player *player_duplicate(const Player *player);
void destroy_player(Player *player);
void reset_player(Player *player);

#endif