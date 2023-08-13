#ifndef UCGI_PRINT_H
#define UCGI_PRINT_H

#include <stdio.h>

#include "game.h"
#include "sim.h"

void print_ucgi_static_moves(Game *game, int nmoves, FILE *outfile,
                             pthread_mutex_t *print_output_mutex);
void print_ucgi_sim_stats(Simmer *simmer, Game *game, double nps,
                          int print_best_play);
#endif