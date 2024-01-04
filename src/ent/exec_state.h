#ifndef EXEC_STATE_H
#define EXEC_STATE_H

#include "autoplay_results.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "inference_results.h"
#include "move.h"
#include "sim_results.h"

struct ExecState;
typedef struct ExecState ExecState;

ExecState *exec_state_create();
void exec_state_destroy(ExecState *exec_state);

Config *exec_state_get_config(const ExecState *exec_state);
Game *exec_state_get_game(const ExecState *exec_state);
SimResults *exec_state_get_sim_results(const ExecState *exec_state);
InferenceResults *exec_state_get_inference_results(const ExecState *exec_state);
AutoplayResults *exec_state_get_autoplay_results(const ExecState *exec_state);
ErrorStatus *exec_state_get_error_status(const ExecState *exec_state);

void exec_state_init_game(ExecState *exec_state);

#endif