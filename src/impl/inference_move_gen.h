#ifndef INFERENCE_MOVE_GEN_H
#define INFERENCE_MOVE_GEN_H

#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/rack.h"
#include "../ent/rack_hash_table.h"

typedef struct InferenceMoveGenArgs {
  Game *game;
  Rack *bag_as_rack;
  // Tiles revealed by opponent's previous move (must be subset of any valid rack)
  const Rack *target_played_tiles;
  int move_list_capacity;
  Equity eq_margin_movegen;
  RackHashTable *rack_hash_table;
  const KWG *override_kwg;
  // Index of the target player (whose rack we're inferring)
  int target_index;
} InferenceMoveGenArgs;

// Main entry point: generates moves for all possible racks derivable from
// the bag_as_rack and populates the rack_hash_table with move lists keyed
// by the rack's leave (complement of played/exchanged tiles).
void generate_rack_moves_from_bag(const InferenceMoveGenArgs *args);

#endif
