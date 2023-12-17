#ifndef INFERENCE_H
#define INFERENCE_H

#include "../def/inference_defs.h"

#include "../util/log.h"

#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/leave_rack.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"

struct Inference;
typedef struct Inference Inference;

Inference *create_inference();
void destroy_inference(Inference *inference);

Game *inference_get_game(const Inference *inference);
const KLV *inference_get_klv(const Inference *inference);
Rack *inference_get_player_to_infer_rack(const Inference *inference);
Rack *inference_get_bag_as_rack(const Inference *inference);
Rack *inference_get_leave(const Inference *inference);
Rack *inference_get_exchanged(const Inference *inference);
int inference_get_distribution_size(const Inference *inference);
int inference_get_player_to_infer_index(const Inference *inference);
int inference_get_actual_score(const Inference *inference);
int inference_get_number_of_tiles_exchanged(const Inference *inference);
int inference_get_draw_and_leave_subtotals_size(const Inference *inference);
int inference_get_initial_tiles_to_infer(const Inference *inference);
double inference_get_equity_margin(const Inference *inference);
uint64_t inference_get_current_rack_index(const Inference *inference);
uint64_t inference_get_total_racks_evaluated(const Inference *inference);
ThreadControl *inference_get_thread_control(const Inference *inference);
InferenceResults *inference_get_results(const Inference *inference);

double get_probability_for_random_minimum_draw(
    const Rack *bag_as_rack, const Rack *rack, uint8_t this_letter, int minimum,
    int number_of_actual_tiles_played);

uint64_t choose(uint64_t n, uint64_t k);

inference_status_t infer(const Config *config, Game *game,
                         Inference *inference);
#endif