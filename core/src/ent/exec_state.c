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
  SimResults *sim_results;
  InferenceResults *inference_results;
  AutoplayResults *autoplay_results;
  ErrorStatus *error_status;
};

ExecState *create_exec_state() {
  ExecState *exec_state = malloc_or_die(sizeof(ExecState));
  exec_state->config = create_default_config();
  exec_state->game = NULL;
  exec_state->sim_results = sim_results_create();
  exec_state->inference_results = inference_results_create();
  exec_state->autoplay_results = create_autoplay_results();
  exec_state->error_status = create_error_status();
  return exec_state;
}

void destroy_exec_state(ExecState *exec_state) {
  destroy_config(exec_state->config);
  if (exec_state->game) {
    destroy_game(exec_state->game);
  }
  sim_results_destroy(exec_state->sim_results);
  inference_results_destroy(exec_state->inference_results);
  destroy_autoplay_results(exec_state->autoplay_results);
  destroy_error_status(exec_state->error_status);
  free(exec_state);
}

Config *exec_state_get_config(const ExecState *exec_state) {
  return exec_state->config;
}

Game *exec_state_get_game(const ExecState *exec_state) {
  return exec_state->game;
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

bool is_game_recreation_required(const Config *config) {
  // If the ld changes (bag and rack size)
  // a recreation is required to resize the
  // dynamically allocated fields.
  return config_get_ld_name_changed(config);
}

// Creates a game if none exists or recreates the game
// if the ld size or other dynamically allocated
// data sizes have changed.
//
// Preconditions:
//  - The config is loaded
void exec_state_init_game(ExecState *exec_state) {
  if (exec_state->game && is_game_recreation_required(exec_state->config)) {
    destroy_game(exec_state->game);
    exec_state->game = NULL;
  }

  if (!exec_state->game) {
    exec_state->game = create_game(exec_state->config);
  } else {
    update_game(exec_state->config, exec_state->game);
  }
}