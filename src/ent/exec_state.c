#include "exec_state.h"

#include <stdbool.h>
#include <stdlib.h>

#include "autoplay_results.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "inference_results.h"
#include "sim_results.h"
#include "validated_move.h"

#include "../util/util.h"

struct ExecState {
  Config *config;
  Game *game;
  MoveList *move_list;
  SimResults *sim_results;
  InferenceResults *inference_results;
  AutoplayResults *autoplay_results;
  ErrorStatus *error_status;
};

ExecState *exec_state_create() {
  ExecState *exec_state = malloc_or_die(sizeof(ExecState));
  exec_state->config = config_create_default();
  exec_state->game = NULL;
  exec_state->move_list = NULL;
  exec_state->sim_results = sim_results_create();
  exec_state->inference_results = inference_results_create();
  exec_state->autoplay_results = autoplay_results_create();
  exec_state->error_status = error_status_create();
  return exec_state;
}

void exec_state_destroy(ExecState *exec_state) {
  if (!exec_state) {
    return;
  }
  config_destroy(exec_state->config);
  game_destroy(exec_state->game);
  move_list_destroy(exec_state->move_list);
  sim_results_destroy(exec_state->sim_results);
  inference_results_destroy(exec_state->inference_results);
  autoplay_results_destroy(exec_state->autoplay_results);
  error_status_destroy(exec_state->error_status);
  free(exec_state);
}

Config *exec_state_get_config(const ExecState *exec_state) {
  return exec_state->config;
}

Game *exec_state_get_game(const ExecState *exec_state) {
  return exec_state->game;
}

MoveList *exec_state_get_move_list(const ExecState *exec_state) {
  return exec_state->move_list;
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
    game_destroy(exec_state->game);
    exec_state->game = NULL;
  }

  if (!exec_state->game) {
    exec_state->game = game_create(exec_state->config);
  } else {
    game_update(exec_state->config, exec_state->game);
  }
}

void exec_state_reset_move_list(ExecState *exec_state) {
  if (exec_state->move_list) {
    move_list_reset(exec_state->move_list);
  }
}

void exec_state_init_move_list(ExecState *exec_state, int capacity) {
  if (!exec_state->move_list) {
    exec_state->move_list = move_list_create(capacity);
  }
}

void exec_state_recreate_move_list(ExecState *exec_state, int capacity) {
  exec_state_init_move_list(exec_state, capacity);
  if (move_list_get_capacity(exec_state->move_list) == capacity) {
    move_list_reset(exec_state->move_list);
  } else {
    move_list_destroy(exec_state->move_list);
    exec_state->move_list = move_list_create(capacity);
  }
}