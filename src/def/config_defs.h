#ifndef CONFIG_DEFS_H
#define CONFIG_DEFS_H

enum {
  DEFAULT_BINGO_BONUS = 50,
  DEFAULT_CHALLENGE_BONUS = 5,
  DEFAULT_MOVE_LIST_CAPACITY = 15,
  DEFAULT_SMALL_MOVE_LIST_CAPACITY = 250000,
};

#define DEFAULT_GAME_VARIANT GAME_VARIANT_CLASSIC
#define EMPTY_RACK_STRING "-"
#define DEFAULT_DATA_PATHS "./data"
#define DEFAULT_WIN_PCT "winpct"
#define COMMAND_FINISHED_KEYWORD "finished"
#define COMMAND_RUNNING_KEYWORD "running"

typedef enum {
  EXEC_MODE_UNKNOWN,
  EXEC_MODE_CONSOLE,
  EXEC_MODE_UCGI,
} exec_mode_t;

#endif