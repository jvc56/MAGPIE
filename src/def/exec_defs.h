#ifndef EXEC_DEFS_H
#define EXEC_DEFS_H

#define TERMINATE_KEYWORD "quit"
#define TERMINATE_KEYWORD_ALIAS_EXIT "exit"
#define TERMINATE_KEYWORD_ALIAS_SHORT "q"

typedef enum {
  ASYNC_STOP_COMMAND_TOKEN,
  ASYNC_STATUS_COMMAND_TOKEN,
  ASYNC_SHOW_GAME_COMMAND_TOKEN,
  NUMBER_OF_ASYNC_COMMAND_TOKENS,
} async_token_t;

// FIXME: refigure these to make them more maintainable
#define ASYNC_STOP_COMMAND_STRING "stop"
#define ASYNC_STOP_COMMAND_STRING_SHORT "sto"

#define ASYNC_STATUS_COMMAND_STRING "status"
#define ASYNC_STATUS_COMMAND_STRING_SHORT "sta"

// While a command runs, "s" alone is shgame, though it is a prefix of all
// three.
#define ASYNC_SHOW_GAME_COMMAND_STRING "shgame"
#define ASYNC_SHOW_GAME_COMMAND_STRING_SHORT "s"

#endif