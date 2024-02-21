#ifndef PLAYERS_DATA_H
#define PLAYERS_DATA_H

#include <stdbool.h>

#include "../def/config_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/simmer_defs.h"
#include "klv.h"
#include "kwg.h"
#include "win_pct.h"

typedef struct PlayersData PlayersData;

PlayersData *players_data_create();
void players_data_destroy(PlayersData *players_data);

const char *players_data_get_name(const PlayersData *players_data,
                                  int player_index);
move_sort_t players_data_get_move_sort_type(const PlayersData *players_data,
                                            int player_index);
move_record_t players_data_get_move_record_type(const PlayersData *players_data,
                                                int player_index);
WinPct *players_data_get_win_pcts(const PlayersData *players_data,
                                  int player_index);
int players_data_get_num_plays(const PlayersData *players_data,
                               int player_index);
int players_data_get_plies(const PlayersData *players_data, int player_index);
int players_data_get_max_iterations(const PlayersData *players_data,
                                    int player_index);
sim_stopping_condition_t players_data_get_stopping_condition(
    const PlayersData *players_data, int player_index);
char *players_data_get_data_name(const PlayersData *players_data,
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
config_load_status_t players_data_load_plies(PlayersData *players_data,
                                             const char *plies,
                                             int player_index);
config_load_status_t players_data_load_win_pct(PlayersData *players_data,
                                               const char *win_pct_name,
                                               int player_index);
config_load_status_t players_data_load_num_plays(PlayersData *players_data,
                                                 const char *num_plays,
                                                 int player_index);
config_load_status_t players_data_load_max_iterations(
    PlayersData *players_data, const char *max_iterations, int player_index);
config_load_status_t players_data_load_stopping_condition(
    PlayersData *players_data, const char *stopping_condition,
    int player_index);
void *players_data_get_data(const PlayersData *players_data,
                            players_data_t players_data_type, int player_index);
bool players_data_get_is_shared(const PlayersData *players_data,
                                players_data_t players_data_type);

void players_data_set(PlayersData *players_data,
                      players_data_t players_data_type,
                      const char *p1_data_name, const char *p2_data_name);
#endif
