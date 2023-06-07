#ifndef INFER_h
#define INFER_h

#include "game.h"
#include "klv.h"
#include "leave_rack.h"
#include "move.h"
#include "rack.h"
#include "stats.h"

typedef struct Inference {
    // Records
    int distribution_size;
    Stat * leave_values;
    int draw_and_leave_subtotals_size;
    int * draw_and_leave_subtotals;
    LeaveRackList * leave_rack_list;

    // Recursive vars
    Game * game;
    Rack * player_to_infer_rack;
    Rack * bag_as_rack;
    Rack * player_leave;
    int player_to_infer_index;
    int actual_score;
    float equity_margin;
    KLV * klv;
} Inference;

int infer(Inference * inference, Game * game, Rack * actual_tiles_played, int player_to_infer_index, int actual_score, float equity_margin);
Inference * create_inference(int distribution_size);
void destroy_inference(Inference * inference);
int get_subtotal(Inference * inference, uint8_t letter, int number_of_letters, int subtotal_index_offset);
int get_subtotal_sum_with_minimum(Inference * inference, uint8_t letter, int minimum_number_of_letters, int subtotal_index);
void get_stat_for_letter(Inference * inference, Stat * stat, uint8_t letter);
double get_probability_for_random_minimum_draw(Inference * inference, uint8_t letter, int minimum, int number_of_actual_tiles_played);
void get_stat_for_letter(Inference * inference, Stat * stat, uint8_t letter);
int choose(int n, int k);

#endif