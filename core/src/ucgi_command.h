#ifndef UCGI_COMMAND_H
#define UCGI_COMMAND_H

#include "command.h"

char *ucgi_search_status(CommandVars *command_vars);
char *ucgi_stop_search(CommandVars *command_vars);
bool process_ucgi_command_async(CommandVars *command_vars, const char *cmd);

#endif
