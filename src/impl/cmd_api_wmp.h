#ifndef CMD_API_WMP_H
#define CMD_API_WMP_H

#include "../def/cmd_api_defs.h"
#include "../ent/word_info_table.h"
#include "wmp_maker.h"
#include <stdbool.h>

// Internal embedding helpers (not exported by libmagpie). The caller must
// exclusively own an idle handle until completion. Builders return owned
// tables; installation transfers ownership only on success. No engine search
// can overlap either operation.
WMP *cmd_api_build_wmp(Magpie *mp, int threads, WMPBuildProgress *progress);
WordInfoTable *cmd_api_build_wit(Magpie *mp);
// Installs matching WMP/WIT together, transferring both only on success.
// Installation enables -wmp and -wit. Before switching lexicons without disk
// tables, issue "set -wit false -wmp false -lex NAME", then build/install the
// new matching tables. The disabled options release the installed tables.
bool cmd_api_install_wmp(Magpie *mp, WMP *wmp, WordInfoTable *wit);

#endif
