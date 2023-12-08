#ifndef COMMAND_H
#define COMMAND_H

#include "autoplay_results.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "inference.h"
#include "simmer.h"

struct CommandVars;
typedef struct CommandVars CommandVars;

CommandVars *create_command_vars();
void destroy_command_vars(CommandVars *command_vars);

char *command_vars_get_command(CommandVars *command_vars);
Config *command_vars_get_config(CommandVars *command_vars);
Game *command_vars_get_game(CommandVars *command_vars);
Simmer *command_vars_get_simmer(CommandVars *command_vars);
Inference *command_vars_get_inference(CommandVars *command_vars);
AutoplayResults *command_vars_get_autoplay_results(CommandVars *command_vars);
ErrorStatus *command_vars_get_error_status(CommandVars *command_vars);

void command_vars_set_command(CommandVars *command_vars, const char *command);
void command_vars_set_config(CommandVars *command_vars, Config *config);
void command_vars_set_game(CommandVars *command_vars, Game *game);
void command_vars_set_simmer(CommandVars *command_vars, Simmer *simmer);
void command_vars_set_inference(CommandVars *command_vars,
                                Inference *inference);
void command_vars_set_autoplay_results(CommandVars *command_vars,
                                       AutoplayResults *autoplay_results);

#endif