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
    int total_possible_leaves;
    int * leaves_including_letter;

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

#endif