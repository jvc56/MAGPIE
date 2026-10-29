#ifndef PEG1_NET_H
#define PEG1_NET_H

// 1PEG (one tile in the bag) networks: reply features over the mover's
// hypothetical worlds, the v3 utility nets (win and spread heads), and the 1PEG
// net chooser that PEG calls as its nested_peg1_chooser.

#include "../src/def/value_net_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "peg_world.h"
#include <stdbool.h>
#include <stdint.h>

// Reply features for stored 1PEG roots. For every candidate and every public
// hypothetical last tile, the world is built from public unseen counts only;
// the actual private split never enters these features.
enum {
  PEG1_REPLY_FEATURES = 10,
  PEG1_REPLY_ROW_HEAD = 3,
  PEG1_REPLY_ROW =
      PEG1_REPLY_ROW_HEAD + VALUE_NET_TILE_TYPES * PEG1_REPLY_FEATURES,
  PEG1_PLAYOUT_CAP = 32,
};

// v1 reply features of scenario, the post-candidate world with the opponent
// on turn, into feature[0..PEG1_REPLY_FEATURES): the opponent's greedy reply
// (value, going out, tiles, pass), the mover's immediate greedy value on the
// post-candidate board, the greedy play-out's second move, final margin and
// length. timing[1..3] accumulate milliseconds.
void peg1_reply_features(const Game *scenario, int mover, Game *work,
                         MoveList *small, Move *move, float *feature,
                         double *timing);

// Milliseconds per phase of building 1PEG inputs and scoring them.
typedef struct Peg1NetTiming {
  double movegen_ms;
  double scalar_ms;
  double world_ms;
  double opp_ms;
  double mover_ms;
  double head_ms;
} Peg1NetTiming;

// Per root, from the mover's view: public unseen counts (total = their sum)
// and, with opp, per hypothetical last tile the opponent's best plays on the
// root board in its PEG_WORLD_ROOT_LANES best lanes.
typedef struct Peg1RootContext {
  int unseen[VALUE_NET_TILE_TYPES];
  int total;
  Move lane_best[VALUE_NET_TILE_TYPES][PEG_WORLD_ROOT_LANES];
} Peg1RootContext;

// Scratch games and move lists for 1PEG features (one per thread).
typedef struct Peg1Scratch {
  Game *scenario;
  Game *opp_root;
  MoveList *best_list;
  MoveList *small;
  Move *move;
  Move *found;
} Peg1Scratch;

Peg1Scratch peg1_scratch_create(const Game *game);

void peg1_scratch_destroy(Peg1Scratch *work);

void peg1_root_context_fill(const Game *root, Peg1Scratch *work,
                            Peg1RootContext *prep, bool opp);

// ---- 1PEG reply features v2 (shared by extraction and the v3 networks) ---
// Opponent reply: the opponent's highest-scoring play on the post-candidate
// board, found incrementally. Per hypothetical world the opponent's best
// plays on the root board in its three best lanes are generated once; for a
// candidate, the best of those whose lane the candidate does not touch keeps
// its score, and only touched lanes are searched (seeded with it). Exactly
// equals a full best-scoring search. Value adds twice the mover's rack when
// the play goes out. Mover follow-up: BEST_SMALL greedy (score plus twice the
// opponent's rack when going out), optionally only for short leaves.
// Per-world v2 features of candidate cand in world tile, written to
// feature[0..PEG1_REPLY_FEATURES) in the v1 layout (playout slots stay zero).
void peg1_reply_features_v2(const Game *root, const Move *cand,
                            uint64_t touched, int tile,
                            const Peg1RootContext *prep, Peg1Scratch *work,
                            bool opp, bool mover_now, float *feature,
                            Peg1NetTiming *timing);

// ---- Utility mode (v3): win probability and spread blended for choosing --
// A v3 network has three outputs per candidate: win logit z, absolute-spread
// residual r (spread = one-ply estimate + 20 r) and a ranking score. Inputs
// are the v2 features plus threshold features: per estimate (one-ply spread
// after the opponent's reply; with mover features also a crude two-ply that
// adds the mover's follow-up when the opponent does not go out), the
// multiplicity-weighted fraction of public hypothetical worlds above each
// threshold, then mean, standard deviation, minimum and maximum. Choosing
// maximizes sim_utility_blend(sigmoid(z), spread, w_win, w_spread, scale);
// a negative w_spread selects margin mode (the ranking score).
enum {
  PEG1_UTILITY_NET_MAGIC = 0x50473155,
  PEG1_UTILITY_NET_OUTPUTS = 3,
  PEG1_THRESHOLDS = 9,
  PEG1_THRESHOLD_BLOCK = PEG1_THRESHOLDS + 4,
};

