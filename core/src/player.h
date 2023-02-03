#ifndef PLAYER_H
#define PLAYER_H

#include "alphabet.h"
#include "rack.h"

typedef struct Player {
    char* name;
    Rack * rack;
    int score;
} Player;

Player * create_player(const char* name);
void destroy_player(Player * player);
void reset_player(Player * player);

#endif