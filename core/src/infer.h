#ifndef INFER_h
#define INFER_h

#include "game.h"
#include "klv.h"
#include "move.h"
#include "rack.h"

typedef struct Inference {
    // Records
    int status;
    int distribution_size;
    uint64_t total_draws;
    uint64_t total_leaves;
    int draw_and_leave_subtotals_size;
    int * draw_and_leave_subtotals;

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

void infer(Inference * inference, Game * game, Rack * actual_tiles_played, int player_to_infer_index, int actual_score, float equity_margin);
Inference * create_inference(int distribution_size);
void destroy_inference(Inference * inference);
int get_subtotal(Inference * inference, uint8_t letter, int number_of_letters, int subtotal_index_offset);
int get_subtotal_sum_with_minimum(Inference * inference, uint8_t letter, int minimum_number_of_letters, int subtotal_index);

#endif