typedef struct Peg1UtilityNet {
  int inputs;
  int width;
  int top_k;
  uint32_t groups;
  int reply_index[PEG1_REPLY_FEATURES];
  int reply_count;
  int v2_inputs;
  int estimates;
  float *blob;
  const float *first;
  const float *bias;
  const float *out;
  float out_bias[PEG1_UTILITY_NET_OUTPUTS];
} Peg1UtilityNet;

// sim_utility_blend's weights for choosing (w_win is 1; a negative w_spread
// selects margin mode).
typedef struct Peg1UtilityWeights {
  double w_win;
  double w_spread;
  double scale;
} Peg1UtilityWeights;

Peg1UtilityNet *peg1_utility_net_load(const char *path);

void peg1_utility_net_destroy(Peg1UtilityNet *net);

// v3 inputs: the v2 columns (public scalars, then the mean, standard
// deviation, minimum and maximum of each reply feature over the hypothetical
// worlds, then the one-ply spread) followed by the threshold block(s), from
// the same per-world features.
void peg1_utility_net_inputs(const Peg1UtilityNet *net, const Game *root,
                             const Move *candidates, int count,
                             Peg1Scratch *work, const Peg1RootContext *prep,
                             float *inputs, Peg1NetTiming *timing);

// Head outputs and the selection value for one v3 input row. The spread head
// is a residual on the row's one-ply spread estimate (input column
// v2_inputs - 1).
double peg1_utility_net_value(const Peg1UtilityNet *net, const float *input,
                              const Peg1UtilityWeights *utility, double *win,
                              double *spread);

// v3 chooser over the static top-K (optionally a cascade: stage-1 network
// shortlists its top entries, stage-2 network rescoring them). Returns the
// chosen static rank, or -1 when no placement exists (the mover passes).
int peg1_utility_net_choose(const Peg1UtilityNet *first,
                            const Peg1UtilityNet *second, int shortlist,
                            const Peg1UtilityWeights *utility, const Game *root,
                            MoveList *pool, Move *candidates, int *count,
                            Peg1Scratch *work, float *inputs, float *inputs2,
                            double *values, double *wins, double *spreads,
                            Peg1NetTiming *timing, Peg1NetTiming *timing2);

// One PEG worker's scratch, pick cache and counters.
typedef struct Peg1NetChooserSlot {
  Peg1Scratch work;
  MoveList *pool;
  float *inputs;
  float *inputs2;
  Game *swap;
  // Open-addressing pick cache, cleared per root decision
  // (peg1_net_chooser_reset).
  uint8_t *keys;
  Move *picks;
  bool *used;
  int cached;
  int64_t calls;
  int64_t hits;
  int64_t checks;
  double ms;
} Peg1NetChooserSlot;

// The 1PEG network (cascade-u by default) picks the move of whichever player
// faces a one-tile bag, always from that player's own information: its rack
// and the public state. It serves three places:
//  - as PegArgs.nested_peg1_chooser, replacing MAGPIE's staged inner peg when
//    the opponent of a 2PEG candidate (or of a 1PEG pass) faces a 1PEG;
//  - as the opponent's reply in 2PEG training labels (peg2data);
//  - optionally as the reply policy when 2PEG picks are scored (peg2eval).
typedef struct Peg1NetChooser {
  Peg1UtilityNet *first;
  Peg1UtilityNet *second;
  int shortlist;
  Peg1UtilityWeights utility;
  bool check;
  bool cache;
  int slots;
  Peg1NetChooserSlot *slot;
} Peg1NetChooser;

// Loads the v3 nets (second "-" = no cascade) and sizes slots worker slots
// for game's board and lexicon.
Peg1NetChooser *peg1_net_chooser_create(const char *first, const char *second,
                                        int shortlist, double w_spread,
                                        double scale, const Game *game,
                                        int slots);

// Clears every slot's pick cache (once per root decision).
void peg1_net_chooser_reset(Peg1NetChooser *net);

void peg1_net_chooser_destroy(Peg1NetChooser *net);

// Network pick for the player on turn when the bag holds one tile and eight
// tiles are unseen to that player; false otherwise. With net->check, the pick
// is recomputed with the bag tile swapped for another hidden tile (from the
// other player's rack) and must not change.
bool peg1_net_chooser_pick(Peg1NetChooser *net, int slot_idx, const Game *game,
                           Move *move);

// The PegPeg1Chooser: peg1_net_chooser_pick on worker_idx's slot (ctx is the
// chooser).
bool peg1_net_chooser_callback(const Game *game, int worker_idx, void *ctx,
                               Move *move);

// Calls, hidden-tile checks and milliseconds, summed over the slots.
void peg1_net_chooser_totals(const Peg1NetChooser *net, int64_t *calls,
                             int64_t *checks, double *ms);

// Pick-cache hits, summed over the slots.
int64_t peg1_net_chooser_cache_hits(const Peg1NetChooser *net);

#endif
