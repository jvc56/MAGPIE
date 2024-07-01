
#ifndef EXEC_H
#define EXEC_H

#include "config.h"

void execute_command_sync(Config *config, const char *command);
void execute_command_async(Config *config, const char *command);
char *command_search_status(Config *config, bool should_halt);
void caches_destroy(void);
void process_command(int argc, char *argv[]);

#endif
