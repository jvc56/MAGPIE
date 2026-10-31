#ifndef PEG2_NET_H
#define PEG2_NET_H

// 2PEG (two tiles in the bag) networks: per-world rows, the candidate nets,
// staged and wide picks, the 2PEG net chooser that PEG calls as its
// nested_peg2_chooser, and the root ranker for PegArgs.root_ranker.

#include "../src/def/letter_distribution_defs.h"
#include "../src/def/value_net_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "peg1_net.h"
#include "peg_net_util.h"
#include "peg_world.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// ---- 2PEG candidate features ------------------------------------------------
// Per-world features for a 2PEG chooser, from the mover's information only:
// the nine unseen tiles are enumerated as the possible bag pairs (the
// opponent holds the other seven), weighted by ordered-draw multiplicity; the
// actual bag is never read. Two tiers, timed separately:
//  - cheap, every candidate: the opponent's top-scoring reply (lane-
//    incremental from its three best plays on the pre-move board, as in the
//    1PEG v2 features) and the mover's greedy follow-up. After a bag-emptying
//    play the follow-up is on the post-move board; after a one-tile play it is
//    after the opponent's top-scoring reply (which draws the last tile).
//  - expensive, one-tile candidates only: the 1PEG network's pick for the
//    opponent, which then faces a 1PEG, and its predicted win probability,
//    spread and utility from the opponent's view. The opponent's information
//    is the same for both draw orders of a pair, so one call serves both.
enum {
  PEG2_FEATURE_WIDTH = 16,
  PEG2_MAX_PAIRS = VALUE_NET_TILE_TYPES * (VALUE_NET_TILE_TYPES + 1) / 2,
  PEG2_ORDERED_PAIRS = PEG2_UNSEEN * (PEG2_UNSEEN - 1),
};

// Per root, from the mover's view: public unseen counts, the possible bag
// pairs (tile_a, tile_b, count ordered draws each) and, per pair, the
// opponent's best plays on the root board in its PEG_WORLD_ROOT_LANES best
// lanes.
typedef struct Peg2RootContext {
  int unseen[VALUE_NET_TILE_TYPES];
  int pairs;
  MachineLetter tile_a[PEG2_MAX_PAIRS];
  MachineLetter tile_b[PEG2_MAX_PAIRS];
  int count[PEG2_MAX_PAIRS];
  Move lane_best[PEG2_MAX_PAIRS][PEG_WORLD_ROOT_LANES];
} Peg2RootContext;

void peg2_root_context_fill(const Game *root, Peg1Scratch *work,
                            Peg2RootContext *prep);

// The opponent's top-scoring reply in world (opponent on turn); features 1-4.
void peg2_opponent_top_reply(const Peg2RootContext *prep, int pair,
                             uint64_t touched, Game *world, int mover,
                             Peg1Scratch *work, float *feature);

// The 1PEG network's pick for the player on turn in a one-tile-bag world, with
// the predicted win probability, spread and utility of that pick from the
// picker's view (the cascade's second network when there is one).
void peg2_reply_net_value(Peg1NetChooser *net, const Game *game, Move *pick,
                          double *win, double *spread, double *value);

// ---- 2PEG cheap-tier chooser ------------------------------------------------
// The cheap-tier 2PEG chooser in native code. For each of the static top K
// placements: the per-world cheap features of peg2feat (the opponent's
// top-scoring reply and the mover's greedy follow-up, over the mover's hidden
// worlds), aggregated as in scripts/peg2_cv.py, then a one-hidden-layer
// network (normalization folded into the first layer). The best score is
// played. Only the public state and the mover's rack are read: worlds are
// enumerated from the public unseen pool, and the bag is never consulted.
// Optional staging: a first model on the opponent-reply columns only (no
// greedy follow-up, the costly half) scores all K, and the full model scores
// the first model's best N.
enum {
  PEG2_CANDIDATE_NET_MAGIC = 0x50473253,
  PEG2_NET_PUBLIC = VALUE_NET_SCALARS - 4 + 2,
  PEG2_THRESHOLDS = 9,
  PEG2_NET_WORLD_SUMMARY = 4 * 6 + 2 * (PEG2_THRESHOLDS + 4),
  PEG2_NET_INPUT = PEG2_NET_PUBLIC + 2 + PEG2_NET_WORLD_SUMMARY,
  // Opponent-reply columns: mean, sd, min and max of world columns 1-4, and
  // the one-ply estimate block.
  PEG2_NET_FIRST_STAGE_INPUT =
      PEG2_NET_PUBLIC + 2 + 4 * 4 + PEG2_THRESHOLDS + 4,
};

// A one-hidden-layer candidate net (normalization folded into the first
// layer); top_k is the static top K it was trained on.
typedef struct Peg2CandidateNet {
  int inputs;
  int width;
  int top_k;
  float *blob;
  const float *first;
  const float *bias;
  const float *out;
  float out_bias;
} Peg2CandidateNet;

Peg2CandidateNet *peg2_candidate_net_load(const char *path);

void peg2_candidate_net_destroy(Peg2CandidateNet *model);

// Nanoseconds per phase of 2PEG picks.
typedef struct Peg2PickTiming {
  int64_t pool_ns;
  int64_t prep_ns;
  int64_t opp_ns;
  int64_t greedy_ns;
  int64_t model_ns;
} Peg2PickTiming;

