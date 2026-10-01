#ifndef SIM_NOMINATION_H
#define SIM_NOMINATION_H

#include "../ent/blocking_setup_params.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../util/io_util.h"
#include "blocking_setup.h"
#include <stdbool.h>
#include <stdint.h>

// Root candidate nomination for simulation: the union of several ranked
// sources, each taking its top N, with provenance for every nominee.
//
//   static    top N by static equity without PAT
//   pat       top N by static equity with the player's PAT weights
//   blocking  top N by base equity + blocking adjustment
//   setup     top N by base equity + setup adjustment
//   exchange  up to N exchanges within exchange_margin of the top static move
//
// Blocking and setup are the pass-relative checks of blocking_setup.h,
// weighted by a lexicon's BlockingSetupParams. They are measured over a
// universe of candidates: the top universe_count static non-pass moves, the
// exchange nominees and the PAT nominees. The "base equity" of the blocking
// and setup lists is the static equity, or with check_base_pat the PAT
// equity; the two are different mechanisms (with check_base_pat, PAT's own
// defense term and the blocking adjustment both reward closing the board,
// so a move can be credited twice for one effect).
//
// Nomination only picks the root candidates. It never changes how the
// simulation's rollouts play: those follow SimArgs (for instance
// pat_rollout_disabled), whatever sources nominated the roots.
//
// Determinism: every ranking breaks ties by static rank, and the checks'
// samples come from seed alone, so the same position, settings and seed
// always give the same nominees in the same order.

typedef enum {
  SIM_NOMINATION_SOURCE_STATIC = 1 << 0,
  SIM_NOMINATION_SOURCE_PAT = 1 << 1,
  SIM_NOMINATION_SOURCE_BLOCKING = 1 << 2,
  SIM_NOMINATION_SOURCE_SETUP = 1 << 3,
  SIM_NOMINATION_SOURCE_EXCHANGE = 1 << 4,
} sim_nomination_source_t;

typedef struct SimNominationSettings {
  int static_count;
  int pat_count;
  int blocking_count;
  int setup_count;
  int exchange_quota;
  // Points below the top static move's equity an exchange may be.
  Equity exchange_margin;
  // Static non-pass moves the checks measure (with the PAT and exchange
  // nominees). Must be at least static_count when checks run.
  int universe_count;
  // Hard cap on the union; 0 for none. Over the cap, nominees are kept by
  // their best rank in any source that nominated them, ties by static rank.
  int max_candidates;
  // Rank the blocking and setup lists by PAT equity instead of static
  // equity. Requires pat_count > 0.
  bool check_base_pat;
  // Required when blocking_count or setup_count is positive.
  const BlockingSetupParams *params;
  // The checks run only when the bag (from the player's view) holds at
  // least this many tiles; below it the blocking and setup lists are empty.
  // Values below 1 mean 1: the checks need a tile to draw.
  int min_check_bag;
  uint64_t seed;
} SimNominationSettings;

// Provenance and values for one nominee. Ranks are 1-based; 0 means the
// move is not in that list (pat_rank 0 also when no PAT list was made).
// check and the adjustments are only meaningful when checked is true.
typedef struct SimNominee {
  uint32_t sources;
  int static_rank;
  int pat_rank;
  int blocking_rank;
  int setup_rank;
  Equity static_equity;
  Equity pat_equity;
  bool checked;
  // The pass-relative check of the move (points).
  BlockingSetupResult check;
  Equity blocking_adjustment;
  Equity setup_adjustment;
} SimNominee;

typedef struct SimNominationTiming {
  int64_t static_movegen_ns;
  int64_t pat_movegen_ns;
  int64_t check_ns;
  int64_t total_ns;
  int universe_count;
  int checked_count;
} SimNominationTiming;

// Scratch state and results of one nomination. Not thread safe: each thread
// uses its own nominator.
typedef struct SimNominator SimNominator;

SimNominator *sim_nominator_create(void);
void sim_nominator_destroy(SimNominator *nominator);

// Nominates candidates for the player on turn. Returns the nominee list,
// sorted by static rank, owned by the nominator and valid until its next
// call; its moves carry their static (no-PAT) equity. The list holds only a
// pass when no tile placement or exchange is legal. Pushes an error and
// returns NULL for invalid settings (a check without params, PAT nominees
// for a player without PAT weights, check_base_pat without PAT nominees, or
// a negative count).
const MoveList *sim_nominator_nominate(SimNominator *nominator,
                                       const Game *game,
                                       const SimNominationSettings *settings,
                                       ErrorStack *error_stack);

// Provenance of nominee idx of the last list.
const SimNominee *sim_nominator_get_nominee(const SimNominator *nominator,
                                            int idx);
const SimNominationTiming *
sim_nominator_get_timing(const SimNominator *nominator);

// A PlayChooserStrategy.sim_candidates_fn context that nominates with fixed
// settings, varying the check seed by call. Owns its nominator.
typedef struct SimNominationCandidateSource {
  SimNominator *nominator;
  SimNominationSettings settings;
  uint64_t calls;
} SimNominationCandidateSource;

void sim_nomination_candidate_source_init(
    SimNominationCandidateSource *source,
    const SimNominationSettings *settings);
void sim_nomination_candidate_source_cleanup(
    SimNominationCandidateSource *source);
// Matches PlayChooserStrategy.sim_candidates_fn. Returns NULL (so the chooser
// falls back to its default candidates) when nomination fails.
const MoveList *sim_nomination_candidates(void *context, const Game *game);

#endif
