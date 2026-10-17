#ifndef CMD_API_WMP_H
#define CMD_API_WMP_H

#include "../def/cmd_api_defs.h"
#include "wmp_maker.h"
#include <stdbool.h>

// Internal embedding helpers (not exported by libmagpie). The caller must
// exclusively own an idle handle until completion. The builder returns an owned
// table; installation transfers ownership only on success. No engine search
// can overlap either operation.
WMP *cmd_api_build_wmp(Magpie *mp, int threads, WMPBuildProgress *progress);
// Installs a WMP matching both players' lexica, transferring it only on
// success. Installation enables -wmp. Before switching lexicons without a disk
// table, issue "set -wmp false -lex NAME", then build/install the new matching
// table. The disabled option releases the installed table.
bool cmd_api_install_wmp(Magpie *mp, WMP *wmp);

#endif
