#ifndef SIM_DEFS_H
#define SIM_DEFS_H

enum {
  MAX_PLIES = 25,
};

// Dual-lexicon modes for play (e.g., TWL player vs CSW opponent)
// These modes affect how opponent moves are generated during simulation/endgame:
// - IGNORANT: Opponent moves generated using the simulating player's lexicon.
//             The player assumes opponent uses the same lexicon as them.
// - INFORMED: Opponent moves generated using opponent's actual lexicon.
//             The player knows opponent may have different/more words available.
typedef enum {
  DUAL_LEXICON_MODE_IGNORANT,
  DUAL_LEXICON_MODE_INFORMED,
} dual_lexicon_mode_t;

#endif