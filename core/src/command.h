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
  const char *command;
  Config *config;
  Game *game;
  Simmer *simmer;
  Inference *inference;
  AutoplayResults *autoplay_results;
  ThreadControl *thread_control;
  ErrorStatus *error_status;
  FILE *outfile;
} CommandVars;

void execute_command_sync(CommandVars *command_vars);
void execute_command_async(CommandVars *command_vars);
char *command_search_status(CommandVars *command_vars, bool should_halt);
CommandVars *create_command_vars(FILE *outfile);
void destroy_command_vars(CommandVars *command_vars);
void process_command(int argc, char *argv[]);

#endif