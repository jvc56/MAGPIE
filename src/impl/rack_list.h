#ifndef RACK_LIST_H
#define RACK_LIST_H

#include "../ent/encoded_rack.h"
#include "../ent/klv.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"

typedef struct RackList RackList;

// Creates a RackList enumerating every full (RACK_SIZE-tile) rack for ld,
// each starting with a count of 0. If forced_racks_filename is NULL or
// empty, every rack is eligible to be drawn as rare (rack_list_get_rare_rack)
// until it reaches target_rack_count, as leavegen normally expects.
// Otherwise, forced_racks_filename is read as a list of racks (one full
// RACK_SIZE rack per line): only those racks are placed in the rare
// partition, so only they are ever drawn, while every other rack still
// starts at count 0 and can still be recorded via rack_list_add_rack (e.g.
// for organically-played racks) without affecting eligibility. The same
// restriction is reapplied on every rack_list_reset. Pushes an error and
// returns NULL on a missing/unopenable file, a line that isn't a full rack,
// or a file with no racks at all.
RackList *rack_list_create(const LetterDistribution *ld, int target_rack_count,
                           const char *forced_racks_filename,
                           ErrorStack *error_stack);
void rack_list_destroy(RackList *rack_list);
void rack_list_reset(RackList *rack_list, int target_rack_count);
void rack_list_add_rack(RackList *rack_list, const Rack *rack, double equity);
void rack_list_write_to_klv(RackList *rack_list, const LetterDistribution *ld,
                            KLV *klv);
// Writes one "<rack>,<count>,<mean>" line per rack that has been observed at
// least once (0 < count < target_rack_count) directly to filename.
void rack_list_write_rack_equity_csv(const RackList *rack_list,
                                     const LetterDistribution *ld,
                                     const char *filename,
                                     ErrorStack *error_stack);
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
// True if this RackList was built (or reset) with a forced_racks_filename
// restricting which racks are eligible to be drawn as rare.
bool rack_list_has_forced_racks(const RackList *rack_list);

#endif
