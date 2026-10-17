#ifndef RACK_LIST_H
#define RACK_LIST_H

#include "../ent/encoded_rack.h"
#include "../ent/klv.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"

typedef struct RackList RackList;

// Creates a RackList enumerating every full (RACK_SIZE-tile) rack for ld,
// each starting with a count of 0. If num_forced_racks is 0, every rack is
// eligible to be drawn as rare (rack_list_get_rare_rack) until it reaches
// target_rack_count, as leavegen normally expects. Otherwise, forced_racks
// is an array of num_forced_racks rack strings (each a full RACK_SIZE
// rack): only those racks are placed in the rare partition, so only they
// are ever drawn, while every other rack still starts at count 0 and can
// still be recorded via rack_list_add_rack (e.g. for organically-played
// racks) without affecting eligibility. The same restriction is reapplied
// on every rack_list_reset. Pushes an error and returns NULL if a forced
// rack isn't a full rack or is listed twice.
RackList *rack_list_create(const LetterDistribution *ld, int target_rack_count,
                           const char *const *forced_racks,
                           int num_forced_racks, ErrorStack *error_stack);
void rack_list_destroy(RackList *rack_list);
void rack_list_reset(RackList *rack_list, int target_rack_count);
void rack_list_add_rack(RackList *rack_list, const Rack *rack, double equity);
// Sets a rack's observation count and mean equity outright, in place of
// replaying the observations that produced them.
//
// rack_list_add_rack takes one game's equity at a time and folds it into a
// running mean, which is what a leavegen run has. birdtest's server has the
// other thing: millions of rows of (rack, occurrence_count, equity_sum)
// aggregated from a whole generation's contributions, with the individual
// games long since discarded. Replaying them is not possible and would not be
// wanted -- rack_list_write_to_klv reads only the count and the mean.
//
// This deliberately does not touch the rare-rack partition. A rack list built
// to convert aggregates into a KLV never draws a rack, and moving the
// partition here would cost a lock per row for a structure nothing goes on to
// read.
//
// Pushes an error if `rack` is not a full RACK_SIZE rack, or if that rack has
// already been set since the last rack_list_mark_all_racks_unset.
void rack_list_set_rack_count_and_mean(RackList *rack_list, const Rack *rack,
                                       uint64_t count, double mean,
                                       ErrorStack *error_stack);
// Marks every rack as not yet set, so that
// rack_list_set_rack_count_and_mean can tell a rack observed zero times from
// one the caller never mentioned. The two are not the same: a rack with a
// count of zero contributes a mean of zero at full draw weight to every leave
// it contains, while a rack nobody supplied is a hole in the input. Without
// this they would be indistinguishable, and a KLV derived from a file missing
// a million racks would look exactly like one derived from a complete file.
void rack_list_mark_all_racks_unset(RackList *rack_list);
// How many racks rack_list_mark_all_racks_unset marked and nothing has set
// since. Zero is the only acceptable answer before writing a KLV.
int rack_list_get_number_of_unset_racks(const RackList *rack_list);
void rack_list_write_to_klv(RackList *rack_list, const LetterDistribution *ld,
                            KLV *klv);
// {"racks":[{"rack","count","mean"}, ...]}, one entry per rack actually
// observed. Caller owns the returned string.
char *rack_list_get_rack_equity_json(const RackList *rack_list,
                                     const LetterDistribution *ld);
bool rack_list_get_rare_rack(RackList *rack_list, XoshiroPRNG *prng,
                             Rack *rack);
int rack_list_get_target_rack_count(const RackList *rack_list);
int rack_list_get_racks_below_target_count(const RackList *rack_list);
int rack_list_get_number_of_racks(const RackList *rack_list);
uint64_t rack_list_get_count(const RackList *rack_list, int klv_index);
double rack_list_get_mean(const RackList *rack_list, int klv_index);
const EncodedRack *rack_list_get_encoded_rack(const RackList *rack_list,
                                              int klv_index);
const KLV *rack_list_get_klv(const RackList *rack_list);

#endif
