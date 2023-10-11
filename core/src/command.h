#ifndef COMMAND_H
#define COMMAND_H

#include "autoplay.h"
#include "config.h"
#include "game.h"
#include "infer.h"
#include "sim.h"
#include "string_util.h"

typedef struct CommandVars {
  StringBuilder *command;
  Config *config;
  Game *game;
  Simmer *simmer;
  Inference *inference;
  AutoplayResults *autoplay_results;
  ThreadControl *thread_control;
  ErrorStatus *error_status;
  FILE *outfile;
} CommandVars;

#endif