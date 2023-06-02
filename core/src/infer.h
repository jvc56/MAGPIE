#ifndef INFER_h
#define INFER_h

#include "game.h"
#include "move.h"
#include "rack.h"

typedef struct Inference {
    // Records
    int status;
    int total_possible_leaves;
    int * leaves_including_letter;

    // Recursive vars
    Game * game;
    Rack * player_on_turn_rack;
    Rack * bag_as_rack;
    Rack * player_leave;
    int current_node_index;
    int leave_tiles_remaining;
    int actual_score;
    float equity_margin;
} Inference;

Inference * infer(Game * game, Rack * actual_tiles_played, int actual_score, float equity_margin);
Inference * create_inference(int distribution_size);
void destroy_inference(Inference * inference);

#endif