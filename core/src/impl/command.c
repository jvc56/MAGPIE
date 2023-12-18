#include "command.h"

#include <stdlib.h>

#include "../util/string_util.h"
#include "../util/util.h"

#include "../ent/autoplay_results.h"
#include "../ent/config.h"
#include "../ent/error_status.h"
#include "../ent/game.h"

#include "inference.h"
#include "move_gen.h"
#include "simmer.h"

struct CommandVars {
  char *command;
  Config *config;
  Game *game;
  MoveGen *gen;
  Simmer *simmer;
  InferenceResults *inference_results;
  AutoplayResults *autoplay_results;
  ErrorStatus *error_status;
};

CommandVars *create_command_vars() {
  CommandVars *command_vars = malloc_or_die(sizeof(CommandVars));
  command_vars->command = NULL;
  command_vars->game = NULL;
  command_vars->gen = NULL;
  command_vars->simmer = NULL;
  command_vars->inference_results = NULL;
  command_vars->autoplay_results = NULL;
  command_vars->config = create_default_config();
  command_vars->error_status = create_error_status();
  return command_vars;
}

void destroy_command_vars(CommandVars *command_vars) {
  if (command_vars->game) {
    destroy_game(command_vars->game);
  }
  if (command_vars->gen) {
    destroy_generator(command_vars->gen);
  }
  if (command_vars->simmer) {
    destroy_simmer(command_vars->simmer);
  }
  if (command_vars->inference_results) {
    inference_results_destroy(command_vars->inference_results);
  }
  if (command_vars->autoplay_results) {
    destroy_autoplay_results(command_vars->autoplay_results);
  }
  destroy_config(command_vars->config);
  destroy_error_status(command_vars->error_status);
  free(command_vars->command);
  free(command_vars);
}

char *command_vars_get_command(CommandVars *command_vars) {
  return command_vars->command;
}

Config *command_vars_get_config(CommandVars *command_vars) {
  return command_vars->config;
}

Game *command_vars_get_game(CommandVars *command_vars) {
  return command_vars->game;
}

MoveGen *command_vars_get_gen(CommandVars *command_vars) {
  return command_vars->gen;
}

Simmer *command_vars_get_simmer(CommandVars *command_vars) {
  return command_vars->simmer;
}

InferenceResults **
command_vars_get_inference_results(CommandVars *command_vars) {
  return &command_vars->inference_results;
}

AutoplayResults *command_vars_get_autoplay_results(CommandVars *command_vars) {
  return command_vars->autoplay_results;
}

ErrorStatus *command_vars_get_error_status(CommandVars *command_vars) {
  return command_vars->error_status;
}

void command_vars_set_command(CommandVars *command_vars, const char *command) {
  if (command_vars->command != NULL) {
    free(command_vars->command);
  }
  command_vars->command = string_duplicate(command);
}

void command_vars_set_config(CommandVars *command_vars, Config *config) {
  command_vars->config = config;
}

void command_vars_set_game(CommandVars *command_vars, Game *game) {
  command_vars->game = game;
}

void command_vars_set_gen(CommandVars *command_vars, MoveGen *gen) {
  command_vars->gen = gen;
}

void command_vars_set_simmer(CommandVars *command_vars, Simmer *simmer) {
  command_vars->simmer = simmer;
}

void command_vars_set_inference_results(CommandVars *command_vars,
                                        InferenceResults *inference_results) {
  command_vars->inference_results = inference_results;
}

void command_vars_set_autoplay_results(CommandVars *command_vars,
                                       AutoplayResults *autoplay_results) {
  command_vars->autoplay_results = autoplay_results;
}

void command_vars_set_error_status(CommandVars *command_vars,
                                   ErrorStatus *error_status) {
  command_vars->error_status = error_status;
}