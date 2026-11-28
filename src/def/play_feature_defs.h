#ifndef PLAY_FEATURE_DEFS_H
#define PLAY_FEATURE_DEFS_H

// The dimensions of the play embedding that Locally Adaptive Monte Carlo
// (lamc.h) searches. Adding a feature adds one value here, before
// NUMBER_OF_PLAY_FEATURES; the switches in play_feature_registry.c have no
// default, so the build names every one that still needs a case. Removing a
// feature deletes its value, and the build names every case left to delete.
typedef enum {
  PLAY_FEATURE_SCORE,
  PLAY_FEATURE_LEAVE_VALUE,
  PLAY_FEATURE_EQUITY,
  PLAY_FEATURE_TILES_PLAYED,
  PLAY_FEATURE_MOVE_TYPE,
  PLAY_FEATURE_BAG_AFTER,
  PLAY_FEATURE_LEAVE_SHAPE,
  PLAY_FEATURE_LEAVE_DUPLICATES,
  PLAY_FEATURE_LOCATION,
  PLAY_FEATURE_DIRECTION,
  PLAY_FEATURE_PREMIUM_ACCESS,
  PLAY_FEATURE_PERIMETER,
  NUMBER_OF_PLAY_FEATURES,
} play_feature_t;

// A continuous feature's columns are standardized across a position's plays
// and followed by the search's gradient steps. A categorical feature's
// columns are one-hot, left unscaled, and never stepped along.
typedef enum {
  PLAY_FEATURE_KIND_CONTINUOUS,
  PLAY_FEATURE_KIND_CATEGORICAL,
} play_feature_kind_t;

enum {
  // The most columns any one feature adds.
  PLAY_FEATURE_MAX_COLUMNS = 4,
  // The most columns an embedding can have: every feature at its widest.
  PLAY_EMBEDDING_MAX_COLUMNS =
      NUMBER_OF_PLAY_FEATURES * PLAY_FEATURE_MAX_COLUMNS,
};

// The features LAMC uses unless -lamcfeatures names others.
#define PLAY_FEATURE_DEFAULT_SET                                               \
  "score,leave,tiles,type,bagafter,leaveshape,location,direction,premium,"     \
  "perimeter"

#endif
