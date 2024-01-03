#ifndef PLAYER_H
#define PLAYER_H

#include "config.h"
#include "rack.h"

struct Player;
typedef struct Player Player;

Player *player_create(const Config *config, int player_index);
void player_destroy(Player *player);

int player_get_index(const Player *player);
const char *player_get_name(const Player *player);
Rack *player_get_rack(const Player *player);
int player_get_score(const Player *player);
move_sort_t player_get_move_sort_type(const Player *player);
move_record_t player_get_move_record_type(const Player *player);
const KWG *player_get_kwg(const Player *player);
const KLV *player_get_klv(const Player *player);

void player_set_name(Player *player, const char *name);
void player_set_rack(Player *player, Rack *rack);
void player_set_score(Player *player, int score);
void player_set_move_sort_type(Player *player, move_sort_t move_sort_type);
void player_set_move_record_type(Player *player,
                                 move_record_t move_record_type);
void player_set_kwg(Player *player, const KWG *kwg);
void player_set_klv(Player *player, const KLV *klv);
void player_increment_score(Player *player, int score);
void player_decrement_score(Player *player, int score);

void player_update(const Config *config, Player *player);
Player *player_duplicate(const Player *player);
void player_reset(Player *player);

#endif