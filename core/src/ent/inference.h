#ifndef INFERENCE_H
#define INFERENCE_H

#include "leave_rack.h"
#include "stats.h"

struct InferenceRecord;
typedef struct InferenceRecord InferenceRecord;

Stat *
inference_record_get_equity_values(const InferenceRecord *inferenceRecord);
uint64_t *inference_record_get_draw_and_leave_subtotals(
    const InferenceRecord *inferenceRecord);

struct Inference;
typedef struct Inference Inference;

Inference *create_inference();
void destroy_inference(Inference *inference);

InferenceRecord *inference_get_leave_record(const Inference *inference);
InferenceRecord *inference_get_exchanged_record(const Inference *inference);
InferenceRecord *inference_get_rack_record(const Inference *inference);
LeaveRackList *inference_get_leave_rack_list(const Inference *inference);
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

#endif