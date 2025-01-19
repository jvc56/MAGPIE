#include "player.h"

#include <stdlib.h>

#include "../def/move_defs.h"

#include "equity.h"

#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "players_data.h"

#include "../util/util.h"

struct Player {
  // All const fields are owned
  // by the caller and are
  // treated as read-only
  int index;
  const char *name;
  Rack *rack;
  Equity score;
  move_sort_t move_sort_type;
  move_record_t move_record_type;
  const KWG *kwg;
  const KLV *klv;
};

void player_reset(Player *player) {
  rack_reset(player->rack);
  player->score = EQUITY_ZERO_VALUE;
}

void player_update(const PlayersData *players_data, Player *player) {
  player->name = players_data_get_name(players_data, player->index);
  player->move_sort_type =
      players_data_get_move_sort_type(players_data, player->index);
  player->move_record_type =
      players_data_get_move_record_type(players_data, player->index);
  player->kwg = players_data_get_kwg(players_data, player->index);
  player->klv = players_data_get_klv(players_data, player->index);
}

Player *player_create(const PlayersData *players_data,
                      const LetterDistribution *ld, int player_index) {
  Player *player = malloc_or_die(sizeof(Player));
  player->index = player_index;
  player->score = EQUITY_ZERO_VALUE;
  player->rack = rack_create(ld_get_size(ld));

  player_update(players_data, player);

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

Equity player_get_score(const Player *player) { return player->score; }

move_sort_t player_get_move_sort_type(const Player *player) {
  return player->move_sort_type;
}

move_record_t player_get_move_record_type(const Player *player) {
  return player->move_record_type;
}

const KWG *player_get_kwg(const Player *player) { return player->kwg; }

const KLV *player_get_klv(const Player *player) { return player->klv; }

void player_set_score(Player *player, Equity score) { player->score = score; }

void player_add_to_score(Player *player, Equity score) {
  player->score += score;
}

void player_set_move_sort_type(Player *player, move_sort_t move_sort_type) {
  player->move_sort_type = move_sort_type;
}

void player_set_move_record_type(Player *player,
                                 move_record_t move_record_type) {
  player->move_record_type = move_record_type;
}