// Scratch for 2PEG picks (one per thread).
typedef struct Peg2PickScratch {
  Peg1Scratch peg1;
  Game *world;
  Game *base;
  MoveList *pool;
  float *rows;
  double *columns;
  double *prob;
} Peg2PickScratch;

Peg2PickScratch peg2_pick_scratch_create(const Game *game);

void peg2_pick_scratch_destroy(Peg2PickScratch *work);

// Scores the static top K of root (mover on turn, two tiles in the bag) and
// returns the static rank of the pick. Without a first model, scores[rank]
// is the full model's score of every rank and first_scores is unused. With
// one, first_scores[rank] is its score of every rank, and scores[rank] the
// full model's score of its best N (NAN elsewhere). With dump, writes each
// candidate's world rows (peg2feat layout; first-stage rows when staged).
int peg2_net_staged_pick(const Game *root, const Peg2CandidateNet *model,
                         const Peg2CandidateNet *first, int stage_n, int top_k,
                         Peg2PickScratch *work, Move *candidates, int *count,
                         int *scored, float *scores, float *first_scores,
                         Peg2PickTiming *timing, FILE *dump);

// ---- Wide 2PEG chooser -----------------------------------------------------
// The first (opponent-reply) model scores every placement of the root (wide
// ranks), not only the static top K. With a full cheap model, that model scores
// the first model's best N and its best score is played; without one, the first
// model's best is played. Ties go to the better wide rank. As in peg2pick, only
// the public state and the mover's rack are read.
int peg2_net_wide_pick(const Game *root, const Peg2CandidateNet *first,
                       const Peg2CandidateNet *model, int stage_n,
                       Peg2PickScratch *work, MoveList *wide, Move *moves,
                       bool *taken, int *count, float *first_scores,
                       float *scores, Peg2PickTiming *timing);

// One PEG worker's scratch, pick cache and counters.
typedef struct Peg2NetChooserSlot {
  Peg2PickScratch work;
  Move candidates[PEG_STATIC_POOL_SIZE];
  Move candidates2[PEG_STATIC_POOL_SIZE];
  Game *swap;
  int64_t calls;
  int64_t hits;
  int64_t checks;
  double ms;
} Peg2NetChooserSlot;

// The 2PEG net chooser: peg2_net_staged_pick with a pick cache per PEG worker
// slot. With three tiles in the bag (ten unseen to the mover), the
// native 2PEG chooser (peg2pick: an opponent-reply model ranks the static top
// K, the full model scores its best N) serves as
//  - PegArgs.nested_peg2_chooser, replacing MAGPIE's staged inner peg when the
//    opponent of a 3PEG candidate faces a 2PEG;
//  - the reply policy on two-tile-bag turns when 3PEG picks are scored
//    (peg3eval), as the 1PEG network is on one-tile-bag turns.
// Like the 1PEG chooser it reads only the on-turn player's rack and the public
// state; with check on, every pick is recomputed with the bag swapped for two
// other hidden tiles and must not change.
typedef struct Peg2NetChooser {
  Peg2CandidateNet *model;
  Peg2CandidateNet *first;
  int stage_n;
  int top_k;
  bool check;
  bool cache;
  // The slots' pick cache, cleared per root decision (peg2_net_chooser_reset).
  PegNetPickCache *pick_cache;
  int slots;
  Peg2NetChooserSlot *slot;
} Peg2NetChooser;

// first_path "-" = no first stage; top_k 0 = the model's. Sizes slots
// worker slots for game's board and lexicon.
Peg2NetChooser *peg2_net_chooser_create(const char *model_path,
                                        const char *first_path, int stage_n,
                                        int top_k, const Game *game, int slots);

// Clears every slot's pick cache (once per root decision).
void peg2_net_chooser_reset(Peg2NetChooser *chooser);

void peg2_net_chooser_destroy(Peg2NetChooser *chooser);

// The chooser's pick for the player on turn when the bag holds two tiles and
// nine tiles are unseen to that player; false otherwise. With chooser->check,
// a cached pick is recomputed, and the pick is recomputed with the bag swapped
// for two other hidden tiles; both must match.
bool peg2_net_chooser_pick(Peg2NetChooser *chooser, int slot_idx,
                           const Game *game, Move *move);

// The PegPeg2Chooser: peg2_net_chooser_pick on worker_idx's slot (ctx is the
// chooser).
bool peg2_net_chooser_callback(const Game *game, int worker_idx, void *ctx,
                               Move *move);

// Calls, hidden-tile checks, milliseconds and pick-cache hits, summed over
// the slots.
void peg2_net_chooser_totals(const Peg2NetChooser *chooser, int64_t *calls,
                             int64_t *checks, double *ms, int64_t *hits);

// The 2PEG chooser's ranking of root's static top 64 (its staged best N by
// the full model, then the rest by the first model): writes up to max moves
// in order into order[], returns how many, and the chooser's pick rank.
int peg2_net_chooser_rank(Peg2NetChooser *chooser, const Game *root,
                          Move *candidates, const Move **order, int max,
                          int *pick_rank);

// PegArgs.root_ranker over the 2PEG chooser: peg2_net_chooser_rank of the
// root's static top 64. ms is the last call's wall time.
typedef struct Peg2NetRanker {
  Peg2NetChooser *chooser;
  MoveList *pool;
  double ms;
} Peg2NetRanker;

// The PegRootRanker; ctx is a Peg2NetRanker.
int peg2_net_root_ranker(const Game *game, void *ctx, Move *moves,
                         int max_moves);

#endif
