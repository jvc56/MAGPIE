
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "player.h"
#include "util.h"

void reset_player(Player *player) {
  reset_rack(player->rack);
  player->score = 0;
}

void update_player(const Config *config, Player *player) {
  player->name = players_data_get_name(config->players_data, player->index);
  player->move_sort_type =
      players_data_get_move_sort_type(config->players_data, player->index);
  player->move_record_type =
      players_data_get_move_record_type(config->players_data, player->index);
  player->kwg = players_data_get_kwg(config->players_data, player->index);
  player->klv = players_data_get_klv(config->players_data, player->index);
  update_or_create_rack(&player->rack, config->letter_distribution->size);
}

Player *create_player(const Config *config, int player_index) {
  Player *player = malloc_or_die(sizeof(Player));
  player->index = player_index;
  player->score = 0;
  player->rack = NULL;
  update_player(config, player);
  return player;
}

Player *copy_player(const Player *player) {
  Player *new_player = malloc_or_die(sizeof(Player));
  new_player->name = player->name;
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
  free(player);
}