#ifndef SIM_DEFS_H
#define SIM_DEFS_H

enum {
  MAX_PLIES = 25,
};

// Dual-lexicon modes for simulation and endgame (e.g., TWL player vs CSW)
// - IGNORANT: Player assumes opponent uses the same lexicon as them.
//             Opponent moves generated using the player's lexicon.
// - INFORMED: Player knows opponent may have different words available.
//             Opponent moves generated using opponent's actual lexicon.
typedef enum {
  DUAL_LEXICON_MODE_IGNORANT,
  DUAL_LEXICON_MODE_INFORMED,
} dual_lexicon_mode_t;

#endif