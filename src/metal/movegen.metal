// WMPG (WMP-on-GPU) movegen kernels.
//
// One thread per rack. Each thread enumerates subracks of the player rack
// summing to the slot's playable-tile count (= word_length - fixed_count),
// hashes each subrack into the per-length WMP buckets, walks the bucket for
// matching bitrack entries, decodes inlined / non-inlined anagrams, and for
// each anagram validates cross-checks + fixed-letter constraints. Blank
// racks (1 or 2 blanks) trigger a multi-stage lookup chain through the
// blank-1 and blank-2 sub-tables, with the blank tile placed at the
// max-equity X-position per anagram.
//
// BitRack encoding: 32 letters × 4 bits, packed into 16 bytes (= 4 uchar4).
//
// Buffers per dispatch (one slot's length L):
//   L_bucket_starts: word-table bucket-start array (num_buckets+1 entries)
//   L_entries:       word-table entries (num_entries × 32 bytes)
//   L_uninlined_letters: anagram letters for entries that overflow inline
//   racks: B × 16 bytes (one BitRack per rack)
//   constants: num_buckets, word_length, target_used_size, cross_sets,
//              fixed_letters, fixed_bitrack, plus scoring (letter_scores,
//              position_multipliers, base_score, bingo_to_add) and leave
//              tables for equity/top-1
//   blank-1 / blank-2 sub-tables (for blank-rack support):
//     b1_bucket_starts, b1_entries, b1_num_buckets
//     b2_bucket_starts, b2_entries, b2_num_buckets

#include <metal_stdlib>
using namespace metal;

// Murmur3-style mix to 64 bits, mirrors bit_rack_mix_to_64 in bit_rack.h.
// Must use the same BIT_RACK_HASH_ROTATION_SHIFT (17) and mix constants as
// the host so bucket indices match WMP exactly.
inline ulong bitrack_mix_to_64(ulong low, ulong high) {
  low ^= high;
  low ^= (high << 17) | (high >> (64 - 17));
  low ^= low >> 33;
  low *= 0xff51afd7ed558ccdULL;
  low ^= low >> 33;
  low *= 0xc4ceb9fe1a85ec53ULL;
  low ^= low >> 33;
  return low;
}

kernel void count_wmpg_kernel(
    device const uint *L_bucket_starts [[buffer(0)]],   // num_buckets+1 entries
    device const uchar *L_entries [[buffer(1)]],         // num_entries × 32
    device const uchar *L_uninlined_letters [[buffer(2)]], // optional
    device const uchar *racks_bytes [[buffer(3)]],       // B × 16
    constant uint &num_buckets [[buffer(4)]],
    constant uint &word_length [[buffer(5)]],
    constant uint &target_used_size [[buffer(6)]],       // word_length - fixed_count
    constant ulong *cross_sets [[buffer(7)]],            // word_length entries
    constant uchar *fixed_letters [[buffer(8)]],          // word_length bytes
    constant ulong &fixed_bitrack_low [[buffer(9)]],
    constant ulong &fixed_bitrack_high [[buffer(10)]],
    device atomic_uint *out_counts [[buffer(11)]],
    uint gid [[thread_position_in_grid]]) {
  // gid = rack_idx
  // Decode rack into per-letter counts.
  uchar letter_counts[32];
  device const uchar *my_rack = racks_bytes + gid * 16;
  for (int i = 0; i < 16; i++) {
    const uchar b = my_rack[i];
    letter_counts[2 * i + 0] = b & 0x0Fu;
    letter_counts[2 * i + 1] = (b >> 4) & 0x0Fu;
  }
  // Find distinct letters (count > 0) and their max counts.
  uchar distinct_ml[16];
  uchar distinct_max[16];
  int distinct_count = 0;
  for (int ml = 0; ml < 32; ml++) {
    if (letter_counts[ml] > 0) {
      if (distinct_count < 16) {
        distinct_ml[distinct_count] = (uchar)ml;
        distinct_max[distinct_count] = letter_counts[ml];
        distinct_count++;
      }
    }
  }
  if (target_used_size == 0u && fixed_bitrack_low == 0 &&
      fixed_bitrack_high == 0) {
    return; // empty target — nothing to look up
  }
  // Enumerate used[] arrays summing to target_used_size, used[k] ≤
  // distinct_max[k]. Standard "next multiset" iteration with stack.
  uchar used[16];
  for (int k = 0; k < 16; k++) {
    used[k] = 0;
  }
  uint count = 0;
  while (true) {
    // Compute current sum.
    int sum = 0;
    for (int k = 0; k < distinct_count; k++) {
      sum += used[k];
    }
    if ((uint)sum == target_used_size) {
      // Build target_bitrack = used (mapped via distinct_ml) + fixed.
      ulong tg_lo = fixed_bitrack_low;
      ulong tg_hi = fixed_bitrack_high;
      for (int k = 0; k < distinct_count; k++) {
        const int ml = distinct_ml[k];
        const int shift = ml * 4;
        const ulong v = (ulong)used[k];
        if (shift < 64) {
          tg_lo += v << shift;
        } else {
          tg_hi += v << (shift - 64);
        }
      }
      // Hash + bucket walk.
      const ulong h = bitrack_mix_to_64(tg_lo, tg_hi);
      const uint bucket_idx = (uint)h & (num_buckets - 1);
      const uint start = L_bucket_starts[bucket_idx];
      const uint end = L_bucket_starts[bucket_idx + 1];
      for (uint i = start; i < end; i++) {
        device const uchar *e = L_entries + i * 32u;
        ulong e_lo;
        ulong e_hi;
        e_lo = (ulong)e[16] | ((ulong)e[17] << 8) | ((ulong)e[18] << 16) |
               ((ulong)e[19] << 24) | ((ulong)e[20] << 32) |
               ((ulong)e[21] << 40) | ((ulong)e[22] << 48) |
               ((ulong)e[23] << 56);
        e_hi = (ulong)e[24] | ((ulong)e[25] << 8) | ((ulong)e[26] << 16) |
               ((ulong)e[27] << 24) | ((ulong)e[28] << 32) |
               ((ulong)e[29] << 40) | ((ulong)e[30] << 48) |
               ((ulong)e[31] << 56);
        if (e_lo != tg_lo || e_hi != tg_hi) {
          continue;
        }
        // Match. Decode entry (inlined or noninlined).
        const uchar b0 = e[0];
        if (b0 != 0) {
          // Inlined: up to 16/word_length anagrams; first byte of each L-byte
          // chunk is non-zero unless that slot is unused.
          const uint max_inline = 16u / word_length;
          for (uint w = 0; w < max_inline; w++) {
            const uint base = w * word_length;
            if (e[base] == 0) {
              break;
            }
            // Verify cross-checks + playthrough letters.
            bool ok = true;
            for (uint j = 0; j < word_length && ok; j++) {
              const uchar wl = e[base + j];
              const uchar fl = fixed_letters[j];
              if (fl != 0) {
                if (wl != fl) {
                  ok = false;
                }
              } else {
                const ulong bit = ((ulong)1) << wl;
                if ((cross_sets[j] & bit) == 0) {
                  ok = false;
                }
              }
            }
            if (ok) {
              count++;
            }
          }
        } else {
          // Non-inlined: word_start at offset 8, num_words at offset 12.
          const uint word_start = (uint)e[8] | ((uint)e[9] << 8) |
                                  ((uint)e[10] << 16) | ((uint)e[11] << 24);
          const uint num_words = (uint)e[12] | ((uint)e[13] << 8) |
                                 ((uint)e[14] << 16) | ((uint)e[15] << 24);
          for (uint w = 0; w < num_words; w++) {
            device const uchar *wl_base =
                L_uninlined_letters + word_start + w * word_length;
            bool ok = true;
            for (uint j = 0; j < word_length && ok; j++) {
              const uchar wl = wl_base[j];
              const uchar fl = fixed_letters[j];
              if (fl != 0) {
                if (wl != fl) {
                  ok = false;
                }
              } else {
                const ulong bit = ((ulong)1) << wl;
                if ((cross_sets[j] & bit) == 0) {
                  ok = false;
                }
              }
            }
            if (ok) {
              count++;
            }
          }
        }
        break; // Found the matching bucket entry; no more to scan.
      }
    }
    // Increment used as multi-base counter.
    int k = 0;
    used[0]++;
    while (k < distinct_count && used[k] > distinct_max[k]) {
      used[k] = 0;
      k++;
      if (k < distinct_count) {
        used[k]++;
      }
    }
    if (k >= distinct_count) {
      break;
    }
  }
  if (count > 0) {
    atomic_fetch_add_explicit(&out_counts[gid], count, memory_order_relaxed);
  }
}

