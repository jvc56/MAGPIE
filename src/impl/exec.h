
#ifndef EXEC_H
#define EXEC_H

#include "../ent/error_stack.h"

#include "config.h"

void execute_command_sync(Config *config, ErrorStack *error_stack,
                          const char *command);
void execute_command_async(Config *config, ErrorStack *error_stack,
                           const char *command);
char *command_search_status(Config *config, bool should_exit);
void caches_destroy(void);
void process_command(int argc, char *argv[]);

#endif
