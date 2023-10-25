#ifndef PLAYER_H
#define PLAYER_H

#include "config.h"
#include "rack.h"

typedef struct Player {
  int index;
  // The player name is owned by the
  // config in the same manner as the
  // KWG and KLV files, which is why
  // it is const here.
  const char *name;
  Rack *rack;
  int score;
  move_sort_t move_sort_type;
  move_record_t move_record_type;
  const KWG *kwg;
  const KLV *klv;
} Player;

Player *create_player(const Config *config, int player_index, const char *name);
Player *copy_player(const Player *player);
void destroy_player(Player *player);
void reset_player(Player *player);

#endif