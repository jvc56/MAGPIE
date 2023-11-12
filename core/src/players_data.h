#ifndef PLAYERS_DATA_H
#define PLAYERS_DATA_H

#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "rack.h"
#include "winpct.h"

typedef enum {
  PLAYERS_DATA_TYPE_KWG,
  PLAYERS_DATA_TYPE_KLV,
  NUMBER_OF_DATA
} players_data_t;

struct PlayersData;
typedef struct PlayersData PlayersData;

void players_data_set_name(PlayersData *players_data, int player_index,
                           const char *player_name);
const char *players_data_get_name(const PlayersData *players_data,
                                  int player_index);
void players_data_set_move_sort_type(PlayersData *players_data,
                                     int player_index,
                                     move_sort_t move_sort_type);
move_sort_t players_data_get_move_sort_type(const PlayersData *players_data,
                                            int player_index);
void players_data_set_move_record_type(PlayersData *players_data,
                                       int player_index,
                                       move_record_t move_record_type);
move_record_t players_data_get_move_record_type(const PlayersData *players_data,
                                                int player_index);
void *players_data_get_data(const PlayersData *players_data,
                            players_data_t players_data_type, int player_index);
char *players_data_get_data_name(const PlayersData *players_data,
                                 players_data_t players_data_type,
                                 int player_index);
bool players_data_get_is_shared(const PlayersData *players_data,
                                players_data_t players_data_type);
KWG *players_data_get_kwg(const PlayersData *players_data, int player_index);
KLV *players_data_get_klv(const PlayersData *players_data, int player_index);
PlayersData *create_players_data();
void destroy_players_data(PlayersData *players_data);
void set_players_data(PlayersData *players_data,
                      players_data_t players_data_type,
                      const char *p1_data_name, const char *p2_data_name);
#endif
