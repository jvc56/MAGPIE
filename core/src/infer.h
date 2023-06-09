#ifndef INFER_h
#define INFER_h

#include "game.h"
#include "klv.h"
#include "leave_rack.h"
#include "move.h"
#include "rack.h"
#include "stats.h"

typedef struct InferenceRecord {
    Rack * rack_leave;
    Stat * equity_values;
    int * draw_and_leave_subtotals;
    LeaveRackList * leave_rack_list;
} InferenceRecord;

typedef struct Inference {
    // InferenceRecords
    InferenceRecord * leave_record;
    InferenceRecord * exchanged_record;
    InferenceRecord * rack_record;

    // Recursive vars
    // Malloc'd by the game:
    Game * game;
    KLV * klv;
    Rack * player_to_infer_rack;
    // Malloc'd by inference:
    Rack * bag_as_rack;
    int distribution_size;
    int player_to_infer_index;
    int actual_score;
    int number_of_tiles_exchanged;
    int draw_and_leave_subtotals_size;
    float equity_margin;
} Inference;

int infer(Inference * inference, Game * game, Rack * actual_tiles_played, int player_to_infer_index, int actual_score, int number_of_tiles_exchanged, float equity_margin);
Inference * create_inference(int distribution_size);
void destroy_inference(Inference * inference);
int get_subtotal(InferenceRecord * record, uint8_t letter, int number_of_letters, int subtotal_index_offset);
int get_subtotal_sum_with_minimum(InferenceRecord * record, uint8_t letter, int minimum_number_of_letters, int subtotal_index_offset);
void get_stat_for_letter(InferenceRecord * record, Stat * stat, uint8_t letter);
double get_probability_for_random_minimum_draw(Rack * bag_as_rack, Rack * rack, uint8_t this_letter, int minimum, int number_of_actual_tiles_played);
int choose(int n, int k);

#endif