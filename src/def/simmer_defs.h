#ifndef SIMMER_DEFS_H
#define SIMMER_DEFS_H

// We use this status type stub for consistency across
// commands. We might add more in the future.
typedef enum {
  SIM_STATUS_SUCCESS,
  SIM_STATUS_NO_MOVES,
} sim_status_t;

typedef enum {
  PLAYS_NOT_SIMILAR,
  PLAYS_SIMILAR,
  UNINITIALIZED_SIMILARITY,
  PLAYS_IDENTICAL,
} similar_plays_t;

#define STOP_COND_NONE 200.0;

#endif
