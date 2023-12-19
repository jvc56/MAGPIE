
#ifndef EXEC_H
#define EXEC_H

#include "../ent/exec_state.h"

void execute_command_sync(ExecState *exec_state, const char *command);
void execute_command_async(ExecState *exec_state, const char *command);
char *command_search_status(ExecState *exec_state, bool should_halt);
void process_command(int argc, char *argv[]);

#endif
