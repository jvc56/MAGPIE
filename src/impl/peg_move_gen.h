#ifndef PEG_MOVE_GEN_H
#define PEG_MOVE_GEN_H

#include "../ent/move.h"
#include "move_gen.h"

// Generates every play into args->move_list. A full list drops its lowest
// equity plays, the pass among them, which a search that tries them all needs,
// so it grows until it holds them.
static inline void peg_generate_all_moves(const MoveGenArgs *args) {
  generate_moves(args);
  while (move_list_get_count(args->move_list) >=
         move_list_get_capacity(args->move_list)) {
    move_list_resize(args->move_list,
                     2 * move_list_get_capacity(args->move_list));
    generate_moves(args);
  }
}

#endif
