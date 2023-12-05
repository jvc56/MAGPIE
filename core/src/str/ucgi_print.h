#ifndef UCGI_PRINT_H
#define UCGI_PRINT_H

#include <stdio.h>

#include "autoplay_results.h"
#include "game.h"
#include "inference.h"
#include "simmer.h"
#include "thread_control.h"

void print_ucgi_static_moves(const Game *game, int nmoves,
                             ThreadControl *thread_control);
void print_ucgi_sim_stats(const Game *game, Simmer *simmer,
                          bool print_best_play);
void print_ucgi_inference_current_rack(uint64_t current_rack_index,
                                       ThreadControl *thread_control);
void print_ucgi_inference_total_racks_evaluated(uint64_t total_racks_evaluated,
                                                ThreadControl *thread_control);
void print_ucgi_inference(const Inference *inference,
                          ThreadControl *thread_control);
void print_ucgi_autoplay_results(const AutoplayResults *autoplay_results,
                                 ThreadControl *thread_control);

char *ucgi_static_moves(const Game *game, int nmoves);
char *ucgi_sim_stats(const Game *game, const Simmer *simmer,
                     bool print_best_play);
#endif