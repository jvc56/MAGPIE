#ifndef MOVE_GEN_PI_H
#define MOVE_GEN_PI_H

#include "../../src/ent/anchor.h"
#include "../../src/ent/kwg_dead_ends.h"

AnchorList *gen_get_anchor_list(int thread_index);
KWGDeadEnds *gen_get_kwgde(int thread_index);

#endif