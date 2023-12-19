#include "exec_state.h"

#include <stdlib.h>

#include "../util/util.h"

#include "autoplay_results.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "inference_results.h"
#include "sim_results.h"

#include "../impl/move_gen.h"

struct ExecState {
  Config *config;
  Game *game;
  MoveGen *gen;
  SimResults *sim_results;
  InferenceResults *inference_results;
  AutoplayResults *autoplay_results;
  ErrorStatus *error_status;
};

ExecState *create_exec_state() {
  ExecState *exec_state = malloc_or_die(sizeof(ExecState));
  exec_state->game = NULL;
  exec_state->gen = NULL;
  exec_state->sim_results = NULL;
  exec_state->inference_results = NULL;
  exec_state->autoplay_results = NULL;
  exec_state->config = create_default_config();
  exec_state->error_status = create_error_status();
  return exec_state;
}

void destroy_exec_state(ExecState *exec_state) {
  if (exec_state->game) {
    destroy_game(exec_state->game);
  }
  if (exec_state->gen) {
    destroy_generator(exec_state->gen);
  }
  if (exec_state->sim_results) {
    sim_results_destroy(exec_state->sim_results);
  }
  if (exec_state->inference_results) {
    inference_results_destroy(exec_state->inference_results);
  }
  if (exec_state->autoplay_results) {
    destroy_autoplay_results(exec_state->autoplay_results);
  }
  destroy_config(exec_state->config);
  destroy_error_status(exec_state->error_status);
  free(exec_state);
}

Config *exec_state_get_config(const ExecState *exec_state) {
  return exec_state->config;
}

Game *exec_state_get_game(const ExecState *exec_state) {
  return exec_state->game;
}

MoveGen *exec_state_get_gen(const ExecState *exec_state) {
  return exec_state->gen;
}

SimResults *exec_state_get_sim_results(const ExecState *exec_state) {
  return exec_state->sim_results;
}

InferenceResults *
exec_state_get_inference_results(const ExecState *exec_state) {
  return exec_state->inference_results;
}

AutoplayResults *exec_state_get_autoplay_results(const ExecState *exec_state) {
  return exec_state->autoplay_results;
}

ErrorStatus *exec_state_get_error_status(const ExecState *exec_state) {
  return exec_state->error_status;
}

void exec_state_set_config(ExecState *exec_state, Config *config) {
  exec_state->config = config;
}

void exec_state_set_game(ExecState *exec_state, Game *game) {
  exec_state->game = game;
}

void exec_state_set_gen(ExecState *exec_state, MoveGen *gen) {
  exec_state->gen = gen;
}

void exec_state_set_sim_results(ExecState *exec_state,
                                SimResults *sim_results) {
  exec_state->sim_results = sim_results;
}

void exec_state_set_inference_results(ExecState *exec_state,
                                      InferenceResults *inference_results) {
  exec_state->inference_results = inference_results;
}

void exec_state_set_autoplay_results(ExecState *exec_state,
                                     AutoplayResults *autoplay_results) {
  exec_state->autoplay_results = autoplay_results;
}

void exec_state_set_error_status(ExecState *exec_state,
                                 ErrorStatus *error_status) {
  exec_state->error_status = error_status;
}