// WMPG-based equity kernel. Same matching as count_wmpg_kernel + computes
// per-match equity (score + leave_value) and atomic_adds (count, equity_sum)
// per rack. Parallel to equity_with_playthrough_kernel but using WMPG hash
// lookup instead of brute-force scan.
//
// Output layout: 2*B uint32s — out[2i]=count, out[2i+1]=equity_sum (raw
// millipoints, signed but stored as uint32 via atomic_fetch_add — works
// because addition wraps consistently in two's complement).
kernel void equity_wmpg_kernel(
    device const uint *L_bucket_starts [[buffer(0)]],
    device const uchar *L_entries [[buffer(1)]],
    device const uchar *L_uninlined_letters [[buffer(2)]],
    device const uchar *racks_bytes [[buffer(3)]],
    constant uint &num_buckets [[buffer(4)]],
    constant uint &word_length [[buffer(5)]],
    constant uint &target_used_size [[buffer(6)]],
    constant ulong *cross_sets [[buffer(7)]],
    constant uchar *fixed_letters [[buffer(8)]],
    constant ulong &fixed_bitrack_low [[buffer(9)]],
    constant ulong &fixed_bitrack_high [[buffer(10)]],
    constant int *letter_scores [[buffer(11)]],
    constant int *position_multipliers [[buffer(12)]],
    constant int &base_score [[buffer(13)]],
    constant int &bingo_to_add [[buffer(14)]],
    constant uchar4 *leave_used [[buffer(15)]],
    constant int *leave_values [[buffer(16)]],
    constant uint &n_leaves [[buffer(17)]],
    device atomic_uint *output [[buffer(18)]],
    uint gid [[thread_position_in_grid]]) {
  uchar letter_counts[32];
  device const uchar *my_rack = racks_bytes + gid * 16;
  for (int i = 0; i < 16; i++) {
    const uchar b = my_rack[i];
    letter_counts[2 * i + 0] = b & 0x0Fu;
    letter_counts[2 * i + 1] = (b >> 4) & 0x0Fu;
  }
  uchar distinct_ml[16];
  uchar distinct_max[16];
  int distinct_count = 0;
  for (int ml = 0; ml < 32; ml++) {
    if (letter_counts[ml] > 0) {
      if (distinct_count < 16) {
        distinct_ml[distinct_count] = (uchar)ml;
        distinct_max[distinct_count] = letter_counts[ml];
        distinct_count++;
      }
    }
  }
  uchar used[16];
  for (int k = 0; k < 16; k++) {
    used[k] = 0;
  }
  uint local_count = 0;
  int local_equity_sum = 0;
  while (true) {
    int sum = 0;
    for (int k = 0; k < distinct_count; k++) {
      sum += used[k];
    }
    if ((uint)sum == target_used_size) {
      ulong tg_lo = fixed_bitrack_low;
      ulong tg_hi = fixed_bitrack_high;
      // Build per-letter "used" counts buffer for leave-table lookup.
      uchar used_letters[32] = {0};
      for (int k = 0; k < distinct_count; k++) {
        const int ml = distinct_ml[k];
        used_letters[ml] = used[k];
        const int shift = ml * 4;
        const ulong v = (ulong)used[k];
        if (shift < 64) {
          tg_lo += v << shift;
        } else {
          tg_hi += v << (shift - 64);
        }
      }
      // Leave bitrack (rack - used). Encode as two ulongs.
      ulong leave_lo = 0;
      ulong leave_hi = 0;
      for (int ml = 0; ml < 32; ml++) {
        const int rem = letter_counts[ml] - used_letters[ml];
        if (rem > 0) {
          const int shift = ml * 4;
          if (shift < 64) {
            leave_lo += ((ulong)rem) << shift;
          } else {
            leave_hi += ((ulong)rem) << (shift - 64);
          }
        }
      }
      // The kernel's leave-table is packed as uchar4×4 chunks (4 chunks per
      // entry); we look up by USED bitrack (= word - fixed) since the table
      // is keyed that way per the equity kernel's convention. Actually
      // equity_with_playthrough_kernel keys leave table by USED, not LEAVE.
      // Match that: scan for entry with used_a..used_d == ours.
      uchar4 used_chunks[4];
      // Pack used_letters[0..15] into uchar4×4 (low nibble = ml*2, high =
      // ml*2+1).
      for (int i = 0; i < 4; i++) {
        uchar b0 = (used_letters[i * 8 + 0] | (used_letters[i * 8 + 1] << 4));
        uchar b1 = (used_letters[i * 8 + 2] | (used_letters[i * 8 + 3] << 4));
        uchar b2 = (used_letters[i * 8 + 4] | (used_letters[i * 8 + 5] << 4));
        uchar b3 = (used_letters[i * 8 + 6] | (used_letters[i * 8 + 7] << 4));
        used_chunks[i] = uchar4(b0, b1, b2, b3);
      }
      int leave_value = 0;
      for (uint k = 0; k < n_leaves; k++) {
        const uchar4 ka = leave_used[k * 4 + 0];
        const uchar4 kb = leave_used[k * 4 + 1];
        const uchar4 kc = leave_used[k * 4 + 2];
        const uchar4 kd = leave_used[k * 4 + 3];
        if (all(used_chunks[0] == ka) && all(used_chunks[1] == kb) &&
            all(used_chunks[2] == kc) && all(used_chunks[3] == kd)) {
          leave_value = leave_values[k];
          break;
        }
      }
      // Hash + bucket walk.
      const ulong h = bitrack_mix_to_64(tg_lo, tg_hi);
      const uint bucket_idx = (uint)h & (num_buckets - 1);
      const uint start = L_bucket_starts[bucket_idx];
      const uint end = L_bucket_starts[bucket_idx + 1];
      for (uint i = start; i < end; i++) {
        device const uchar *e = L_entries + i * 32u;
        ulong e_lo;
        ulong e_hi;
        e_lo = (ulong)e[16] | ((ulong)e[17] << 8) | ((ulong)e[18] << 16) |
               ((ulong)e[19] << 24) | ((ulong)e[20] << 32) |
               ((ulong)e[21] << 40) | ((ulong)e[22] << 48) |
               ((ulong)e[23] << 56);
        e_hi = (ulong)e[24] | ((ulong)e[25] << 8) | ((ulong)e[26] << 16) |
               ((ulong)e[27] << 24) | ((ulong)e[28] << 32) |
               ((ulong)e[29] << 40) | ((ulong)e[30] << 48) |
               ((ulong)e[31] << 56);
        if (e_lo != tg_lo || e_hi != tg_hi) {
          continue;
        }
        const uchar b0 = e[0];
        if (b0 != 0) {
          const uint max_inline = 16u / word_length;
          for (uint w = 0; w < max_inline; w++) {
            const uint base = w * word_length;
            if (e[base] == 0) {
              break;
            }
            int score = base_score + bingo_to_add;
            bool ok = true;
            for (uint j = 0; j < word_length && ok; j++) {
              const uchar wl = e[base + j];
              const uchar fl = fixed_letters[j];
              if (fl != 0) {
                if (wl != fl) {
                  ok = false;
                  break;
                }
              } else {
                const ulong bit = ((ulong)1) << wl;
                if ((cross_sets[j] & bit) == 0) {
                  ok = false;
                  break;
                }
              }
              score += letter_scores[wl] * position_multipliers[j];
            }
            if (ok) {
              local_count++;
              local_equity_sum += score + leave_value;
            }
          }
        } else {
          const uint word_start = (uint)e[8] | ((uint)e[9] << 8) |
                                  ((uint)e[10] << 16) | ((uint)e[11] << 24);
          const uint num_words = (uint)e[12] | ((uint)e[13] << 8) |
                                 ((uint)e[14] << 16) | ((uint)e[15] << 24);
          for (uint w = 0; w < num_words; w++) {
            device const uchar *wl_base =
                L_uninlined_letters + word_start + w * word_length;
            int score = base_score + bingo_to_add;
            bool ok = true;
            for (uint j = 0; j < word_length && ok; j++) {
              const uchar wl = wl_base[j];
              const uchar fl = fixed_letters[j];
              if (fl != 0) {
                if (wl != fl) {
                  ok = false;
                  break;
                }
              } else {
                const ulong bit = ((ulong)1) << wl;
                if ((cross_sets[j] & bit) == 0) {
                  ok = false;
                  break;
                }
              }
              score += letter_scores[wl] * position_multipliers[j];
            }
            if (ok) {
              local_count++;
              local_equity_sum += score + leave_value;
            }
          }
        }
        break;
      }
    }
    int k = 0;
    used[0]++;
    while (k < distinct_count && used[k] > distinct_max[k]) {
      used[k] = 0;
      k++;
      if (k < distinct_count) {
        used[k]++;
      }
    }
    if (k >= distinct_count) {
      break;
    }
  }
  if (local_count > 0) {
    atomic_fetch_add_explicit(&output[2 * gid + 0], local_count,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&output[2 * gid + 1], (uint)local_equity_sum,
                              memory_order_relaxed);
  }
}

