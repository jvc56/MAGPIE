#ifndef SIM_RESULTS_H
#define SIM_RESULTS_H

#include <pthread.h>
#include <stdbool.h>

#include "../def/bai_defs.h"
#include "../def/game_defs.h"
#include "../ent/bai_result.h"

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
bool simmed_play_get_is_epigon(const SimmedPlay *simmed_play);
void simmed_play_set_is_epigon(SimmedPlay *simmed_play);
uint64_t simmed_play_get_seed(SimmedPlay *simmed_play);
void simmed_play_add_score_stat(SimmedPlay *simmed_play, Equity score,
                                bool is_bingo, int ply);
void simmed_play_add_equity_stat(SimmedPlay *simmed_play, Equity initial_spread,
                                 Equity spread, Equity leftover);
double simmed_play_add_win_pct_stat(const WinPct *wp, SimmedPlay *simmed_play,
                                    Equity spread, Equity leftover,
                                    game_end_reason_t game_end_reason,
                                    int game_unseen_tiles, bool plies_are_odd);

typedef struct SimResults SimResults;

SimResults *sim_results_create(void);
void sim_results_reset(const MoveList *move_list, SimResults *sim_results,
                       int max_plies, uint64_t seed);
void sim_results_destroy(SimResults *sim_results);

int sim_results_get_number_of_plays(const SimResults *sim_results);
int sim_results_get_max_plies(const SimResults *sim_results);
int sim_results_get_node_count(const SimResults *sim_results);
int sim_results_get_iteration_count(const SimResults *sim_results);
SimmedPlay *sim_results_get_simmed_play(const SimResults *sim_results,
                                        const int index);
SimmedPlay *sim_results_get_sorted_simmed_play(SimResults *sim_results,
                                               int index);
BAIResult *sim_results_get_bai_result(const SimResults *sim_results);

void sim_results_set_iteration_count(SimResults *sim_results, int count);
void sim_results_lock_simmed_plays(SimResults *sim_results);
void sim_results_unlock_simmed_plays(SimResults *sim_results);
bool sim_results_sort_plays_by_win_rate(SimResults *sim_results);
void sim_results_increment_node_count(SimResults *sim_results);

#endif