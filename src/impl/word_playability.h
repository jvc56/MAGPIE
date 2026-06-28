#ifndef WORD_PLAYABILITY_H
#define WORD_PLAYABILITY_H

#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

// Word-playability metrics, collected over an autoplay run (one analysis per
// position the player on turn faces). For every word in the full lexicon it
// accumulates three metrics, all derived from the position's full,
// equity-sorted move list:
//
//   count   - uniform play count: +1 for every word (main word AND every cross
//             /hook word) the best move forms, summed over all positions.
//   penalty - "equity loss if the player knew every word except this one": for
//             each word W the best move forms, the equity gap to the best move
//             that forms no W. Summed in Equity millipoints.
//   bonus   - "equity gained from knowing a small word set plus this word":
//             only when a small-set KWG is supplied. Baseline is the best move
//             whose words are all in the small set; for each move that becomes
//             playable by adding exactly one word W (all its other words are in
//             the small set), W accrues the gap over the baseline, but only on
//             positions where adding W beats the baseline (i.e. a small-set
//             word is not already best). Summed in Equity millipoints.
//
// Equity is summed as int64 millipoints (no floating point); the Equity type is
// already integer millipoints.

typedef enum {
  WORD_PLAYABILITY_SORT_COUNT,
  WORD_PLAYABILITY_SORT_PENALTY,
  WORD_PLAYABILITY_SORT_BONUS,
} word_playability_sort_t;

// Shared, read-only context: the enumerated full lexicon, the optional small
// word set, and output options. One instance is shared across worker threads.
typedef struct WordPlayabilityContext WordPlayabilityContext;

// Enumerates every word of full_kwg into a sorted table. small_kwg may be NULL
// (then the bonus metric is omitted). Takes ownership of out_path (frees it on
// destroy). ld and small_kwg are borrowed (not freed).
WordPlayabilityContext *
word_playability_context_create(const KWG *full_kwg, const KWG *small_kwg,
                                const LetterDistribution *ld, char *out_path,
                                word_playability_sort_t sort);
void word_playability_context_destroy(WordPlayabilityContext *ctx);
bool word_playability_context_has_bonus(const WordPlayabilityContext *ctx);

// Enables the position cache dump: each worker writes its analyzed positions to
// <dump_prefix>.<n> (binary) for the offline greedy. Run with the small set as
// the seed so the dumped baseline is the seed baseline.
void word_playability_context_set_dump_prefix(WordPlayabilityContext *ctx,
                                              const char *dump_prefix);

// Writes the full lexicon, one word per line in id order, so the offline greedy
// maps dump word ids back to words.
void word_playability_write_word_list(const WordPlayabilityContext *ctx,
                                      const char *path,
                                      ErrorStack *error_stack);
uint32_t word_playability_context_num_words(const WordPlayabilityContext *ctx);

// Full-lexicon index of `word` (length `len`); UINT32_MAX if absent. Blank
// flags on the letters are ignored.
uint32_t word_playability_context_word_index(const WordPlayabilityContext *ctx,
                                             const MachineLetter *word,
                                             int len);

// Per-thread accumulator (count/penalty/bonus arrays + reusable move list and
// scratch). Bound to a context for the lexicon size.
typedef struct WordPlayabilityCounts WordPlayabilityCounts;

WordPlayabilityCounts *
word_playability_counts_create(const WordPlayabilityContext *ctx);
void word_playability_counts_destroy(WordPlayabilityCounts *counts);

// Per-word accumulated values (for inspection/testing). Penalty and bonus are
// in Equity millipoints; bonus is 0 when no small set was configured.
uint64_t word_playability_counts_get_count(const WordPlayabilityCounts *counts,
                                           uint32_t word_index);
int64_t word_playability_counts_get_penalty(const WordPlayabilityCounts *counts,
                                            uint32_t word_index);
int64_t word_playability_counts_get_bonus(const WordPlayabilityCounts *counts,
                                          uint32_t word_index);

// Generates the full equity-sorted move list for the player on turn in `game`
// and folds its three metrics into `counts`. No-op if the position yields no
// moves. Does not mutate the game.
void word_playability_counts_add_position(WordPlayabilityCounts *counts,
                                          const WordPlayabilityContext *ctx,
                                          const Game *game);

// Adds every per-word total of src into dst (the reduce step).
void word_playability_counts_merge(WordPlayabilityCounts *dst,
                                   const WordPlayabilityCounts *src);

// Positions analyzed and how many had a true bingo (best move places all
// RACK_SIZE tiles) as the best play.
uint64_t
word_playability_counts_get_positions(const WordPlayabilityCounts *counts);
uint64_t
word_playability_counts_get_bingos(const WordPlayabilityCounts *counts);

// Writes the sorted CSV (sort key descending, ties alphabetical) to the
// context's output path. Columns: word,count,penalty[,bonus].
void word_playability_write_csv(const WordPlayabilityContext *ctx,
                                const WordPlayabilityCounts *totals,
                                ErrorStack *error_stack);

#endif
