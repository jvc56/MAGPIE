#ifndef EXEC_STATE_H
#define EXEC_STATE_H

#include "autoplay_results.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "inference_results.h"
#include "sim_results.h"

// FIXME: movegen will go away soon

#include "../impl/move_gen.h"

struct ExecState;
typedef struct ExecState ExecState;

ExecState *create_exec_state();
void destroy_exec_state(ExecState *exec_state);

Config *exec_state_get_config(const ExecState *exec_state);
Game *exec_state_get_game(const ExecState *exec_state);
MoveGen *exec_state_get_gen(const ExecState *exec_state);
SimResults *exec_state_get_sim_results(const ExecState *exec_state);
InferenceResults *exec_state_get_inference_results(const ExecState *exec_state);
AutoplayResults *exec_state_get_autoplay_results(const ExecState *exec_state);
ErrorStatus *exec_state_get_error_status(const ExecState *exec_state);

void exec_state_set_config(ExecState *exec_state, Config *config);
void exec_state_set_game(ExecState *exec_state, Game *game);
void exec_state_set_gen(ExecState *exec_state, MoveGen *gen);
void exec_state_set_sim_results(ExecState *exec_state, SimResults *sim_results);
void exec_state_set_inference_results(ExecState *exec_state,
                                      InferenceResults *inference_results);
void exec_state_set_autoplay_results(ExecState *exec_state,
                                     AutoplayResults *autoplay_results);
void exec_state_set_error_status(ExecState *exec_state,
                                 ErrorStatus *error_status);

#endif