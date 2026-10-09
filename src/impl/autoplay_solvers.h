#ifndef AUTOPLAY_SOLVERS_H
#define AUTOPLAY_SOLVERS_H

#include "../ent/autoplay_results.h"
#include "../ent/autoplay_solver_settings.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

// Endgame and pre-endgame (PEG) solving for a player in an autoplay game. One
// context per autoplay worker thread, reused across that worker's turns and
// games. See AutoplaySolverSettings for when each solver runs.
typedef struct AutoplaySolverCtx AutoplaySolverCtx;

AutoplaySolverCtx *autoplay_solver_ctx_create(void);
void autoplay_solver_ctx_destroy(AutoplaySolverCtx *ctx);

// Whether a player with `settings` solves the position in `game` (the player
// on turn's): the bag is empty and it solves endgames, or the bag holds
// PEG_MIN_BAG..peg_max_bag tiles and it runs PEG.
bool autoplay_solver_applies(const AutoplaySolverSettings *settings,
                             const Game *game);

// Solves the position in `game` for the player on turn and returns the move
// to play, owned by `ctx` and valid until its next solve. Only call when
// autoplay_solver_applies says so.
//
// Every solve runs with `num_threads` threads, no time limit,
// and the endgame depth / PEG schedule `settings` states. `shared_tt` is the
// run's endgame transposition table, shared by every worker's endgame and PEG
// leaf solves. `seed` seeds the solve; a nonzero seed derived from the task
// makes runs as consistent as multithreading allows, never reproducible.
//
// `run_thread_control` is the run's (NULL for none): a stop requested on it --
// the user's, or a contribute task's time limit -- stops the solve, which then
// returns NULL without an error, as it does when the run was stopped before
// the solve began. The caller is abandoning the run, and plays on without the
// solver.
//
// With `record` set, the solve's ranking is kept for the positions recorder
// (autoplay_solver_get_analysis), each play with its static equity.
//
// Pushes an error, and returns NULL, if a solve that was not stopped produced
// no move: with no time limit that is a bug, and the run should fail rather
// than fall back.
const Move *autoplay_solver_solve(AutoplaySolverCtx *ctx,
                                  const AutoplaySolverSettings *settings,
                                  Game *game, TranspositionTable *shared_tt,
                                  int num_threads, uint64_t seed,
                                  ThreadControl *run_thread_control,
                                  bool record, ErrorStack *error_stack);

// The ranking behind the last solve, for the positions recorder. Valid until
// the next solve, and only after one made with `record` set.
const SolverAnalysis *
autoplay_solver_get_analysis(const AutoplaySolverCtx *ctx);

// A nonzero seed for one solve, derived from the game's seed, the turn and the
// player, so a task's solves are seeded by the task alone.
uint64_t autoplay_solver_seed(uint64_t game_seed, int turn_number,
                              int player_index);

#endif
