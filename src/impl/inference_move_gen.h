#ifndef INFERENCE_MOVE_GEN_H
#define INFERENCE_MOVE_GEN_H

#include "../ent/anchor.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
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
} InferenceMoveGenArgs;

// Main entry point: generates moves for all possible racks derivable from
// the bag_as_rack and populates the rack_hash_table with move lists keyed
// by the rack's leave (complement of played/exchanged tiles).
void generate_rack_moves_from_bag(const InferenceMoveGenArgs *args);

// Record a scoring play to the rack hash table.
// Given the current rack and a scoring move, computes the leave,
// looks up the leave value, and adds the move with equity = score + leave_value.
//
// The anchor provides the starting square (row, col) and direction for the play.
// Its highest_possible_equity and highest_possible_score fields should be set
// to EQUITY_MAX_VALUE since we're not using them for pruning in this context.
//
// The ld and klv parameters are needed for leave computation and lookup.
void record_scoring_play_to_rack_hash_table(RackHashTable *rht,
                                            const LetterDistribution *ld,
                                            const KLV *klv,
                                            const Rack *current_rack,
                                            const Anchor *anchor,
                                            const Move *move);

#endif
