#ifndef FORCE_RACK_LIST_H
#define FORCE_RACK_LIST_H

#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"
#include "../util/io_util.h"

// A fixed, immutable list of racks read from a file, one rack per line. Used
// by autoplay's "forceracksfile" mode to force-draw racks from an externally
// provided list (e.g. from a distributed leavegen coordinator) instead of
// leavegen's dynamically tracked RackList.
typedef struct ForceRackList ForceRackList;

ForceRackList *force_rack_list_create(const LetterDistribution *ld,
                                      const char *filename,
                                      ErrorStack *error_stack);
void force_rack_list_destroy(ForceRackList *force_rack_list);
void force_rack_list_get_random_rack(const ForceRackList *force_rack_list,
                                     XoshiroPRNG *prng, Rack *rack_out);

#endif