// WMPG top-1 pass 1. Same matching/scoring/leave as equity_wmpg_kernel; instead
// of accumulating per-rack count and equity_sum, tracks the per-thread best
// equity_mp seen and does a single atomic_fetch_max into best_eq_mp[gid] at
// thread end. Pass-1 across multiple slot dispatches accumulates the global
// per-rack max equity.
//
// Blank handling: when a subrack contains 1 blank, the kernel does a blank-1
// table lookup; the entry's `blank_letters` bitvector tells which letters X
// the blank could substitute for. For each set bit X, a follow-up word-table
// lookup is done with the blank replaced by X. For each anagram with letter X
// at one or more positions, the blank is placed at the X-position with the
// SMALLEST `position_multipliers[p]` (yielding the maximum equity since the
// blank scores 0 at p, so we want to subtract the least letter_score×mult).
// 2-blank racks: blank-2 table → for each first-blank letter X1, recurse to
// blank-1 path with X1 substituted, then iterate second-blank substitutions.
kernel void top1_eqmp_wmpg_kernel(
    device const uint *L_bucket_starts [[buffer(0)]],
    device const uchar *L_entries [[buffer(1)]],
    device const uchar *L_uninlined_letters [[buffer(2)]],
    device const uchar *racks_bytes [[buffer(3)]],
    constant uint &num_buckets [[buffer(4)]],
    constant uint &word_length [[buffer(5)]],
    constant uint &target_used_size [[buffer(6)]],
    constant ulong *cross_sets [[buffer(7)]],
    constant uchar *fixed_letters [[buffer(8)]],
    constant ulong &fixed_bitrack_low [[buffer(9)]],
    constant ulong &fixed_bitrack_high [[buffer(10)]],
    constant int *letter_scores [[buffer(11)]],
    constant int *position_multipliers [[buffer(12)]],
    constant int &base_score [[buffer(13)]],
    constant int &bingo_to_add [[buffer(14)]],
    constant uchar4 *leave_used [[buffer(15)]],
    constant int *leave_values [[buffer(16)]],
    constant uint &n_leaves [[buffer(17)]],
    device atomic_int *best_eq_mp [[buffer(18)]],
    constant uint &leave_stride [[buffer(19)]],
    device const uint *b1_bucket_starts [[buffer(20)]],
    device const uchar *b1_entries [[buffer(21)]],
    constant uint &b1_num_buckets [[buffer(22)]],
    device const uint *b2_bucket_starts [[buffer(23)]],
    device const uchar *b2_entries [[buffer(24)]],
    constant uint &b2_num_buckets [[buffer(25)]],
    uint gid [[thread_position_in_grid]]) {
  const uint leave_base = gid * leave_stride;
  uchar letter_counts[32];
  device const uchar *my_rack = racks_bytes + gid * 16;
  for (int i = 0; i < 16; i++) {
    const uchar b = my_rack[i];
    letter_counts[2 * i + 0] = b & 0x0Fu;
    letter_counts[2 * i + 1] = (b >> 4) & 0x0Fu;
  }
  uchar distinct_ml[16];
  uchar distinct_max[16];
  int distinct_count = 0;
  int blank_idx = -1;
  for (int ml = 0; ml < 32; ml++) {
    if (letter_counts[ml] > 0) {
      if (distinct_count < 16) {
        if (ml == 0) {
          blank_idx = distinct_count;
        }
        distinct_ml[distinct_count] = (uchar)ml;
        distinct_max[distinct_count] = letter_counts[ml];
        distinct_count++;
      }
    }
  }
  uchar used[16];
  for (int k = 0; k < 16; k++) {
    used[k] = 0;
  }
  int local_best = INT_MIN;
  bool any_match = false;
  while (true) {
    int sum = 0;
    for (int k = 0; k < distinct_count; k++) {
      sum += used[k];
    }
    if ((uint)sum == target_used_size) {
      ulong tg_lo = fixed_bitrack_low;
      ulong tg_hi = fixed_bitrack_high;
      uchar used_letters[32] = {0};
      for (int k = 0; k < distinct_count; k++) {
        const int ml = distinct_ml[k];
        used_letters[ml] = used[k];
        const int shift = ml * 4;
        const ulong v = (ulong)used[k];
        if (shift < 64) {
          tg_lo += v << shift;
        } else {
          tg_hi += v << (shift - 64);
        }
      }
      uchar4 used_chunks[4];
      for (int i = 0; i < 4; i++) {
        uchar b0 = (used_letters[i * 8 + 0] | (used_letters[i * 8 + 1] << 4));
        uchar b1 = (used_letters[i * 8 + 2] | (used_letters[i * 8 + 3] << 4));
        uchar b2 = (used_letters[i * 8 + 4] | (used_letters[i * 8 + 5] << 4));
        uchar b3 = (used_letters[i * 8 + 6] | (used_letters[i * 8 + 7] << 4));
        used_chunks[i] = uchar4(b0, b1, b2, b3);
      }
      int leave_value = 0;
      for (uint k = 0; k < n_leaves; k++) {
        const uint ki = leave_base + k;
        const uchar4 ka = leave_used[ki * 4 + 0];
        const uchar4 kb = leave_used[ki * 4 + 1];
        const uchar4 kc = leave_used[ki * 4 + 2];
        const uchar4 kd = leave_used[ki * 4 + 3];
        if (all(used_chunks[0] == ka) && all(used_chunks[1] == kb) &&
            all(used_chunks[2] == kc) && all(used_chunks[3] == kd)) {
          leave_value = leave_values[ki];
          break;
        }
      }
      const int blank_in_subrack =
          (blank_idx >= 0) ? (int)used[blank_idx] : 0;
      // Number of blank substitutions to consider:
      //   0 blanks: directly look up tg in word table
      //   1 blank: blank-1(tg) → for each X in blank_letters, look up
      //            (tg with blank→X) in word table; place blank at
      //            X-position with smallest position_multipliers[p]
      //   2 blanks: blank-2(tg) → blank-1(tg with blank1→X1) → word(...→X2);
      //            place two blanks at the X1/X2 positions with smallest
      //            position_multipliers values
      //
      // Outer loop over blank substitutions sets (X1, X2) — for 0 blanks
      // we run the word-table walk once with X1 = X2 = 0xFF (sentinel
      // meaning "no blank"). For 1 blank, X1 iterates over set bits of
      // blank_letters, X2 = 0xFF. For 2 blanks, X1 iterates first-blank
      // letters, X2 iterates the resulting blank-1 entry's blank_letters.
      //
      // This is structured as a single outer loop emitting (filled_lo,
      // filled_hi, X1, X2) tuples; the inner block does word-table lookup
      // and per-anagram match+score+blank-discount calculation.
      uint b1_bv = 0;
      bool have_b1_bv = false;
      uint b2_bv = 0;
      if (blank_in_subrack == 1) {
        // Look up tg in blank-1 table; cache blank_letters bitvector.
        const ulong h_b1 = bitrack_mix_to_64(tg_lo, tg_hi);
        const uint bi = (uint)h_b1 & (b1_num_buckets - 1);
        const uint sb = b1_bucket_starts[bi];
        const uint eb = b1_bucket_starts[bi + 1];
        for (uint i = sb; i < eb; i++) {
          device const uchar *e = b1_entries + i * 32u;
          ulong elo = (ulong)e[16] | ((ulong)e[17] << 8) | ((ulong)e[18] << 16) |
                      ((ulong)e[19] << 24) | ((ulong)e[20] << 32) |
                      ((ulong)e[21] << 40) | ((ulong)e[22] << 48) |
                      ((ulong)e[23] << 56);
          ulong ehi = (ulong)e[24] | ((ulong)e[25] << 8) | ((ulong)e[26] << 16) |
                      ((ulong)e[27] << 24) | ((ulong)e[28] << 32) |
                      ((ulong)e[29] << 40) | ((ulong)e[30] << 48) |
                      ((ulong)e[31] << 56);
          if (elo == tg_lo && ehi == tg_hi) {
            b1_bv = (uint)e[8] | ((uint)e[9] << 8) | ((uint)e[10] << 16) |
                    ((uint)e[11] << 24);
            have_b1_bv = true;
            break;
          }
        }
      } else if (blank_in_subrack == 2) {
        // Look up tg in blank-2 table; cache first_blank_letters bitvector.
        const ulong h_b2 = bitrack_mix_to_64(tg_lo, tg_hi);
        const uint bi = (uint)h_b2 & (b2_num_buckets - 1);
        const uint sb = b2_bucket_starts[bi];
        const uint eb = b2_bucket_starts[bi + 1];
        for (uint i = sb; i < eb; i++) {
          device const uchar *e = b2_entries + i * 32u;
          ulong elo = (ulong)e[16] | ((ulong)e[17] << 8) | ((ulong)e[18] << 16) |
                      ((ulong)e[19] << 24) | ((ulong)e[20] << 32) |
                      ((ulong)e[21] << 40) | ((ulong)e[22] << 48) |
                      ((ulong)e[23] << 56);
          ulong ehi = (ulong)e[24] | ((ulong)e[25] << 8) | ((ulong)e[26] << 16) |
                      ((ulong)e[27] << 24) | ((ulong)e[28] << 32) |
                      ((ulong)e[29] << 40) | ((ulong)e[30] << 48) |
                      ((ulong)e[31] << 56);
          if (elo == tg_lo && ehi == tg_hi) {
            b2_bv = (uint)e[8] | ((uint)e[9] << 8) | ((uint)e[10] << 16) |
                    ((uint)e[11] << 24);
            break;
          }
        }
      }

      // Iterate the blank substitution choices (X1, X2). For 0 blanks,
      // X1=X2=255 (no-blank sentinel). For 1 blank, X1 iterates blank_letters,
      // X2=255. For 2 blanks, X1 iterates first_blank_letters; for each, do
      // a blank-1 lookup of (tg with blank→X1) to get the second blank's
      // letters bitvector, then iterate X2 over those.
      const uint x_max = 32u;
      uint x1_iter_start = 255u; // sentinel meaning "no x1 substitution"
      uint x1_iter_end = 256u;
      if (blank_in_subrack >= 1 && have_b1_bv == false &&
          blank_in_subrack == 1) {
        // No blank-1 entry found → no matches for 1-blank subrack
        x1_iter_end = x1_iter_start; // skip
      }
      if (blank_in_subrack == 1) {
        x1_iter_start = 1u;
        x1_iter_end = x_max;
      } else if (blank_in_subrack == 2) {
        x1_iter_start = 1u;
        x1_iter_end = x_max;
      }
      for (uint X1 = x1_iter_start;
           X1 < x1_iter_end || X1 == 255u; X1++) {
        if (X1 != 255u && blank_in_subrack == 1 &&
            !(b1_bv & (1u << X1))) {
          continue;
        }
        if (X1 != 255u && blank_in_subrack == 2 &&
            !(b2_bv & (1u << X1))) {
          continue;
        }
        // Build target with first blank substituted.
        ulong tf_lo = tg_lo;
        ulong tf_hi = tg_hi;
        if (blank_in_subrack >= 1 && X1 != 255u) {
          tf_lo -= 1ul; // remove 1 blank (blank is at bit 0 of low half)
          const int sh = (int)X1 * 4;
          if (sh < 64) tf_lo += (ulong)1 << sh;
          else tf_hi += (ulong)1 << (sh - 64);
        }

        // For 2-blank: look up (tf) in blank-1 table to get second-blank bv
        uint b1_inner_bv = 0;
        if (blank_in_subrack == 2) {
          const ulong h_b1 = bitrack_mix_to_64(tf_lo, tf_hi);
          const uint bi = (uint)h_b1 & (b1_num_buckets - 1);
          const uint sb = b1_bucket_starts[bi];
          const uint eb = b1_bucket_starts[bi + 1];
          for (uint i = sb; i < eb; i++) {
            device const uchar *e = b1_entries + i * 32u;
            ulong elo = (ulong)e[16] | ((ulong)e[17] << 8) |
                        ((ulong)e[18] << 16) | ((ulong)e[19] << 24) |
                        ((ulong)e[20] << 32) | ((ulong)e[21] << 40) |
                        ((ulong)e[22] << 48) | ((ulong)e[23] << 56);
            ulong ehi = (ulong)e[24] | ((ulong)e[25] << 8) |
                        ((ulong)e[26] << 16) | ((ulong)e[27] << 24) |
                        ((ulong)e[28] << 32) | ((ulong)e[29] << 40) |
                        ((ulong)e[30] << 48) | ((ulong)e[31] << 56);
            if (elo == tf_lo && ehi == tf_hi) {
              b1_inner_bv = (uint)e[8] | ((uint)e[9] << 8) |
                            ((uint)e[10] << 16) | ((uint)e[11] << 24);
              break;
            }
          }
        }

        uint x2_iter_start = 255u;
        uint x2_iter_end = 256u;
        if (blank_in_subrack == 2) {
          x2_iter_start = 1u;
          x2_iter_end = x_max;
        }
        for (uint X2 = x2_iter_start;
             X2 < x2_iter_end || X2 == 255u; X2++) {
          if (X2 != 255u && blank_in_subrack == 2 &&
              !(b1_inner_bv & (1u << X2))) {
            continue;
          }
          // Build final filled bitrack (with both blanks substituted if 2).
          ulong ff_lo = tf_lo;
          ulong ff_hi = tf_hi;
          if (blank_in_subrack == 2 && X2 != 255u) {
            ff_lo -= 1ul; // remove second blank
            const int sh = (int)X2 * 4;
            if (sh < 64) ff_lo += (ulong)1 << sh;
            else ff_hi += (ulong)1 << (sh - 64);
          }

          // Word-table lookup with the fully filled bitrack.
          const ulong h = bitrack_mix_to_64(ff_lo, ff_hi);
          const uint bucket_idx = (uint)h & (num_buckets - 1);
          const uint start = L_bucket_starts[bucket_idx];
          const uint end = L_bucket_starts[bucket_idx + 1];
          for (uint i = start; i < end; i++) {
            device const uchar *e = L_entries + i * 32u;
            ulong e_lo;
            ulong e_hi;
            e_lo = (ulong)e[16] | ((ulong)e[17] << 8) | ((ulong)e[18] << 16) |
                   ((ulong)e[19] << 24) | ((ulong)e[20] << 32) |
                   ((ulong)e[21] << 40) | ((ulong)e[22] << 48) |
                   ((ulong)e[23] << 56);
            e_hi = (ulong)e[24] | ((ulong)e[25] << 8) | ((ulong)e[26] << 16) |
                   ((ulong)e[27] << 24) | ((ulong)e[28] << 32) |
                   ((ulong)e[29] << 40) | ((ulong)e[30] << 48) |
                   ((ulong)e[31] << 56);
            if (e_lo != ff_lo || e_hi != ff_hi) {
              continue;
            }
            const uchar b0 = e[0];
            const uint max_inline = 16u / word_length;
            const uint num_inline_or_uninline =
                (b0 != 0) ? max_inline
                          : ((uint)e[12] | ((uint)e[13] << 8) |
                             ((uint)e[14] << 16) | ((uint)e[15] << 24));
            const uint word_start =
                (b0 != 0) ? 0u
                          : ((uint)e[8] | ((uint)e[9] << 8) |
                             ((uint)e[10] << 16) | ((uint)e[11] << 24));
            for (uint w = 0; w < num_inline_or_uninline; w++) {
              uchar wl_buf[16];
              if (b0 != 0) {
                const uint base = w * word_length;
                if (e[base] == 0) {
                  break;
                }
                for (uint j = 0; j < word_length; j++) {
                  wl_buf[j] = e[base + j];
                }
              } else {
                device const uchar *wl_base =
                    L_uninlined_letters + word_start + w * word_length;
                for (uint j = 0; j < word_length; j++) {
                  wl_buf[j] = wl_base[j];
                }
              }
              // Match + score; track minimum position_multipliers at
              // X1- and X2-positions for the blank discount.
              int score = base_score + bingo_to_add;
              bool ok = true;
              int min_pm_X1_first = INT_MAX;
              int min_pm_X1_second = INT_MAX;
              int min_pm_X2 = INT_MAX;
              for (uint j = 0; j < word_length && ok; j++) {
                const uchar wl = wl_buf[j];
                const uchar fl = fixed_letters[j];
                if (fl != 0) {
                  if (wl != fl) {
                    ok = false;
                    break;
                  }
                } else {
                  const ulong bit = ((ulong)1) << wl;
                  if ((cross_sets[j] & bit) == 0) {
                    ok = false;
                    break;
                  }
                  // Track candidate blank positions.
                  const int pm = position_multipliers[j];
                  if (X1 != 255u && wl == (uchar)X1) {
                    if (pm <= min_pm_X1_first) {
                      min_pm_X1_second = min_pm_X1_first;
                      min_pm_X1_first = pm;
                    } else if (pm < min_pm_X1_second) {
                      min_pm_X1_second = pm;
                    }
                  }
                  if (X2 != 255u && wl == (uchar)X2 && X1 != X2) {
                    if (pm < min_pm_X2) {
                      min_pm_X2 = pm;
                    }
                  }
                }
                score += letter_scores[wl] * position_multipliers[j];
              }
              if (!ok) {
                continue;
              }
              // Apply blank discount.
              int eq = score + leave_value;
              if (blank_in_subrack == 1) {
                if (min_pm_X1_first == INT_MAX) {
                  continue; // no X1 position — no valid blank placement
                }
                eq -= letter_scores[X1] * min_pm_X1_first;
              } else if (blank_in_subrack == 2) {
                if (X1 == X2) {
                  if (min_pm_X1_second == INT_MAX) {
                    continue; // need at least 2 X-positions
                  }
                  eq -= letter_scores[X1] *
                        (min_pm_X1_first + min_pm_X1_second);
                } else {
                  if (min_pm_X1_first == INT_MAX || min_pm_X2 == INT_MAX) {
                    continue;
                  }
                  eq -= letter_scores[X1] * min_pm_X1_first +
                        letter_scores[X2] * min_pm_X2;
                }
              }
              if (!any_match || eq > local_best) {
                local_best = eq;
                any_match = true;
              }
            }
            break;
          }
          if (X2 == 255u) break;
        }
        if (X1 == 255u) break;
      }
    }
    int k = 0;
    used[0]++;
    while (k < distinct_count && used[k] > distinct_max[k]) {
      used[k] = 0;
      k++;
      if (k < distinct_count) {
        used[k]++;
      }
    }
    if (k >= distinct_count) {
      break;
    }
  }
  if (any_match) {
    atomic_fetch_max_explicit(&best_eq_mp[gid], local_best,
                              memory_order_relaxed);
  }
}

