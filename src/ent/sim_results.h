#ifndef SIM_RESULTS_H
#define SIM_RESULTS_H

#include "../compat/cpthread.h"
#include "../def/bai_defs.h"
#include "../def/game_defs.h"
#include "../def/sim_defs.h"
#include "bai_result.h"
#include "game.h"
#include "move.h"
#include "stats.h"
#include "win_pct.h"
#include <stdbool.h>

typedef struct PlyInfo {
  Stat *score_stat;
  Stat *bingo_stat;
} PlyInfo;

typedef struct SimmedPlay {
  Move *move;
  Stat *equity_stat;
  Stat *leftover_stat;
  Stat *win_pct_stat;
  uint64_t similarity_key;
  int unsorted_play_index;
  XoshiroPRNG *prng;
  PlyInfo *ply_infos;
  cpthread_mutex_t mutex;
} SimmedPlay;

Move *simmed_play_get_move(const SimmedPlay *simmed_play);
Stat *simmed_play_get_score_stat(const SimmedPlay *simmed_play, int stat_index);
Stat *simmed_play_get_bingo_stat(const SimmedPlay *simmed_play, int stat_index);
Stat *simmed_play_get_equity_stat(const SimmedPlay *simmed_play);
Stat *simmed_play_get_win_pct_stat(const SimmedPlay *simmed_play);
int simmed_play_get_unsorted_play_index(const SimmedPlay *simmed_play);
uint64_t simmed_play_get_seed(SimmedPlay *simmed_play);
void simmed_play_add_score_stat(SimmedPlay *simmed_play, Equity score,
                                bool is_bingo, int ply);
void simmed_play_add_equity_stat(SimmedPlay *simmed_play, Equity initial_spread,
                                 Equity spread, Equity leftover);
double simmed_play_add_win_pct_stat(const WinPct *wp, SimmedPlay *simmed_play,
                                    Equity spread, Equity leftover,
                                    game_end_reason_t game_end_reason,
                                    int game_unseen_tiles, bool plies_are_odd);
const PlyInfo *simmed_play_get_ply_info(const SimmedPlay *simmed_play,
                                        int index);
typedef struct SimResults SimResults;

SimResults *sim_results_create(void);
void sim_results_reset(const MoveList *move_list, SimResults *sim_results,
                       int num_plies, uint64_t seed);
void sim_results_destroy(SimResults *sim_results);

int sim_results_get_number_of_plays(const SimResults *sim_results);
int sim_results_get_num_plies(const SimResults *sim_results);
uint64_t sim_results_get_node_count(const SimResults *sim_results);
void sim_results_increment_node_count(SimResults *sim_results);
uint64_t sim_results_get_iteration_count(const SimResults *sim_results);
void sim_results_increment_iteration_count(SimResults *sim_results);
SimmedPlay *sim_results_get_simmed_play(const SimResults *sim_results,
                                        int index);
const Rack *sim_results_get_rack(const SimResults *sim_results);
void sim_results_set_rack(SimResults *sim_results, const Rack *rack);
BAIResult *sim_results_get_bai_result(const SimResults *sim_results);

void sim_results_lock_simmed_plays(SimResults *sim_results);
void sim_results_unlock_simmed_plays(SimResults *sim_results);
void sim_results_get_nth_best_move(const SimResults *sim_results, int n,
                                   Move *move);
void sim_results_set_valid_for_current_game_state(SimResults *sim_results,
                                                  bool valid);
bool sim_results_get_valid_for_current_game_state(
    const SimResults *sim_results);
bool sim_results_lock_and_sort_display_simmed_plays(SimResults *sim_results);
void sim_results_unlock_display_infos(SimResults *sim_results);
SimmedPlay *sim_results_get_display_simmed_play(const SimResults *sim_results,
                                                int index);
bool sim_results_plays_are_similar(const SimResults *sim_results, int i, int j);

#endif