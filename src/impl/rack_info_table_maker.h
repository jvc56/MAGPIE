#ifndef RACK_INFO_TABLE_MAKER_H
#define RACK_INFO_TABLE_MAKER_H

#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack_info_table.h"
#include "../ent/wmp.h"
#include <stdint.h>

// Build a RackInfoTable covering every possible RACK_SIZE-tile rack in the
// given letter distribution.
//
// The KLV is used to compute leave values for every 2^RACK_SIZE subset of
// each rack.
//
// If the WMP is non-NULL and playthrough_min_played_size <= RACK_SIZE, the
// table is also populated with a "playthrough_existence" uint32 per
// canonical subrack for every played_size in
// [playthrough_min_played_size, RACK_SIZE]. Each such uint32 is a bitmask
// of machine letters L such that the played subrack + {L} anagrams to a
// valid word of length (played_size + 1); the maker computes it by
// querying the WMP's one-blank path. Passing RACK_SIZE + 1 (or any value
// greater than RACK_SIZE) disables playthrough population entirely.
//
// num_threads: number of threads to use (<= 0 means single-threaded).
RackInfoTable *make_rack_info_table(const KLV *klv, const WMP *wmp,
                                    const LetterDistribution *ld,
                                    int num_threads,
                                    uint8_t playthrough_min_played_size);

#endif
