#ifndef INFERENCE_CARRYOVER_H
#define INFERENCE_CARRYOVER_H

// Carry-over of an opponent-rack inference from one of a player's turns to
// their next (research branch).
//
// An inference of a player's move weights every rack they could have held.
// Summed by the tiles the move KEPT, and divided by the ways to draw those
// tiles from the pool unseen then, that is the likelihood of everything
// observed so far given the kept tiles. The next inference of the same player
// multiplies each rack's weight by the mean of that likelihood over the ways
// to choose which of the rack's tiles were the kept ones (plus a floor), so
// what their earlier moves said about the tiles they still hold is not thrown
// away.
//
// An InferenceCarryoverTable is one turn's likelihoods: the kept leaves with a
// positive weight, their tiles and their weights, looked up by the leave's
// KLV word index. It already includes the factor carried into that turn, so
// the latest table is the whole chain; earlier ones are only for postmortems.
//
// The table needs no adjustment for the tiles the observer has drawn since it
// was recorded. With U the pool unseen then, S a kept leave and X the tiles
// drawn since, ways(S from U) * ways(X from U - S) = ways(X from U) *
// ways(S from U - X): the posterior of S updated for the draw of X, over the
// prior of S in the pool that remains, is the recorded likelihood times a
// constant. A kept leave that can no longer be held is never looked up, as
// the racks inferred are drawn from the current pool; it is only left out of
// the floor (inference_carryover_table_get_max_within).

#include "../def/rack_defs.h"
#include "klv.h"
#include "letter_distribution.h"
#include "rack.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// One slot per KLV leave, holding a float's bits, so that inference workers
// can add to a slot without a lock.
typedef _Atomic uint32_t InferenceCarryoverSlot;

// The floor added to every rack's carry-over factor, as a fraction of the
// table's largest weight: a kept leave the earlier inference gave almost no
// weight does not rule a rack out. Not tuned.
#define INFERENCE_CARRYOVER_FLOOR_FRACTION 1e-3

typedef struct InferenceCarryoverTable InferenceCarryoverTable;

// The table of the positive slots, which were filled by an inference whose
// target kept `kept` tiles. NULL when there is nothing to carry: no positive
// slot, or kept outside 1 .. RACK_SIZE - 1 (a bingo keeps nothing).
InferenceCarryoverTable *
inference_carryover_table_create_from_slots(const InferenceCarryoverSlot *slots,
                                            const KLV *klv, int kept);
InferenceCarryoverTable *
inference_carryover_table_duplicate(const InferenceCarryoverTable *table);
void inference_carryover_table_destroy(InferenceCarryoverTable *table);

int inference_carryover_table_get_kept(const InferenceCarryoverTable *table);
uint32_t
inference_carryover_table_get_count(const InferenceCarryoverTable *table);
uint32_t
inference_carryover_table_get_num_leaves(const InferenceCarryoverTable *table);
float inference_carryover_table_get_max(const InferenceCarryoverTable *table);
// The entries, in increasing order of leave index.
uint32_t
inference_carryover_table_get_entry_index(const InferenceCarryoverTable *table,
                                          uint32_t entry_idx);
float inference_carryover_table_get_entry_weight(
    const InferenceCarryoverTable *table, uint32_t entry_idx);
void inference_carryover_table_get_entry_leave(
    const InferenceCarryoverTable *table, uint32_t entry_idx, int dist_size,
    Rack *leave);

// The weight of the leave with this KLV word index, 0 when it has none.
float inference_carryover_table_get_weight(const InferenceCarryoverTable *table,
                                           uint32_t leave_index);
// The largest weight among the leaves that can be drawn from the pool: the
// ones the target can still hold.
float inference_carryover_table_get_max_within(
    const InferenceCarryoverTable *table, const Rack *pool);
// The mean weight over the ways to draw a leave of the table's size from the
// pool: what the weights average to under the prior.
double inference_carryover_table_get_mean_within(
    const InferenceCarryoverTable *table, const Rack *pool);
// Writes the table into slots for the inference workers to read: every slot
// is zeroed and the table's entries set. There must be
// inference_carryover_table_get_num_leaves slots.
void inference_carryover_table_fill_slots(const InferenceCarryoverTable *table,
                                          InferenceCarryoverSlot *slots);

// A text file for postmortems: "# key<TAB>value" lines (kept, max, entries,
// and label when one is given), then "<leave><TAB><weight>" for each entry in
// decreasing order of weight. Returns false when the file cannot be written.
bool inference_carryover_table_write(const InferenceCarryoverTable *table,
                                     const LetterDistribution *ld,
                                     const char *label, const char *filename);
// Reads a file inference_carryover_table_write wrote. NULL when it cannot be
// read or does not fit the KLV.
InferenceCarryoverTable *
inference_carryover_table_read(const KLV *klv, const LetterDistribution *ld,
                               const char *filename);

// The chain of one target's tables for a caller with no game history (a game
// played move by move): the latest table, if the chain is unbroken.
typedef struct InferenceCarryoverChain InferenceCarryoverChain;

InferenceCarryoverChain *inference_carryover_chain_create(void);
void inference_carryover_chain_destroy(InferenceCarryoverChain *chain);
// The chain starts again: a new game, or a move of the target's that was not
// inferred. A pass by the target leaves their rack as it was and is not a
// reason to reset.
void inference_carryover_chain_reset(InferenceCarryoverChain *chain);
// The table to carry into the target's next inference, or NULL.
const InferenceCarryoverTable *
inference_carryover_chain_get_table(const InferenceCarryoverChain *chain);
// The inferences in a row behind the current table.
int inference_carryover_chain_get_length(const InferenceCarryoverChain *chain);
// After an inference of the target: the table it recorded becomes the current
// one (the chain takes ownership), or the chain resets when it is NULL.
void inference_carryover_chain_advance(InferenceCarryoverChain *chain,
                                       InferenceCarryoverTable *table);

#endif
