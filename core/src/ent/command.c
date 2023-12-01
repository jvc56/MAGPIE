#include <stdlib.h>

#include "autoplay_results.h"
#include "command.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "inference.h"
#include "simmer.h"

struct CommandVars {
  char *command;
  Config *config;
  Game *game;
  Simmer *simmer;
  Inference *inference;
  AutoplayResults *autoplay_results;
  ErrorStatus *error_status;
};

Game *get_game(const CommandVars *cmd_vars) { return cmd_vars->game; }

CommandVars *create_command_vars() {
  CommandVars *command_vars = malloc_or_die(sizeof(CommandVars));
  command_vars->command = NULL;
  command_vars->game = NULL;
  command_vars->simmer = NULL;
  command_vars->inference = NULL;
  command_vars->autoplay_results = NULL;
  command_vars->config = create_default_config();
  command_vars->error_status = create_error_status();
  return command_vars;
}

void destroy_command_vars(CommandVars *command_vars) {
  if (command_vars->game) {
    destroy_game(command_vars->game);
  }
  if (command_vars->simmer) {
    destroy_simmer(command_vars->simmer);
  }
  if (command_vars->inference) {
    destroy_inference(command_vars->inference);
  }
  if (command_vars->autoplay_results) {
    destroy_autoplay_results(command_vars->autoplay_results);
  }
  destroy_config(command_vars->config);
  destroy_error_status(command_vars->error_status);
  free(command_vars->command);
  free(command_vars);
}

void set_command(CommandVars *command_vars, const char *command) {
  free(command_vars->command);
  command_vars->command = string_duplicate(command);
}