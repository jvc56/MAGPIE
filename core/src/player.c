
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "player.h"

void reset_player(Player * player) {
    reset_rack(player->rack);
    player->score = 0;
}

Player * create_player(const char* name, int array_size) {
    Player * player = malloc(sizeof(Player));
    player->name = strdup(name);
    player->rack = create_rack(array_size);
    player->score = 0;
    return player;
}

Player * copy_player(Player * player) {
    Player * new_player = malloc(sizeof(Player));
    new_player->name = strdup(player->name);
    new_player->rack = copy_rack(player->rack);
    new_player->score = player->score;
    new_player->strategy_params = player->strategy_params;
    return new_player;
}

void destroy_player(Player * player) {
    destroy_rack(player->rack);
    free(player->name);
    free(player);
}