#ifndef COMMAND_H
#define COMMAND_H

#include "autoplay.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "infer.h"
#include "sim.h"
#include "string_util.h"

typedef struct CommandVars {
  char *command;
  Config *config;
  Game *game;
  Simmer *simmer;
  Inference *inference;
  AutoplayResults *autoplay_results;
  ErrorStatus *error_status;
} CommandVars;

void execute_command_sync(CommandVars *command_vars, const char *command);
void execute_command_async(CommandVars *command_vars, const char *command);
char *command_search_status(CommandVars *command_vars, bool should_halt);
CommandVars *create_command_vars();
void destroy_command_vars(CommandVars *command_vars);
void process_command(int argc, char *argv[]);

#endif