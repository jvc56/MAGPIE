
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "player.h"
#include "alphabet.h"

void reset_player(Player * player) {
    reset_rack(player->rack);
    player->score = 0;
}

Player * create_player(const char* name) {
    Player * player = malloc(sizeof(Player));
    player->name = strdup(name);
    player->rack = create_rack();
    player->score = 0;
    return player;
}

void destroy_player(Player * player) {
    destroy_rack(player->rack);
    free(player->name);
    free(player);
}