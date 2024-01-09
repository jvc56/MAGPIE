#ifndef SIM_RESULTS_H
#define SIM_RESULTS_H

#include <pthread.h>
#include <stdbool.h>

#include "../def/game_defs.h"

#include "move.h"
#include "stats.h"
#include "win_pct.h"

typedef struct SimmedPlay SimmedPlay;

Move *simmed_play_get_move(const SimmedPlay *simmed_play);
Stat *simmed_play_get_score_stat(const SimmedPlay *simmed_play, int stat_index);
Stat *simmed_play_get_bingo_stat(const SimmedPlay *simmed_play, int stat_index);
Stat *simmed_play_get_equity_stat(const SimmedPlay *simmed_play);
Stat *simmed_play_get_win_pct_stat(const SimmedPlay *simmed_play);
int simmed_play_get_id(const SimmedPlay *simmed_play);
bool simmed_play_get_ignore(const SimmedPlay *simmed_play);
void simmed_play_set_ignore(SimmedPlay *simmed_play, bool ignore);

void simmed_play_add_score_stat(SimmedPlay *sp, int score, bool is_bingo,
                                int ply, bool lock);
void simmed_play_add_equity_stat(SimmedPlay *sp, int initial_spread, int spread,
                                 float leftover, bool lock);
void simmed_play_add_win_pct_stat(const WinPct *wp, SimmedPlay *sp, int spread,
                                  float leftover,
                                  game_end_reason_t game_end_reason,
                                  int game_unseen_tiles, bool plies_are_odd,
                                  bool lock);

typedef struct SimResults SimResults;

SimResults *sim_results_create();
void sim_results_reset(const MoveList *move_list, SimResults *sim_results,
                       int num_simmed_plays, int max_plies);
void sim_results_destroy(SimResults *sim_results);

int sim_results_get_number_of_plays(const SimResults *sim_results);
int sim_results_get_max_plies(const SimResults *sim_results);
int sim_results_get_node_count(const SimResults *sim_results);
int sim_results_get_iteration_count(const SimResults *sim_results);
SimmedPlay *sim_results_get_simmed_play(SimResults *sim_results, int index);

void sim_results_increment_iteration_count(SimResults *sim_results);
void sim_results_lock_simmed_plays(SimResults *sim_results);
void sim_results_unlock_simmed_plays(SimResults *sim_results);
bool sim_results_sort_plays_by_win_rate(SimResults *sim_results);
void sim_results_increment_node_count(SimResults *sim_results);

#endif