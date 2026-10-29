#ifndef PEG_NET_DATA_TEST_H
#define PEG_NET_DATA_TEST_H

// pegnet: modes that make training data: positions, labels and features.

#include "../src/util/string_util.h"

// pegnet:peg1data
// Fresh 1PEG roots from static self-play with their static top-64
// candidates, labels and input rows. Fields: count, offset, seed, prefix.
// Writes PREFIX.positions.csv, .labels.csv, .scenarios.csv and
// .inputs.f32.
void peg1_labels_mode(const StringSplitter *fields);

// pegnet:peg1roots
// Fresh 1PEG roots from static self-play (the peg1data source procedure),
// with public candidate scalars and per-world reply features but no labels.
void peg1_roots_mode(const StringSplitter *fields);

// pegnet:peg1feat2
// Stored-shard extraction with the v2 definitions (reply2.f32, v1 layout).
void peg1_features_v2_mode(const StringSplitter *fields);

// pegnet:peg1pos
// Fresh 1PEG root positions only (the peg1data source procedure, no labels or
// features). Writes PREFIX.positions.csv in the peg1data format.
void peg1_positions_mode(const StringSplitter *fields);

// pegnet:peg1deep
// Deeper per-world labels for chosen candidates (teacher-quality check).
// Choices CSV rows: root,candidate (static rank), grouped by root in
// positions order. Every public hypothetical last tile is labelled with an
// endgame search of the requested plies (greedy leaf horizon, as depth2).
void peg1_deep_labels_mode(const StringSplitter *fields);

// pegnet:peg2pos
// peg_positions_mode with two tiles in the bag.
void peg2_positions_mode(const StringSplitter *fields);

// pegnet:pegnpos
// PEG_POSITIONS_BAG tiles in the bag (1..RACK_SIZE); peg1pos, peg2pos and
// peg3pos are the fixed sizes.
void peg_positions_mode(const StringSplitter *fields);

// pegnet:peg2data
// 2PEG labels. For each board, each of the static top-K placements is played
// in every hidden world: which two of the nine unseen tiles are in the bag
// and, for a one-tile play, which of them the mover draws (weighted by
// multiplicity). A play of two or more tiles empties the bag. After a one-tile
// play the opponent faces a 1PEG and the network picks its move from the
// opponent's information; any further one-tile-bag turn is picked the same
// way. The bag-empty endgame is then searched to a fixed depth. Fields:
// positions prefix, output prefix, top K, endgame plies, first model, second
// model|-, shortlist, w_spread, scale, check boards (network information-rule
// check on the first N boards).
void peg2_labels_mode(const StringSplitter *fields);

// pegnet:peg2feat
// Fields: positions prefix, output prefix, top K, first model, second
// model|-, shortlist, w_spread, scale, net features (0|1), check boards.
// Writes OUT.cands.csv (one row per candidate), OUT.public.f32 (the 68 public
// scalars, static equity and spread after the move per candidate) and
// OUT.worlds.f32 (PEG2_FEATURE_WIDTH floats per candidate world, in
// cands.csv order; the columns are listed where peg2_features_mode opens the
// file).
void peg2_features_mode(const StringSplitter *fields);

// pegnet:peg3pos
// peg_positions_mode with three tiles in the bag.
void peg3_positions_mode(const StringSplitter *fields);

#endif
