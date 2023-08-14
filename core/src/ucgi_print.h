#ifndef UCGI_PRINT_H
#define UCGI_PRINT_H

#include <stdio.h>

#include "game.h"
#include "sim.h"
#include "thread_control.h"

void print_ucgi_static_moves(Game *game, int nmoves,
                             ThreadControl *thread_control);
void print_ucgi_sim_stats(Simmer *simmer, Game *game, double nps,
                          int print_best_play);
void print_ucgi_inference_current_rack_index(uint64_t current_rack_index,
                                             ThreadControl *thread_control);
#endif