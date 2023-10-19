
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "player.h"
#include "util.h"

void reset_player(Player *player) {
  reset_rack(player->rack);
  player->score = 0;
}

Player *create_player(Config *config, int player_index, const char *name) {
  Player *player = malloc_or_die(sizeof(Player));
  player->index = player_index;
  player->name = strdup(name);
  player->rack = create_rack(config->letter_distribution->size);
  player->score = 0;
  player->move_sort_type =
      players_data_get_move_sort_type(config->players_data, player_index);
  player->move_record_type =
      players_data_set_move_record_type(config->players_data, player_index);
  player->kwg = players_data_get_kwg(config->players_data, player_index);
  player->klv = players_data_get_klv(config->players_data, player_index);
  return player;
}

Player *copy_player(Player *player) {
  Player *new_player = malloc_or_die(sizeof(Player));
  new_player->name = strdup(player->name);
  new_player->rack = copy_rack(player->rack);
  new_player->score = player->score;
  new_player->move_sort_type = player->move_sort_type;
  new_player->move_record_type = player->move_record_type;
  new_player->kwg = player->kwg;
  new_player->klv = player->klv;
  return new_player;
}

void destroy_player(Player *player) {
  destroy_rack(player->rack);
  free(player->name);
  free(player);
}