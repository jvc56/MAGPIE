
#ifndef EXEC_H
#define EXEC_H

#include "config.h"

typedef struct CommandArgs {
  Config *config;
  ErrorStack *error_stack;
} CommandArgs;

void execute_command_sync(CommandArgs *command_args, const char *command);
void execute_command_async(CommandArgs *command_args, const char *command);
char *command_search_status(Config *config, bool should_exit);
void caches_destroy(void);
void process_command(int argc, char *argv[], FILE *errorout);

#endif