// WMPG top-1 pass 2. Re-runs the same matching/scoring; for each match whose
// equity_mp == best_eq_mp[gid], either CAS-from-sentinel (speedy mode) or
// atomic_min (canonical mode) on a packed locator.
//
// Locator format identical to top1_loc_kernel:
//   bits 31..28: row (4) | 27..24: col (4) | 23: dir (1) |
//   22..19: length (4)   | 18..0: word_idx (19)
//
// CAVEAT: in WMPG-land we don't have a flat-lex word_idx. The kernel uses a
// per-thread "anagram counter" — a monotonically increasing index incremented
// for every validated anagram regardless of subrack. This gives a stable but
// implementation-defined ordering. Within (row, col, dir, length) the canonical
// tiebreak picks the smallest WMPG counter, which may differ from MAGPIE's
// flat-lex word_idx tiebreak when multiple words at the same slot tie at the
// exact same equity. The chosen move is still a correct best-equity move.
kernel void top1_loc_wmpg_kernel(
    device const uint *L_bucket_starts [[buffer(0)]],
    device const uchar *L_entries [[buffer(1)]],
    device const uchar *L_uninlined_letters [[buffer(2)]],
    device const uchar *racks_bytes [[buffer(3)]],
    constant uint &num_buckets [[buffer(4)]],
    constant uint &word_length [[buffer(5)]],
    constant uint &target_used_size [[buffer(6)]],
    constant ulong *cross_sets [[buffer(7)]],
    constant uchar *fixed_letters [[buffer(8)]],
    constant ulong &fixed_bitrack_low [[buffer(9)]],
    constant ulong &fixed_bitrack_high [[buffer(10)]],
    constant int *letter_scores [[buffer(11)]],
    constant int *position_multipliers [[buffer(12)]],
    constant int &base_score [[buffer(13)]],
    constant int &bingo_to_add [[buffer(14)]],
    constant uchar4 *leave_used [[buffer(15)]],
    constant int *leave_values [[buffer(16)]],
    constant uint &n_leaves [[buffer(17)]],
    device const int *best_eq_mp [[buffer(18)]],
    constant uint &row [[buffer(19)]],
    constant uint &col [[buffer(20)]],
    constant uint &dir [[buffer(21)]],
    constant uint &mode [[buffer(22)]],
    device atomic_uint *best_loc [[buffer(23)]],
    constant uint &leave_stride [[buffer(24)]],
    device const uint *b1_bucket_starts [[buffer(25)]],
    device const uchar *b1_entries [[buffer(26)]],
    constant uint &b1_num_buckets [[buffer(27)]],
    device const uint *b2_bucket_starts [[buffer(28)]],
    device const uchar *b2_entries [[buffer(29)]],
    constant uint &b2_num_buckets [[buffer(30)]],
    uint gid [[thread_position_in_grid]]) {
  const int target_eq = best_eq_mp[gid];
  const uint leave_base = gid * leave_stride;
  uchar letter_counts[32];
  device const uchar *my_rack = racks_bytes + gid * 16;
  for (int i = 0; i < 16; i++) {
    const uchar b = my_rack[i];
    letter_counts[2 * i + 0] = b & 0x0Fu;
    letter_counts[2 * i + 1] = (b >> 4) & 0x0Fu;
  }
  uchar distinct_ml[16];
  uchar distinct_max[16];
  int distinct_count = 0;
  int blank_idx = -1;
  for (int ml = 0; ml < 32; ml++) {
    if (letter_counts[ml] > 0) {
      if (distinct_count < 16) {
        if (ml == 0) {
          blank_idx = distinct_count;
        }
        distinct_ml[distinct_count] = (uchar)ml;
        distinct_max[distinct_count] = letter_counts[ml];
        distinct_count++;
      }
    }
  }
  uchar used[16];
  for (int k = 0; k < 16; k++) {
    used[k] = 0;
  }
  uint anagram_counter = 0;
  while (true) {
    int sum = 0;
    for (int k = 0; k < distinct_count; k++) {
      sum += used[k];
    }
    if ((uint)sum == target_used_size) {
      ulong tg_lo = fixed_bitrack_low;
      ulong tg_hi = fixed_bitrack_high;
      uchar used_letters[32] = {0};
      for (int k = 0; k < distinct_count; k++) {
        const int ml = distinct_ml[k];
        used_letters[ml] = used[k];
        const int shift = ml * 4;
        const ulong v = (ulong)used[k];
        if (shift < 64) {
          tg_lo += v << shift;
        } else {
          tg_hi += v << (shift - 64);
        }
      }
      uchar4 used_chunks[4];
      for (int i = 0; i < 4; i++) {
        uchar b0 = (used_letters[i * 8 + 0] | (used_letters[i * 8 + 1] << 4));
        uchar b1 = (used_letters[i * 8 + 2] | (used_letters[i * 8 + 3] << 4));
        uchar b2 = (used_letters[i * 8 + 4] | (used_letters[i * 8 + 5] << 4));
        uchar b3 = (used_letters[i * 8 + 6] | (used_letters[i * 8 + 7] << 4));
        used_chunks[i] = uchar4(b0, b1, b2, b3);
      }
      int leave_value = 0;
      for (uint k = 0; k < n_leaves; k++) {
        const uint ki = leave_base + k;
        const uchar4 ka = leave_used[ki * 4 + 0];
        const uchar4 kb = leave_used[ki * 4 + 1];
        const uchar4 kc = leave_used[ki * 4 + 2];
        const uchar4 kd = leave_used[ki * 4 + 3];
        if (all(used_chunks[0] == ka) && all(used_chunks[1] == kb) &&
            all(used_chunks[2] == kc) && all(used_chunks[3] == kd)) {
          leave_value = leave_values[ki];
          break;
        }
      }
      const int blank_in_subrack =
          (blank_idx >= 0) ? (int)used[blank_idx] : 0;
      uint b1_bv = 0;
      uint b2_bv = 0;
      if (blank_in_subrack == 1) {
        const ulong h_b1 = bitrack_mix_to_64(tg_lo, tg_hi);
        const uint bi = (uint)h_b1 & (b1_num_buckets - 1);
        const uint sb = b1_bucket_starts[bi];
        const uint eb = b1_bucket_starts[bi + 1];
        for (uint i = sb; i < eb; i++) {
          device const uchar *e = b1_entries + i * 32u;
          ulong elo = (ulong)e[16] | ((ulong)e[17] << 8) | ((ulong)e[18] << 16) |
                      ((ulong)e[19] << 24) | ((ulong)e[20] << 32) |
                      ((ulong)e[21] << 40) | ((ulong)e[22] << 48) |
                      ((ulong)e[23] << 56);
          ulong ehi = (ulong)e[24] | ((ulong)e[25] << 8) | ((ulong)e[26] << 16) |
                      ((ulong)e[27] << 24) | ((ulong)e[28] << 32) |
                      ((ulong)e[29] << 40) | ((ulong)e[30] << 48) |
                      ((ulong)e[31] << 56);
          if (elo == tg_lo && ehi == tg_hi) {
            b1_bv = (uint)e[8] | ((uint)e[9] << 8) | ((uint)e[10] << 16) |
                    ((uint)e[11] << 24);
            break;
          }
        }
      } else if (blank_in_subrack == 2) {
        const ulong h_b2 = bitrack_mix_to_64(tg_lo, tg_hi);
        const uint bi = (uint)h_b2 & (b2_num_buckets - 1);
        const uint sb = b2_bucket_starts[bi];
        const uint eb = b2_bucket_starts[bi + 1];
        for (uint i = sb; i < eb; i++) {
          device const uchar *e = b2_entries + i * 32u;
          ulong elo = (ulong)e[16] | ((ulong)e[17] << 8) | ((ulong)e[18] << 16) |
                      ((ulong)e[19] << 24) | ((ulong)e[20] << 32) |
                      ((ulong)e[21] << 40) | ((ulong)e[22] << 48) |
                      ((ulong)e[23] << 56);
          ulong ehi = (ulong)e[24] | ((ulong)e[25] << 8) | ((ulong)e[26] << 16) |
                      ((ulong)e[27] << 24) | ((ulong)e[28] << 32) |
                      ((ulong)e[29] << 40) | ((ulong)e[30] << 48) |
                      ((ulong)e[31] << 56);
          if (elo == tg_lo && ehi == tg_hi) {
            b2_bv = (uint)e[8] | ((uint)e[9] << 8) | ((uint)e[10] << 16) |
                    ((uint)e[11] << 24);
            break;
          }
        }
      }

      const uint x_max = 32u;
      uint x1_iter_start = 255u;
      uint x1_iter_end = 256u;
      if (blank_in_subrack == 1) {
        x1_iter_start = 1u;
        x1_iter_end = x_max;
      } else if (blank_in_subrack == 2) {
        x1_iter_start = 1u;
        x1_iter_end = x_max;
      }
      for (uint X1 = x1_iter_start;
           X1 < x1_iter_end || X1 == 255u; X1++) {
        if (X1 != 255u && blank_in_subrack == 1 && !(b1_bv & (1u << X1))) {
          continue;
        }
        if (X1 != 255u && blank_in_subrack == 2 && !(b2_bv & (1u << X1))) {
          continue;
        }
        ulong tf_lo = tg_lo;
        ulong tf_hi = tg_hi;
        if (blank_in_subrack >= 1 && X1 != 255u) {
          tf_lo -= 1ul;
          const int sh = (int)X1 * 4;
          if (sh < 64) tf_lo += (ulong)1 << sh;
          else tf_hi += (ulong)1 << (sh - 64);
        }
        uint b1_inner_bv = 0;
        if (blank_in_subrack == 2) {
          const ulong h_b1 = bitrack_mix_to_64(tf_lo, tf_hi);
          const uint bi = (uint)h_b1 & (b1_num_buckets - 1);
          const uint sb = b1_bucket_starts[bi];
          const uint eb = b1_bucket_starts[bi + 1];
          for (uint i = sb; i < eb; i++) {
            device const uchar *e = b1_entries + i * 32u;
            ulong elo = (ulong)e[16] | ((ulong)e[17] << 8) |
                        ((ulong)e[18] << 16) | ((ulong)e[19] << 24) |
                        ((ulong)e[20] << 32) | ((ulong)e[21] << 40) |
                        ((ulong)e[22] << 48) | ((ulong)e[23] << 56);
            ulong ehi = (ulong)e[24] | ((ulong)e[25] << 8) |
                        ((ulong)e[26] << 16) | ((ulong)e[27] << 24) |
                        ((ulong)e[28] << 32) | ((ulong)e[29] << 40) |
                        ((ulong)e[30] << 48) | ((ulong)e[31] << 56);
            if (elo == tf_lo && ehi == tf_hi) {
              b1_inner_bv = (uint)e[8] | ((uint)e[9] << 8) |
                            ((uint)e[10] << 16) | ((uint)e[11] << 24);
              break;
            }
          }
        }
        uint x2_iter_start = 255u;
        uint x2_iter_end = 256u;
        if (blank_in_subrack == 2) {
          x2_iter_start = 1u;
          x2_iter_end = x_max;
        }
        for (uint X2 = x2_iter_start;
             X2 < x2_iter_end || X2 == 255u; X2++) {
          if (X2 != 255u && blank_in_subrack == 2 &&
              !(b1_inner_bv & (1u << X2))) {
            continue;
          }
          ulong ff_lo = tf_lo;
          ulong ff_hi = tf_hi;
          if (blank_in_subrack == 2 && X2 != 255u) {
            ff_lo -= 1ul;
            const int sh = (int)X2 * 4;
            if (sh < 64) ff_lo += (ulong)1 << sh;
            else ff_hi += (ulong)1 << (sh - 64);
          }
          const ulong h = bitrack_mix_to_64(ff_lo, ff_hi);
          const uint bucket_idx = (uint)h & (num_buckets - 1);
          const uint start = L_bucket_starts[bucket_idx];
          const uint end = L_bucket_starts[bucket_idx + 1];
          for (uint i = start; i < end; i++) {
            device const uchar *e = L_entries + i * 32u;
            ulong e_lo;
            ulong e_hi;
            e_lo = (ulong)e[16] | ((ulong)e[17] << 8) | ((ulong)e[18] << 16) |
                   ((ulong)e[19] << 24) | ((ulong)e[20] << 32) |
                   ((ulong)e[21] << 40) | ((ulong)e[22] << 48) |
                   ((ulong)e[23] << 56);
            e_hi = (ulong)e[24] | ((ulong)e[25] << 8) | ((ulong)e[26] << 16) |
                   ((ulong)e[27] << 24) | ((ulong)e[28] << 32) |
                   ((ulong)e[29] << 40) | ((ulong)e[30] << 48) |
                   ((ulong)e[31] << 56);
            if (e_lo != ff_lo || e_hi != ff_hi) {
              continue;
            }
            const uchar b0 = e[0];
            const uint max_inline = 16u / word_length;
            const uint num_inline_or_uninline =
                (b0 != 0) ? max_inline
                          : ((uint)e[12] | ((uint)e[13] << 8) |
                             ((uint)e[14] << 16) | ((uint)e[15] << 24));
            const uint word_start =
                (b0 != 0) ? 0u
                          : ((uint)e[8] | ((uint)e[9] << 8) |
                             ((uint)e[10] << 16) | ((uint)e[11] << 24));
            for (uint w = 0; w < num_inline_or_uninline; w++) {
              uchar wl_buf[16];
              if (b0 != 0) {
                const uint base = w * word_length;
                if (e[base] == 0) {
                  break;
                }
                for (uint j = 0; j < word_length; j++) {
                  wl_buf[j] = e[base + j];
                }
              } else {
                device const uchar *wl_base =
                    L_uninlined_letters + word_start + w * word_length;
                for (uint j = 0; j < word_length; j++) {
                  wl_buf[j] = wl_base[j];
                }
              }
              int score = base_score + bingo_to_add;
              bool ok = true;
              for (uint j = 0; j < word_length && ok; j++) {
                const uchar wl = wl_buf[j];
                const uchar fl = fixed_letters[j];
                if (fl != 0) {
                  if (wl != fl) {
                    ok = false;
                    break;
                  }
                } else {
                  const ulong bit = ((ulong)1) << wl;
                  if ((cross_sets[j] & bit) == 0) {
                    ok = false;
                    break;
                  }
                }
                score += letter_scores[wl] * position_multipliers[j];
              }
              if (!ok) {
                continue;
              }
              // Enumerate blank-position placements; for each, compute eq
              // and write locator if it matches target_eq.
              if (blank_in_subrack == 0) {
                const int eq = score + leave_value;
                if (eq == target_eq) {
                  const uint packed = ((row & 0xFu) << 28) |
                                      ((col & 0xFu) << 24) |
                                      ((dir & 0x1u) << 23) |
                                      ((word_length & 0xFu) << 19) |
                                      (anagram_counter & 0x7FFFFu);
                  if (mode == 0u) {
                    uint expected = 0xFFFFFFFFu;
                    atomic_compare_exchange_weak_explicit(
                        &best_loc[gid], &expected, packed,
                        memory_order_relaxed, memory_order_relaxed);
                  } else {
                    atomic_fetch_min_explicit(&best_loc[gid], packed,
                                              memory_order_relaxed);
                  }
                }
                anagram_counter++;
              } else if (blank_in_subrack == 1) {
                for (uint p = 0; p < word_length; p++) {
                  if (wl_buf[p] != (uchar)X1 || fixed_letters[p] != 0) {
                    continue;
                  }
                  const int eq = score + leave_value -
                                 letter_scores[X1] * position_multipliers[p];
                  if (eq == target_eq) {
                    const uint packed = ((row & 0xFu) << 28) |
                                        ((col & 0xFu) << 24) |
                                        ((dir & 0x1u) << 23) |
                                        ((word_length & 0xFu) << 19) |
                                        (anagram_counter & 0x7FFFFu);
                    if (mode == 0u) {
                      uint expected = 0xFFFFFFFFu;
                      atomic_compare_exchange_weak_explicit(
                          &best_loc[gid], &expected, packed,
                          memory_order_relaxed, memory_order_relaxed);
                    } else {
                      atomic_fetch_min_explicit(&best_loc[gid], packed,
                                                memory_order_relaxed);
                    }
                  }
                  anagram_counter++;
                }
              } else { // blank_in_subrack == 2
                for (uint p1 = 0; p1 < word_length; p1++) {
                  if (wl_buf[p1] != (uchar)X1 || fixed_letters[p1] != 0) {
                    continue;
                  }
                  const uint p2_lo = (X1 == X2) ? (p1 + 1) : 0u;
                  for (uint p2 = p2_lo; p2 < word_length; p2++) {
                    if (p2 == p1) continue;
                    if (wl_buf[p2] != (uchar)X2 || fixed_letters[p2] != 0) {
                      continue;
                    }
                    const int eq = score + leave_value -
                                   letter_scores[X1] *
                                       position_multipliers[p1] -
                                   letter_scores[X2] *
                                       position_multipliers[p2];
                    if (eq == target_eq) {
                      const uint packed = ((row & 0xFu) << 28) |
                                          ((col & 0xFu) << 24) |
                                          ((dir & 0x1u) << 23) |
                                          ((word_length & 0xFu) << 19) |
                                          (anagram_counter & 0x7FFFFu);
                      if (mode == 0u) {
                        uint expected = 0xFFFFFFFFu;
                        atomic_compare_exchange_weak_explicit(
                            &best_loc[gid], &expected, packed,
                            memory_order_relaxed, memory_order_relaxed);
                      } else {
                        atomic_fetch_min_explicit(&best_loc[gid], packed,
                                                  memory_order_relaxed);
                      }
                    }
                    anagram_counter++;
                  }
                }
              }
            }
            break;
          }
          if (X2 == 255u) break;
        }
        if (X1 == 255u) break;
      }
    }
    int k = 0;
    used[0]++;
    while (k < distinct_count && used[k] > distinct_max[k]) {
      used[k] = 0;
      k++;
      if (k < distinct_count) {
        used[k]++;
      }
    }
    if (k >= distinct_count) {
      break;
    }
  }
}
