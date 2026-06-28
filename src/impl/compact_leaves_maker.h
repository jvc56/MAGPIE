#ifndef COMPACT_LEAVES_MAKER_H
#define COMPACT_LEAVES_MAKER_H

#include "../ent/compact_leaves.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fits a CompactLeaves model approximating klv's leave values, minimizing
// frequency-weighted squared error under target_bytes. weights is a per-leave
// frequency array indexed by the KLV word index (NULL => uniform). radix_code
// selects the fixed-point quantum (default COMPACT_LEAVES_RADIX_EIGHTH).
//
// v1 model: intercept + per-tile + duplicate penalty + 5x5 vowel/consonant
// table + greedily selected synergies (scored by weighted-error-reduction per
// byte), jointly refit. The size budget is spent on synergies. When bit_packed
// is true the body is sub-byte bit-packed (FLAG_BITPACKED), so the selection
// uses the packed cost and fits more synergies into target_bytes.
//
// target_bytes has an effective floor: the base block (header + per-tile + dup
// + V/C coefficients, no synergies) is always emitted, so a target below that
// floor (~111 B bit-packed / ~180 B byte for English) yields a base-only file
// AT the floor size, not smaller. Above the floor the file is always
// <= target_bytes.
CompactLeaves *
compact_leaves_create_from_klv(const KLV *klv, const LetterDistribution *ld,
                               const uint64_t *weights, size_t target_bytes,
                               uint8_t radix_code, bool bit_packed);

// Reads a leave-frequency CSV ("leave_str,count" per line, blank tile = '?', as
// written by `autoplay topkleaves`) into a per-leave weight array indexed by
// KLV word index (caller frees). Lines that don't parse or name an unknown
// leave are skipped. On open failure, pushes an error and returns NULL.
uint64_t *compact_leaves_read_weights_csv(const KLV *klv,
                                          const LetterDistribution *ld,
                                          const char *csv_path,
                                          ErrorStack *error_stack);

// Overwrites every leave value in klv with the compact model's prediction, so
// the result can be written out as a .klv2 (lossy expansion) for evaluation.
void compact_leaves_overwrite_klv(const CompactLeaves *cl, KLV *klv,
                                  const LetterDistribution *ld);

// Frequency-weighted and unweighted RMS (in points) of the QUANTIZED model vs
// the KLV's values, over all leaves. weights NULL => unweighted only.
void compact_leaves_rms(const CompactLeaves *cl, const KLV *klv,
                        const LetterDistribution *ld, const uint64_t *weights,
                        double *weighted_rms_out, double *unweighted_rms_out);

#endif
