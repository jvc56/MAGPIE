
#ifndef EXEC_H
#define EXEC_H

#include "../ent/command.h"

void execute_command_sync(CommandVars *command_vars, const char *command);
void execute_command_async(CommandVars *command_vars, const char *command);
char *command_search_status(CommandVars *command_vars, bool should_halt);
void process_command(int argc, char *argv[]);

#endif
