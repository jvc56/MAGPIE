#ifndef PLAYERS_DATA_H
#define PLAYERS_DATA_H

#include <stdbool.h>

#include "../def/move_defs.h"
#include "../def/players_data_defs.h"

#include "klv.h"
#include "kwg.h"

typedef struct PlayersData PlayersData;

PlayersData *players_data_create(void);
void players_data_destroy(PlayersData *players_data);

const char *players_data_get_name(const PlayersData *players_data,
                                  int player_index);
move_sort_t players_data_get_move_sort_type(const PlayersData *players_data,
                                            int player_index);
move_record_t players_data_get_move_record_type(const PlayersData *players_data,
                                                int player_index);
const char *players_data_get_data_name(const PlayersData *players_data,
                                       players_data_t players_data_type,
                                       int player_index);
KWG *players_data_get_kwg(const PlayersData *players_data, int player_index);
KLV *players_data_get_klv(const PlayersData *players_data, int player_index);

void players_data_set_name(PlayersData *players_data, int player_index,
                           const char *player_name);
void players_data_set_move_sort_type(PlayersData *players_data,
                                     int player_index,
                                     move_sort_t move_sort_type);
void players_data_set_move_record_type(PlayersData *players_data,
                                       int player_index,
                                       move_record_t move_record_type);
void *players_data_get_data(const PlayersData *players_data,
                            players_data_t players_data_type, int player_index);
bool players_data_get_is_shared(const PlayersData *players_data,
                                players_data_t players_data_type);

void players_data_set(PlayersData *players_data,
                      players_data_t players_data_type, const char *data_path,
                      const char *p1_data_name, const char *p2_data_name);
#endif
