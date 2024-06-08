#ifndef SIMMER_DEFS_H
#define SIMMER_DEFS_H

typedef enum {
  SIM_STOPPING_CONDITION_NONE,
  SIM_STOPPING_CONDITION_95PCT,
  SIM_STOPPING_CONDITION_98PCT,
  SIM_STOPPING_CONDITION_99PCT,
} sim_stopping_condition_t;

// We use this status type stub for consistency across
// commands. We might add more in the future.
typedef enum {
  SIM_STATUS_SUCCESS,
  SIM_STATUS_NO_MOVES,
  SIM_STATUS_GAME_NOT_LOADED,
  SIM_STATUS_EXCHANGE_MALFORMED_RACK,
} sim_status_t;

typedef enum {
  PLAYS_NOT_SIMILAR,
  PLAYS_SIMILAR,
  UNINITIALIZED_SIMILARITY,
  PLAYS_IDENTICAL,
} similar_plays_t;

#endif
