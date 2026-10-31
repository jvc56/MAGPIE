#ifndef KWG_WOADWAGE_H
#define KWG_WOADWAGE_H

#include "../def/letter_distribution_defs.h"
#include "kwg.h"
#include <stdbool.h>
#include <stdint.h>

// Woadwage's lexicon: the words of a KWG's lexicon, and every word made of
// two of them run together. Letters are machine letters without blank flags.

// Whether letters[0..length) is a woadwage word.
bool kwg_accepts_woadwage(const KWG *kwg, const MachineLetter *letters,
                          int length);

// The letters that, on an empty square between the letters before it and the
// letters after it in a line, make a woadwage word with them. Either side may
// be empty.
uint64_t kwg_compute_woadwage_cross_set(const KWG *kwg,
                                        const MachineLetter *before,
                                        int before_length,
                                        const MachineLetter *after,
                                        int after_length);

#endif
