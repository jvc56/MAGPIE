#ifndef PEG_NET_UTIL_H
#define PEG_NET_UTIL_H

// General helpers for the PEG-net harness (pegnet: modes): files, hashing,
// endgame and PEG calls, configs, the positions files, the net choosers' cache
// keys.

#include "../src/def/value_net_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "../src/util/io_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Creates path for writing (mode "w" or "wb"); dies if it exists or cannot be
// created, as C11's "x" modes do.
FILE *fopen_exclusive_or_die(const char *path, const char *mode);

// Turn cap for self-play games and play-outs: a guard against a game that
// never ends.
enum {
  PEG_NET_MAX_TURNS = 256,
};

// One SplitMix64 step from state value: a well-mixed 64-bit hash.
uint64_t splitmix64(uint64_t value);

// The top static-equity move for the player on turn.
const Move *top_equity_move(const Game *game, MoveList *list);

// Fixed-depth match endgames have no timer and no transposition table. Each
// call owns its search state, so neither arm can inherit another arm's cache.
int fixed_depth_endgame_move(const Game *game, Move *move, int plies);

// Both definitions used by the existing endgame tools: tile-count fraction
// and rack-point fraction. The cross-set shortcut only returns early when
// authoritative; partial multi-tile results still require move generation.
void endgame_stuck_fractions(Game *game, MoveList *moves, int seat,
                             float *count_fraction, float *point_fraction);

// mover's score minus the opponent's, in points.
double spread_for_player(const Game *game, int mover);

// The side on turn's greedy endgame choice: highest score, plus twice the
// opponent's rack when going out. Returns the greedy value; pass when no
// placement exists.
int greedy_endgame_move(Game *game, MoveList *small, Move *move, bool *out,
                        int *tiles, bool *pass);

// Experiment toggle for PR #819's best-reply shortcuts (-ebestreply): set
// PEG_NET_BEST_REPLY=0 to disable them in the harness's PEG and exact solves.
bool best_reply_shortcuts_disabled(void);

// MAGPIE's default PEG pick for the player on turn (config_fill_peg_args
// defaults, no time budget), optionally with a 1PEG chooser in place of the
// nested inner peg at one-tile nested levels. chooser2/nest_depth: the 2PEG
// drop-in at two-tile nested nodes (NULL = the staged inner peg) and
// PegArgs.nested_max_depth. only/n_only: when n_only > 0, PEG solves only
// those root moves (PegArgs.only_moves); NULL/0 = the full root move list.
// PEG2_PICK_MAX_STAGE and PEG2_PICK_BUDGET_S limit picks with two or more
// tiles in the bag; PEG_PICK_TRACE=1 traces PEG's stages to stderr. *ms is
// the solve's wall time.
void default_peg_pick_with_nets(const Game *game, int threads,
                                PegPeg1Chooser chooser, void *chooser_ctx,
                                PegPeg2Chooser chooser2, void *chooser2_ctx,
                                int nest_depth, const Move *const *only,
                                int n_only, Move *best, double *ms);

// default_peg_pick_with_nets without a 2PEG chooser, at the default nesting
// depth.
void default_peg_pick_only(const Game *game, int threads,
                           PegPeg1Chooser chooser, void *chooser_ctx,
                           const Move *const *only, int n_only, Move *best,
                           double *ms);

// default_peg_pick_only over every root move.
void default_peg_pick(const Game *game, int threads, PegPeg1Chooser chooser,
                      void *chooser_ctx, Move *best, double *ms);

// The net choosers' pick caches. Key: board letters, the on-turn rack, both
// scores, the on-turn index and the scoreless-turn count -- everything the
// networks read.
enum {
  PEG_NET_CACHE_KEY_BYTES =
      BOARD_DIM * BOARD_DIM + VALUE_NET_TILE_TYPES + 2 * 4 + 2,
  PEG_NET_CACHE_SLOTS = 4096,
};

uint64_t peg_net_cache_key(const Game *game, uint8_t *key);

// The rack info table is off: 2PEG searches sort by score, so it only adds
// cold page faults on its 1.9 GB map (leave values are unchanged without it).
Config *peg_net_config_create(void);

// Loads one positions row (peg1/peg2 layout) into root; returns the root id or
// -1 at end of file.
int peg_positions_next_root(FILE *positions, char **line, size_t *capacity,
                            Game *root, ErrorStack *errors, int bag_size,
                            int *expected, uint64_t *source_seed);

// Weighted mean, standard deviation, min and max of values[row * stride].
void weighted_stats(const double *values, int stride, const double *prob,
                    int rows, double *stats, int block);

#endif
