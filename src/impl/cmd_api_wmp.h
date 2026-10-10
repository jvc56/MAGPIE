#ifndef CMD_API_WMP_H
#define CMD_API_WMP_H

#include "../def/cmd_api_defs.h"
#include "../ent/word_info_table.h"
#include "wmp_maker.h"
#include <stdbool.h>

// Embedding helpers. The caller must exclusively own an idle handle until
// completion. Builders return owned tables; installation transfers ownership
// only on success. No engine search can overlap either operation.
WMP *magpie_build_wmp(Magpie *mp, int threads, WMPBuildProgress *progress);
WordInfoTable *magpie_build_wit(Magpie *mp);
// Installs matching WMP/WIT together, transferring both only on success.
bool magpie_install_wmp(Magpie *mp, WMP *wmp, WordInfoTable *wit);

#endif
