#ifndef RACK_LIST_H
#define RACK_LIST_H

#include "../ent/encoded_rack.h"
#include "../ent/klv.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"

typedef struct RackList RackList;

RackList *rack_list_create(const LetterDistribution *ld, int target_rack_count);
void rack_list_destroy(RackList *rack_list);
void rack_list_reset(RackList *rack_list, int target_rack_count);
void rack_list_add_rack(RackList *rack_list, const Rack *rack, double equity);
void rack_list_write_to_klv(RackList *rack_list, const LetterDistribution *ld,
                            KLV *klv);
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