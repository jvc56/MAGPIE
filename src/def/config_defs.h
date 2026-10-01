#ifndef CONFIG_DEFS_H
#define CONFIG_DEFS_H

#define DEFAULT_SETTINGS_FILENAME "settings.txt"
#define MAGPIE_PROMPT "magpie>"

enum {
  DEFAULT_BINGO_BONUS = 50,
  DEFAULT_CHALLENGE_BONUS = 5,
  DEFAULT_SMALL_MOVE_LIST_CAPACITY = 250000,
};

#define DEFAULT_GAME_VARIANT GAME_VARIANT_CLASSIC
#define EMPTY_RACK_STRING "-"
#define DEFAULT_DATA_PATHS "./data"
// Without -winpct, the win percentage table is DEFAULT_WIN_PCT_PREFIX
// followed by the letter distribution's name (e.g. winpct_french).
#define DEFAULT_WIN_PCT_PREFIX "winpct_"
// Given to -winpct, returns to that per-distribution default.
#define DEFAULT_WIN_PCT_ARG "default"
// Static-ish rollouts (the rbs options): the candidates the policy weighs
// and its default race threshold (see BlockingSetupPolicySettings).
enum {
  DEFAULT_ROLLOUT_BLOCKING_SETUP_UNIVERSE = 60,
  DEFAULT_ROLLOUT_BLOCKING_SETUP_EXCHANGES = 5,
  DEFAULT_ROLLOUT_BLOCKING_SETUP_EXCHANGE_MARGIN = 35,
};
#define DEFAULT_ROLLOUT_BLOCKING_SETUP_Z 3.0
#define COMMAND_FINISHED_KEYWORD "finished"
#define COMMAND_RUNNING_KEYWORD "running"

typedef enum {
  EXEC_MODE_UNKNOWN,
  EXEC_MODE_SYNC,
  EXEC_MODE_ASYNC,
} exec_mode_t;

#define EXEC_MODE_SYNC_STRING "sync"
#define EXEC_MODE_ASYNC_STRING "async"

#endif