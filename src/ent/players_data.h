#ifndef PLAYERS_DATA_H
#define PLAYERS_DATA_H

#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "klv.h"
#include "kwg.h"
#include "wmp.h"
#include <stdbool.h>

typedef struct PlayersData PlayersData;

PlayersData *players_data_create(void);
void players_data_destroy(PlayersData *players_data);

move_sort_t players_data_get_move_sort_type(const PlayersData *players_data,
                                            int player_index);
move_record_t players_data_get_move_record_type(const PlayersData *players_data,
                                                int player_index);
const char *players_data_get_data_name(const PlayersData *players_data,
                                       players_data_t players_data_type,
                                       int player_index);
KWG *players_data_get_kwg(const PlayersData *players_data, int player_index);
KLV *players_data_get_klv(const PlayersData *players_data, int player_index);
WMP *players_data_get_wmp(const PlayersData *players_data, int player_index);

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
bool players_data_get_use_when_available(const PlayersData *players_data,
                                         players_data_t players_data_type,
                                         int player_index);
void players_data_set_use_when_available(PlayersData *players_data,
                                         players_data_t players_data_type,
                                         int player_index,
                                         bool use_when_available);
void players_data_set(PlayersData *players_data,
                      players_data_t players_data_type, const char *data_paths,
                      const char *p1_data_name, const char *p2_data_name,
                      ErrorStack *error_stack);
void players_data_reload(PlayersData *players_data,
                         players_data_t players_data_type,
                         const char *data_paths, ErrorStack *error_stack);

// Sets WMP data directly for both players (shared) without loading from file.
// The WMP will be marked as externally managed and won't be destroyed by
// players_data_set or config loading.
void players_data_set_wmp_direct(PlayersData *players_data, WMP *wmp);

// Check if WMP is externally managed (won't be destroyed by config loading)
bool players_data_wmp_is_external(const PlayersData *players_data);

// Destroy externally-managed WMP when done
void players_data_destroy_external_wmp(PlayersData *players_data);

#endif
