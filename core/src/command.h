#ifndef COMMAND_H
#define COMMAND_H

#include "autoplay.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "infer.h"
#include "sim.h"
#include "string_util.h"

struct CommandVars;
typedef struct CommandVars CommandVars;

// FIXME: This is a temporary exposure of the game
// that should only used for wasm. The
// static_evaluation and score_play functions in wasm_api.c
// should be moved to game.c so the command vars do not
// have to be exposed like this.
Game *get_game(const struct CommandVars *cmd_vars);

void execute_command_sync(CommandVars *command_vars, const char *command);
void execute_command_async(CommandVars *command_vars, const char *command);
char *command_search_status(CommandVars *command_vars, bool should_halt);
CommandVars *create_command_vars();
void destroy_command_vars(CommandVars *command_vars);
void process_command(int argc, char *argv[]);

#endif