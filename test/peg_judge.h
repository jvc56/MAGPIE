#ifndef PEG_JUDGE_H
#define PEG_JUDGE_H

// Exact continuation (endgame solves with a wall-clock cap, play-out of the
// drawing turns) and the judge: every pick of a pegbudget run evaluated over
// all hidden worlds (pegnet:peg3oracle).

#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/transposition_table.h"
#include "../src/impl/endgame.h"
#include "../src/util/string_util.h"
#include "peg1_net.h"
#include "peg2_net.h"
#include <stdbool.h>

// ---- Exact continuation ----------------------------------------------------
// After a 1PEG pick is played with the stored game's actual last tile, one
// full-depth endgame_solve from the post-move position gives the final spread
// under optimal play by both sides. A pass leaves the tile in the bag; the
// player then on turn plays MAGPIE's default PEG pick (one thread, built-in
// schedule) until the bag is empty, and the endgame is then solved exactly.
// PEG_EXACT_CAP_S > 0 caps each solve's wall-clock time (the solver's
// external deadline, checked inside the search): a capped solve returns its
// last completed depth's value, is not proven, and records the depth it was
// searching (capped, depth).
typedef struct PegExactSolver {
  EndgameCtx *ctx;
  EndgameResults *results;
  TranspositionTable *tt;
  int plies;
  double cap_seconds;
  bool capped;
  int depth;
} PegExactSolver;

PegExactSolver peg_exact_create(int plies, double tt_fraction);

void peg_exact_destroy(PegExactSolver *exact);

// Value of the rest of the game for the player on turn; *proven when the
// solve stopped below the ply cap (root value proven).
int peg_exact_value(PegExactSolver *exact, const Game *game, bool *proven);

// Mover's final spread after `move` with the exact continuation. *peg_turns
// counts default-PEG turns taken while the bag still held the tile.
double peg_exact_final_spread(PegExactSolver *exact, const Game *root,
                              const Move *move, int seat, Game *game,
                              bool *proven, double *solve_ms, double *peg_ms,
                              int *peg_turns);

// Who replies after a 2PEG candidate when picks are scored: the 1PEG network
// chooser or MAGPIE's PEG.
typedef enum {
  PEG2_REPLY_NET,
  PEG2_REPLY_PEG,
} Peg2ReplyPolicy;

// Plays the turns that still draw: a one-tile bag by the reply policy, a
// two-tile bag (only after a pass) by default PEG or static top 1.
int peg2_play_drawing_turns(Game *game, Peg1NetChooser *net,
                            Peg2ReplyPolicy reply, MoveList *static_list,
                            double *reply_ms);

// Mover's final spread after a fixed-depth endgame from a bag-empty position
// (either player on turn).
double peg2_fixed_depth_label(Game *game, int mover, int plies,
                              EndgameCtx **ctx, EndgameResults *results,
                              ThreadControl *control);

// Plays the turns that still draw: a one-tile bag by the 1PEG network, a
// two-tile bag by the 2PEG chooser, anything else (a three-tile bag after a
// root pass, or a declined pick) by static top 1; with reply peg, every
// drawing turn by default PEG.
int peg3_play_drawing_turns(Game *game, Peg1NetChooser *net,
                            Peg2NetChooser *chooser, int slot_idx,
                            Peg2ReplyPolicy reply, MoveList *static_list,
                            double *reply_ms, int *static_turns);

// pegnet:peg3oracle
// Fields: positions prefix, picks (a peg3eval .peg3.csv: root, code, ...,
// quoted move; an empty quoted move picks the static rank code), output prefix,
// plies, tt fraction per thread, 1PEG first, second|-, shortlist, w_spread,
// scale, 2PEG model, 2PEG first|-, stage N, top K. PEG_JUDGE_THREADS (default
// 1) threads share each pick's worlds; PEG_EXACT_CAP_S caps each world's exact
// solve. PEG_JUDGE_SAMPLES=K scores K random deals plus the actual one
// (weight 0, or 1 with PEG_JUDGE_ACTUAL_WEIGHT=1) in place of every world.
// PEG_JUDGE_SIM=CANDS,ITERS,PLIES plays the drawing turns by a static sim for
// the player on turn (the best CANDS placements, ITERS rollouts each of PLIES
// plies, one thread, seeded from PEG_JUDGE_SEED, the root, the world and the
// turn) in place of the nets, which are then loaded but unused.
// Writes PREFIX.oracle.csv (one row per root and distinct pick: the
// codes that chose it, weighted win% (W=1, T=.5), mean final spread, counts,
// and the actual world's final) and PREFIX.worlds.csv (every world's bag,
// weight and final). Resumes by root.
void peg_judge_mode(const StringSplitter *fields);

#endif
