#ifndef RACK_INFO_TABLE_MAKER_H
#define RACK_INFO_TABLE_MAKER_H

#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack_info_table.h"
#include "../ent/wmp.h"

// Build a RackInfoTable covering every possible RACK_SIZE-tile rack in the
// given letter distribution. The KLV is used to compute leave values for
// every 2^RACK_SIZE subset of each rack. The WMP is reserved for future
// per-rack data (e.g. word existence per length) and may be NULL if not
// needed.
//
// num_threads: number of threads to use (<= 0 means single-threaded).
RackInfoTable *make_rack_info_table(const KLV *klv, const WMP *wmp,
                                    const LetterDistribution *ld,
                                    int num_threads);

#endif
