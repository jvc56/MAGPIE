#include "player.h"

#include <stdlib.h>

#include "../def/move_defs.h"

#include "klv.h"
#include "kwg.h"

#include "../util/util.h"

struct Player {
  // All const fields are owned
  // by the caller and are
  // treated as read-only
  int index;
  const char *name;
  Rack *rack;
  int score;
  move_sort_t move_sort_type;
  move_record_t move_record_type;
  const KWG *kwg;
  const KLV *klv;
};

void player_reset(Player *player) {
  rack_reset(player->rack);
  player->score = 0;
}

void player_update(const Config *config, Player *player) {
  PlayersData *players_data = config_get_players_data(config);
  player->name = players_data_get_name(players_data, player->index);
  player->move_sort_type =
      players_data_get_move_sort_type(players_data, player->index);
  player->move_record_type =
      players_data_get_move_record_type(players_data, player->index);
  player->kwg = players_data_get_kwg(players_data, player->index);
  player->klv = players_data_get_klv(players_data, player->index);

  rack_destroy(player->rack);

  player->rack = rack_create(ld_get_size(config_get_ld(config)));
}

Player *player_create(const Config *config, int player_index) {
  Player *player = malloc_or_die(sizeof(Player));
  player->index = player_index;
  player->score = 0;
  player->rack = rack_create(ld_get_size(config_get_ld(config)));

  player_update(config, player);

  return player;
}

Player *player_duplicate(const Player *player) {
  Player *new_player = malloc_or_die(sizeof(Player));
  new_player->index = player->index;
  new_player->name = player->name;
  new_player->rack = rack_duplicate(player->rack);
  new_player->score = player->score;
  new_player->move_sort_type = player->move_sort_type;
  new_player->move_record_type = player->move_record_type;
  new_player->kwg = player->kwg;
  new_player->klv = player->klv;
  return new_player;
}

void player_destroy(Player *player) {
  if (!player) {
    return;
  }
  rack_destroy(player->rack);
  free(player);
}

int player_get_index(const Player *player) { return player->index; }

const char *player_get_name(const Player *player) { return player->name; }

Rack *player_get_rack(const Player *player) { return player->rack; }

int player_get_score(const Player *player) { return player->score; }

move_sort_t player_get_move_sort_type(const Player *player) {
  return player->move_sort_type;
}

move_record_t player_get_move_record_type(const Player *player) {
  return player->move_record_type;
}

const KWG *player_get_kwg(const Player *player) { return player->kwg; }

const KLV *player_get_klv(const Player *player) { return player->klv; }

void player_set_name(Player *player, const char *name) { player->name = name; }

void player_set_rack(Player *player, Rack *rack) { player->rack = rack; }

void player_set_score(Player *player, int score) { player->score = score; }

void player_increment_score(Player *player, int score) {
  player->score += score;
}

void player_decrement_score(Player *player, int score) {
  player->score -= score;
}

void player_set_move_sort_type(Player *player, move_sort_t move_sort_type) {
  player->move_sort_type = move_sort_type;
}

void player_set_move_record_type(Player *player,
                                 move_record_t move_record_type) {
  player->move_record_type = move_record_type;
}

void player_set_kwg(Player *player, const KWG *kwg) { player->kwg = kwg; }

void player_set_klv(Player *player, const KLV *klv) { player->klv = klv; }