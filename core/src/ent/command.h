#ifndef COMMAND_H
#define COMMAND_H

struct CommandVars;
typedef struct CommandVars CommandVars;

CommandVars *create_command_vars();
void destroy_command_vars(CommandVars *command_vars);

// FIXME: This is a temporary exposure of the game
// that should only used for wasm. The
// static_evaluation and score_play functions in wasm_api.c
// should be moved to game.c so the command vars do not
// have to be exposed like this.
Game *get_game(const struct CommandVars *cmd_vars);
void set_command(CommandVars *command_vars, const char *command);

#endif