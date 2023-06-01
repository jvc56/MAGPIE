#ifndef INFER_h
#define INFER_h

#include "game.h"
#include "move.h"

typedef struct Inference {
    int status;
    int total_possible_leaves;
    int total_possible_distinct_leaves;
    int * leaves_including_letter;
} Inference;

Inference * infer(Game *  game, Move * move);
Inference * create_inference(int distribution_size);
void destroy_inference(Inference * inference);

#endif