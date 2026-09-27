#ifndef PAT_LEXICON_H
#define PAT_LEXICON_H

#include "../def/letter_distribution_defs.h"
#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "pat.h"

// The lexicon tables a PATWeights evaluates with: per-letter hook
// flexibility and the floater through tables, built from the lexicon by
// pat_prepare_hook_flex.

// Builds the per-letter flexibility table (the number of two-letter words
// containing each letter) from the lexicon. Used to approximate the hook
// and extension flexibility of squares whose real cross and extension sets
// do not exist yet because they are created by the move being evaluated.
// Must be called before the weights are used for evaluation; kwg may be
// NULL, which zeroes the table.
void pat_prepare_hook_flex(PATWeights *pat, const KWG *kwg,
                           const LetterDistribution *ld);
// Fills PATWeights.hook_leave_value: each tile's value as a one-tile leave
// under klv (NULL zeroes it), what an opponent gives up by spending it on a
// hook. Must be called with the evaluating player's leaves before
// hook_value weights are used.
void pat_prepare_hook_leaves(PATWeights *pat, const KLV *klv,
                             const LetterDistribution *ld);
// Returns the flexibility table entry for an (unblanked) machine letter.
int pat_get_hook_flex(const PATWeights *pat, MachineLetter ml);
// The end-specific through tables: word_end 0 when ml is the word's first
// letter, 1 when its last (see pat_walk_words).
int pat_get_through_score_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span);
int pat_get_through_count_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span);
// Entries of the unsigned floater through-table; see
// PATWeights.through_count.
int pat_get_through_count(const PATWeights *pat, MachineLetter ml, int span);
// The run-keyed through tables (see PATWeights.run_through): the
// log-scaled count and mean rest-of-word score of words of the given
// length whose first (word_end 0) or last (word_end 1) key_len letters,
// in word order, are key. Zero when the tables were never prepared.
int pat_get_run_through_count(const PATWeights *pat, int word_end,
                              const MachineLetter *key, int key_len,
                              int word_length);
int pat_get_run_through_score(const PATWeights *pat, int word_end,
                              const MachineLetter *key, int key_len,
                              int word_length);

#endif
