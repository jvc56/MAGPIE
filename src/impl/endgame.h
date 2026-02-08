#ifndef ENDGAME_H
#define ENDGAME_H

// Note: The endgame solver currently only supports RACK_SIZE <= 7 and
// BOARD_DIM <= 16. The rack-size limitation comes from SmallMove, which
// encodes up to 7 tiles in a 64-bit integer for memory efficiency during
// search. The board-size limitation comes from MoveUndo's 16-bit
// tiles_placed_mask.

#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
#include "../ent/small_move_arena.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"

enum {
  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE = 1024 * 1024,
};

// ValueMap: open-addressing hash table for storing ground truth position values.
// Used to record values from a shallow search and inject them as move estimates
// in a deeper search, with optional Gaussian noise.
typedef struct ValueMapEntry {
  uint64_t key;   // parent_hash ^ tiny_move
  int32_t value;  // negamax value (from the child's side-to-move perspective)
  bool occupied;
} ValueMapEntry;

typedef struct ValueMap {
  ValueMapEntry *entries;
  uint32_t capacity;
  uint32_t count;
} ValueMap;

ValueMap *value_map_create(uint32_t capacity);
void value_map_destroy(ValueMap *vm);
void value_map_store(ValueMap *vm, uint64_t key, int32_t value);
// Returns true if found, false otherwise. Writes value to *out_value.
bool value_map_lookup(const ValueMap *vm, uint64_t key, int32_t *out_value);

typedef struct EndgameSolver EndgameSolver;

// Callback for per-ply PV reporting during iterative deepening
// Parameters: depth, value (spread delta), pv_line, game, user_data
typedef void (*EndgamePerPlyCallback)(int depth, int32_t value,
                                      const struct PVLine *pv_line,
                                      const struct Game *game, void *user_data);

typedef struct EndgameArgs {
  ThreadControl *thread_control;
  const Game *game;
  double tt_fraction_of_mem;
  int plies;
  int initial_small_move_arena_size;
  int num_threads;
  EndgamePerPlyCallback per_ply_callback;
  void *per_ply_callback_data;
} EndgameArgs;

EndgameSolver *endgame_solver_create(void);
void endgame_solve(EndgameSolver *solver, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack);
void endgame_solver_destroy(EndgameSolver *es);

// Run a shallow search and record ground truth position values into a ValueMap.
// The caller owns the returned ValueMap and must destroy it.
ValueMap *endgame_record_ground_truth(EndgameSolver *solver,
                                      const EndgameArgs *endgame_args);

// Set a ValueMap to be used for move ordering estimates.
// The solver does NOT take ownership; caller must keep it alive.
void endgame_solver_set_value_map(EndgameSolver *solver, ValueMap *vm);

// Set Gaussian noise standard deviation for value map estimates.
// 0.0 = perfect estimates, larger = noisier.
void endgame_solver_set_estimate_noise(EndgameSolver *solver, double stddev);

void string_builder_endgame_results(StringBuilder *pv_description,
                                    const EndgameResults *results,
                                    const Game *game,
                                    const GameHistory *game_history,
                                    bool add_line_breaks);
char *endgame_results_get_string(const EndgameResults *results,
                                 const Game *game,
                                 const GameHistory *game_history,
                                 bool add_line_breaks);
#endif
