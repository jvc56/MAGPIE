#ifndef SIMMER_H
#define SIMMER_H

#include <pthread.h>

struct SimmedPlay;
typedef struct SimmedPlay SimmedPlay;

Move *simmed_play_get_move(const SimmedPlay *simmed_play);
Stat *simmed_play_get_score_stat(const SimmedPlay *simmed_play, int stat_index);
Stat *simmed_play_get_bingo_stat(const SimmedPlay *simmed_play, int stat_index);
Stat *simmed_play_get_equity_stat(const SimmedPlay *simmed_play);
Stat *simmed_play_get_leftover_stat(const SimmedPlay *simmed_play);
Stat *simmed_play_get_win_pct_stat(const SimmedPlay *simmed_play);
bool simmed_play_is_ignore(const SimmedPlay *simmed_play);
int simmed_play_get_id(const SimmedPlay *simmed_play);
pthread_mutex_t *simmed_play_get_mutex(const SimmedPlay *simmed_play);
bool is_simmed_play_ignore(const SimmedPlay *simmed_play);
int simmer_get_iteration_count(Simmer *simmer);

struct Simmer;
typedef struct Simmer Simmer;

Simmer *create_simmer(const Config *config);
void destroy_simmer(Simmer *simmer);
void sort_plays_by_win_rate(Simmer *simmer);
int simmer_get_node_count(Simmer *simmer);
int simmer_get_number_of_plays(Simmer *simmer);
SimmedPlay *simmer_get_simmed_play(Simmer *simmer, int simmed_play_index);
int simmer_get_max_plies(Simmer *simmer);

#endif
