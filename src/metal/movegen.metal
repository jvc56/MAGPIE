// Phase-3 non-playthrough match kernels.
//
// BitRack encoding: 32 letters * 4 bits, packed into 16 bytes (= 4 uchar4).
// Subset test: for every letter, word.count[ml] <= rack.count[ml]. We split
// each byte into its low and high nibble (treating both as 0..15) and use
// vector compares.
//
// match_kernel — one thread per (rack, word). Writes B*n_words flag bytes.
//                Used for B=1 correctness validation against CPU.
// count_kernel — same subset test, but each match atomically increments a
//                per-rack count. Output is B uint32s; safe to dispatch at
//                very large B without materializing a flag matrix.

#include <metal_stdlib>
using namespace metal;

inline bool subset_chunk(uchar4 word, uchar4 rack) {
  const uchar4 lo_mask = uchar4(0x0F);
  return all((word & lo_mask) <= (rack & lo_mask)) &&
         all(((word >> 4) & lo_mask) <= ((rack >> 4) & lo_mask));
}

inline bool subset_test_at(device const uchar4 *bitracks, uint word_idx,
                           device const uchar4 *racks, uint rack_idx) {
  const uint w_off = word_idx * 4;
  const uint r_off = rack_idx * 4;
  return subset_chunk(bitracks[w_off + 0], racks[r_off + 0]) &&
         subset_chunk(bitracks[w_off + 1], racks[r_off + 1]) &&
         subset_chunk(bitracks[w_off + 2], racks[r_off + 2]) &&
         subset_chunk(bitracks[w_off + 3], racks[r_off + 3]);
}

kernel void match_kernel(device const uchar4 *bitracks [[buffer(0)]],
                         device const uchar4 *racks [[buffer(1)]],
                         constant uint &n_words [[buffer(2)]],
                         device uchar *flags [[buffer(3)]],
                         uint2 gid [[thread_position_in_grid]]) {
  const uint word_idx = gid.x;
  const uint rack_idx = gid.y;
  if (word_idx >= n_words) {
    return;
  }
  const bool ok = subset_test_at(bitracks, word_idx, racks, rack_idx);
  flags[rack_idx * n_words + word_idx] = ok ? 1 : 0;
}

kernel void count_kernel(device const uchar4 *bitracks [[buffer(0)]],
                         device const uchar4 *racks [[buffer(1)]],
                         constant uint &n_words [[buffer(2)]],
                         device atomic_uint *counts [[buffer(3)]],
                         uint2 gid [[thread_position_in_grid]]) {
  const uint word_idx = gid.x;
  const uint rack_idx = gid.y;
  if (word_idx >= n_words) {
    return;
  }
  if (subset_test_at(bitracks, word_idx, racks, rack_idx)) {
    atomic_fetch_add_explicit(&counts[rack_idx], 1u, memory_order_relaxed);
  }
}

// Cross-check-filtered count kernel. In addition to the subset test, requires
// each letter of the word to satisfy its cross-set bit at the corresponding
// slot position.
//   bitracks: words' bitracks (already advanced to first_word by buffer offset)
//   letters:  words' letters (advanced to length L's start)
//   word_length L: bytes per word in `letters`
//   cross_sets: L uint64 bitvectors, cross_sets[i] = legal letters at slot pos i
kernel void count_with_cross_kernel(device const uchar4 *bitracks [[buffer(0)]],
                                    device const uchar *letters [[buffer(1)]],
                                    device const uchar4 *racks [[buffer(2)]],
                                    constant uint &n_words [[buffer(3)]],
                                    constant uint &word_length [[buffer(4)]],
                                    constant ulong *cross_sets [[buffer(5)]],
                                    device atomic_uint *counts [[buffer(6)]],
                                    uint2 gid
                                    [[thread_position_in_grid]]) {
  const uint word_idx = gid.x;
  const uint rack_idx = gid.y;
  if (word_idx >= n_words) {
    return;
  }
  if (!subset_test_at(bitracks, word_idx, racks, rack_idx)) {
    return;
  }
  device const uchar *word_letters = letters + word_idx * word_length;
  for (uint i = 0; i < word_length; i++) {
    const ulong bit = ((ulong)1) << word_letters[i];
    if ((cross_sets[i] & bit) == 0) {
      return;
    }
  }
  atomic_fetch_add_explicit(&counts[rack_idx], 1u, memory_order_relaxed);
}

// WMPG-based count kernel. Per-rack thread (one thread per rack, not per
// word-pair like the brute-force kernels). For each rack:
//   1. Decode rack into 32 per-letter counts (low+high nibble of each of 16
//      bytes).
//   2. Enumerate "used" multisets ⊆ rack with size == target_used_size
//      = word_length - fixed_count. Iterates all distinct-letter combos.
//   3. For each used: target_bitrack = used + fixed_bitrack. Compute
//      bit_rack_mix_to_64(target) → bucket index in WMPG length-L table.
//   4. Walk entries in [bucket_starts[bucket], bucket_starts[bucket+1])
//      comparing bitrack. On match, decode entry: inlined letters in entry
//      bytes 0..15, OR uninlined letters at uninlined_letters[word_start..].
//   5. For each anagram, verify each non-fixed position's letter is in
//      cross_sets[i] AND each fixed position's word letter equals
//      fixed_letters[i]. If all pass, atomic_add into per-rack count.
//
// Buffers per dispatch (one slot's length L):
//   L_bucket_starts (uint32 array, length num_buckets+1)
//   L_entries (uchar*, num_entries × 32 bytes)
//   L_uninlined_letters (uchar*, num_uninlined_words × L bytes)
//   racks (B × 16 bytes)
//   constants: num_buckets, word_length, target_used_size, cross_sets,
//              fixed_letters, fixed_bitrack
//   output: B uint32 atomic counts

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

// Pack a uchar4×4 BitRack into two ulongs (low and high 64 bits).
inline void bitrack_to_ulongs(uchar4 a, uchar4 b, uchar4 c, uchar4 d,
                              thread ulong *lo_out, thread ulong *hi_out) {
  ulong lo = (ulong)a.x | ((ulong)a.y << 8) | ((ulong)a.z << 16) |
             ((ulong)a.w << 24) | ((ulong)b.x << 32) | ((ulong)b.y << 40) |
             ((ulong)b.z << 48) | ((ulong)b.w << 56);
  ulong hi = (ulong)c.x | ((ulong)c.y << 8) | ((ulong)c.z << 16) |
             ((ulong)c.w << 24) | ((ulong)d.x << 32) | ((ulong)d.y << 40) |
             ((ulong)d.z << 48) | ((ulong)d.w << 56);
  *lo_out = lo;
  *hi_out = hi;
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

// Top-1 (best move per rack by equity), two-pass design for millipoint
// precision (M2 doesn't support 64-bit shader atomics).
//
//   Pass 1 (top1_eqmp_kernel): atomic_fetch_max on per-rack int32 of
//     equity_millipoints. Accumulates across all slot dispatches. After
//     pass 1 completes, best_eq_mp[rack] is the max equity over all matches
//     for that rack at any slot.
//   Pass 2 (top1_loc_kernel): re-runs the matching+scoring; for each match
//     whose equity_mp == best_eq_mp[rack], atomically compare-exchanges into
//     best_loc[rack] (sentinel 0xFFFFFFFF -> packed (slot_id<<16 | word_idx)).
//     First match to land at max wins; later matches see non-sentinel and skip.
//
// Output buffers (caller-allocated, B entries each):
//   best_eq_mp: signed int32, initialized to INT32_MIN before first pass-1
//   best_loc:   uint32, initialized to 0xFFFFFFFF before first pass-2
//
// Both kernels share the same matching/scoring/leave-lookup logic; only the
// final output protocol differs.
// `leave_stride` (in entries): 0 = shared leaves (all racks read leave_used
// at offset 0..n_leaves-1, legacy behavior). Nonzero = per-rack leaves with
// that stride; rack i reads [i*stride, i*stride + n_leaves). Caller pads
// each rack's table to stride entries with sentinel-unmatching bitracks.
kernel void top1_eqmp_kernel(device const uchar4 *bitracks [[buffer(0)]],
                             device const uchar *letters [[buffer(1)]],
                             device const uchar4 *racks [[buffer(2)]],
                             constant uint &n_words [[buffer(3)]],
                             constant uint &word_length [[buffer(4)]],
                             constant ulong *cross_sets [[buffer(5)]],
                             constant uchar *fixed_letters [[buffer(6)]],
                             constant uchar4 *fixed_bitrack_chunks [[buffer(7)]],
                             constant int *letter_scores [[buffer(8)]],
                             constant int *position_multipliers [[buffer(9)]],
                             constant int &base_score [[buffer(10)]],
                             constant int &bingo_to_add [[buffer(11)]],
                             constant uchar4 *leave_used [[buffer(12)]],
                             constant int *leave_values [[buffer(13)]],
                             constant uint &n_leaves [[buffer(14)]],
                             device atomic_int *best_eq_mp [[buffer(15)]],
                             constant uint &leave_stride [[buffer(16)]],
                             uint2 gid [[thread_position_in_grid]]) {
  const uint word_idx = gid.x;
  const uint rack_idx = gid.y;
  if (word_idx >= n_words) {
    return;
  }
  const uint w_off = word_idx * 4;
  const uint r_off = rack_idx * 4;
  const uchar4 wa = bitracks[w_off + 0];
  const uchar4 wb = bitracks[w_off + 1];
  const uchar4 wc = bitracks[w_off + 2];
  const uchar4 wd = bitracks[w_off + 3];
  const uchar4 fa = fixed_bitrack_chunks[0];
  const uchar4 fb = fixed_bitrack_chunks[1];
  const uchar4 fc = fixed_bitrack_chunks[2];
  const uchar4 fd = fixed_bitrack_chunks[3];
  const uchar4 ra = racks[r_off + 0] + fa;
  const uchar4 rb = racks[r_off + 1] + fb;
  const uchar4 rc = racks[r_off + 2] + fc;
  const uchar4 rd = racks[r_off + 3] + fd;
  if (!(subset_chunk(wa, ra) && subset_chunk(wb, rb) &&
        subset_chunk(wc, rc) && subset_chunk(wd, rd))) {
    return;
  }
  device const uchar *word_letters = letters + word_idx * word_length;
  int score = base_score + bingo_to_add;
  for (uint i = 0; i < word_length; i++) {
    const uchar wl = word_letters[i];
    const uchar fl = fixed_letters[i];
    if (fl != 0) {
      if (wl != fl) {
        return;
      }
    } else {
      const ulong bit = ((ulong)1) << wl;
      if ((cross_sets[i] & bit) == 0) {
        return;
      }
    }
    score += letter_scores[wl] * position_multipliers[i];
  }
  const uchar4 ua = wa - fa;
  const uchar4 ub = wb - fb;
  const uchar4 uc = wc - fc;
  const uchar4 ud = wd - fd;
  const uint leave_base = rack_idx * leave_stride;
  int leave_value = 0;
  for (uint k = 0; k < n_leaves; k++) {
    const uint ki = leave_base + k;
    const uchar4 ka = leave_used[ki * 4 + 0];
    const uchar4 kb = leave_used[ki * 4 + 1];
    const uchar4 kc = leave_used[ki * 4 + 2];
    const uchar4 kd = leave_used[ki * 4 + 3];
    if (all(ua == ka) && all(ub == kb) && all(uc == kc) && all(ud == kd)) {
      leave_value = leave_values[ki];
      break;
    }
  }
  const int equity_mp = score + leave_value;
  atomic_fetch_max_explicit(&best_eq_mp[rack_idx], equity_mp,
                            memory_order_relaxed);
}

// Pass 2: locate. Re-runs match+equity, compares to best_eq_mp[rack]; on tie,
// updates best_loc[rack] using one of two tiebreak modes.
//   mode == 0 (speedy):    CAS first-wins from sentinel 0xFFFFFFFF.
//                          One atomic op per match, non-deterministic across
//                          ties.
//   mode != 0 (canonical): atomic_fetch_min on packed locator.
//                          Smaller packed wins → MAGPIE-equivalent ordering on
//                          (row, col, dir, length, word_idx).
// Both modes write the same locator format:
//   bits 31..28: row (4 bits)
//   bits 27..24: col (4 bits)
//   bit 23:      dir (0 = horizontal wins under min)
//   bits 22..19: length (4 bits)
//   bits 18..0:  word_idx within length slice (19 bits, 524288)
// Caller decodes via the same shifts/masks regardless of mode.
kernel void top1_loc_kernel(device const uchar4 *bitracks [[buffer(0)]],
                            device const uchar *letters [[buffer(1)]],
                            device const uchar4 *racks [[buffer(2)]],
                            constant uint &n_words [[buffer(3)]],
                            constant uint &word_length [[buffer(4)]],
                            constant ulong *cross_sets [[buffer(5)]],
                            constant uchar *fixed_letters [[buffer(6)]],
                            constant uchar4 *fixed_bitrack_chunks [[buffer(7)]],
                            constant int *letter_scores [[buffer(8)]],
                            constant int *position_multipliers [[buffer(9)]],
                            constant int &base_score [[buffer(10)]],
                            constant int &bingo_to_add [[buffer(11)]],
                            constant uchar4 *leave_used [[buffer(12)]],
                            constant int *leave_values [[buffer(13)]],
                            constant uint &n_leaves [[buffer(14)]],
                            device const int *best_eq_mp [[buffer(15)]],
                            constant uint &row [[buffer(16)]],
                            constant uint &col [[buffer(17)]],
                            constant uint &dir [[buffer(18)]],
                            constant uint &mode [[buffer(19)]],
                            device atomic_uint *best_loc [[buffer(20)]],
                            constant uint &leave_stride [[buffer(21)]],
                            uint2 gid [[thread_position_in_grid]]) {
  const uint word_idx = gid.x;
  const uint rack_idx = gid.y;
  if (word_idx >= n_words) {
    return;
  }
  const uint w_off = word_idx * 4;
  const uint r_off = rack_idx * 4;
  const uchar4 wa = bitracks[w_off + 0];
  const uchar4 wb = bitracks[w_off + 1];
  const uchar4 wc = bitracks[w_off + 2];
  const uchar4 wd = bitracks[w_off + 3];
  const uchar4 fa = fixed_bitrack_chunks[0];
  const uchar4 fb = fixed_bitrack_chunks[1];
  const uchar4 fc = fixed_bitrack_chunks[2];
  const uchar4 fd = fixed_bitrack_chunks[3];
  const uchar4 ra = racks[r_off + 0] + fa;
  const uchar4 rb = racks[r_off + 1] + fb;
  const uchar4 rc = racks[r_off + 2] + fc;
  const uchar4 rd = racks[r_off + 3] + fd;
  if (!(subset_chunk(wa, ra) && subset_chunk(wb, rb) &&
        subset_chunk(wc, rc) && subset_chunk(wd, rd))) {
    return;
  }
  device const uchar *word_letters = letters + word_idx * word_length;
  int score = base_score + bingo_to_add;
  for (uint i = 0; i < word_length; i++) {
    const uchar wl = word_letters[i];
    const uchar fl = fixed_letters[i];
    if (fl != 0) {
      if (wl != fl) {
        return;
      }
    } else {
      const ulong bit = ((ulong)1) << wl;
      if ((cross_sets[i] & bit) == 0) {
        return;
      }
    }
    score += letter_scores[wl] * position_multipliers[i];
  }
  const uchar4 ua = wa - fa;
  const uchar4 ub = wb - fb;
  const uchar4 uc = wc - fc;
  const uchar4 ud = wd - fd;
  const uint leave_base = rack_idx * leave_stride;
  int leave_value = 0;
  for (uint k = 0; k < n_leaves; k++) {
    const uint ki = leave_base + k;
    const uchar4 ka = leave_used[ki * 4 + 0];
    const uchar4 kb = leave_used[ki * 4 + 1];
    const uchar4 kc = leave_used[ki * 4 + 2];
    const uchar4 kd = leave_used[ki * 4 + 3];
    if (all(ua == ka) && all(ub == kb) && all(uc == kc) && all(ud == kd)) {
      leave_value = leave_values[ki];
      break;
    }
  }
  const int equity_mp = score + leave_value;
  if (equity_mp != best_eq_mp[rack_idx]) {
    return;
  }
  const uint packed = ((row & 0xFu) << 28) | ((col & 0xFu) << 24) |
                      ((dir & 0x1u) << 23) | ((word_length & 0xFu) << 19) |
                      (word_idx & 0x7FFFFu);
  if (mode == 0u) {
    uint expected = 0xFFFFFFFFu;
    atomic_compare_exchange_weak_explicit(&best_loc[rack_idx], &expected,
                                          packed, memory_order_relaxed,
                                          memory_order_relaxed);
  } else {
    atomic_fetch_min_explicit(&best_loc[rack_idx], packed,
                              memory_order_relaxed);
  }
}

// Equity kernel = score kernel + per-match leave lookup. Per match, computes
// used_bitrack = word - fixed (the letters drawn from the rack), then linearly
// scans a small per-rack leave_table to find the matching used and retrieve
// its precomputed leave_value. Equity = score + leave_value.
//
// leave_table layout: n_leaves entries, each entry is 4 uchar4 (16-byte
// used_bitrack) followed in a parallel array by an int32 leave_value.
// Linear scan is fine because n_leaves is small (≤ ~256 for size-7 racks).
kernel void equity_with_playthrough_kernel(
    device const uchar4 *bitracks [[buffer(0)]],
    device const uchar *letters [[buffer(1)]],
    device const uchar4 *racks [[buffer(2)]],
    constant uint &n_words [[buffer(3)]],
    constant uint &word_length [[buffer(4)]],
    constant ulong *cross_sets [[buffer(5)]],
    constant uchar *fixed_letters [[buffer(6)]],
    constant uchar4 *fixed_bitrack_chunks [[buffer(7)]],
    constant int *letter_scores [[buffer(8)]],
    constant int *position_multipliers [[buffer(9)]],
    constant int &base_score [[buffer(10)]],
    constant int &bingo_to_add [[buffer(11)]],
    constant uchar4 *leave_used [[buffer(12)]],
    constant int *leave_values [[buffer(13)]],
    constant uint &n_leaves [[buffer(14)]],
    device atomic_uint *output [[buffer(15)]],
    uint2 gid [[thread_position_in_grid]]) {
  const uint word_idx = gid.x;
  const uint rack_idx = gid.y;
  if (word_idx >= n_words) {
    return;
  }
  const uint w_off = word_idx * 4;
  const uint r_off = rack_idx * 4;
  const uchar4 wa = bitracks[w_off + 0];
  const uchar4 wb = bitracks[w_off + 1];
  const uchar4 wc = bitracks[w_off + 2];
  const uchar4 wd = bitracks[w_off + 3];
  const uchar4 fa = fixed_bitrack_chunks[0];
  const uchar4 fb = fixed_bitrack_chunks[1];
  const uchar4 fc = fixed_bitrack_chunks[2];
  const uchar4 fd = fixed_bitrack_chunks[3];
  const uchar4 ra = racks[r_off + 0] + fa;
  const uchar4 rb = racks[r_off + 1] + fb;
  const uchar4 rc = racks[r_off + 2] + fc;
  const uchar4 rd = racks[r_off + 3] + fd;
  if (!(subset_chunk(wa, ra) && subset_chunk(wb, rb) &&
        subset_chunk(wc, rc) && subset_chunk(wd, rd))) {
    return;
  }
  device const uchar *word_letters = letters + word_idx * word_length;
  int score = base_score + bingo_to_add;
  for (uint i = 0; i < word_length; i++) {
    const uchar wl = word_letters[i];
    const uchar fl = fixed_letters[i];
    if (fl != 0) {
      if (wl != fl) {
        return;
      }
    } else {
      const ulong bit = ((ulong)1) << wl;
      if ((cross_sets[i] & bit) == 0) {
        return;
      }
    }
    score += letter_scores[wl] * position_multipliers[i];
  }
  // used = word - fixed (per-byte, safe because subset test ensures
  // word_count[ml] >= fixed_count[ml] at fixed positions, and other letters
  // have fixed_count[ml] = 0).
  const uchar4 ua = wa - fa;
  const uchar4 ub = wb - fb;
  const uchar4 uc = wc - fc;
  const uchar4 ud = wd - fd;
  int leave_value = 0;
  for (uint k = 0; k < n_leaves; k++) {
    const uchar4 ka = leave_used[k * 4 + 0];
    const uchar4 kb = leave_used[k * 4 + 1];
    const uchar4 kc = leave_used[k * 4 + 2];
    const uchar4 kd = leave_used[k * 4 + 3];
    if (all(ua == ka) && all(ub == kb) && all(uc == kc) && all(ud == kd)) {
      leave_value = leave_values[k];
      break;
    }
  }
  const int equity = score + leave_value;
  atomic_fetch_add_explicit(&output[2 * rack_idx + 0], 1u,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&output[2 * rack_idx + 1], (uint)equity,
                            memory_order_relaxed);
}

// Score-and-count kernel. For each (rack, word) pair that passes
// playthrough+cross-set+subset, computes the placement score and atomically
// adds it to per-rack accumulators (output[2*rack_idx]=count,
// output[2*rack_idx+1]=score_sum).
//
// Score model matches MAGPIE's record_tile_placement_move:
//   total = sum_over_all_i(letter_scores[word.letters[i]] * multiplier[i])
//         + base_score
//         + bingo_to_add (if conditions met, computed at host)
// where multiplier[i] = 0 at fixed (playthrough) positions, else
//   letter_mult[i] * (prod_word_mult + is_cross_word[i] * this_word_mult[i])
// and base_score = playthrough_score * prod_word_mult + hooked_cross_total
// (sum of cross_scores at placed positions times their word_mults).
// All slot-constant terms are pre-computed by the host and passed in.
kernel void score_with_playthrough_kernel(
    device const uchar4 *bitracks [[buffer(0)]],
    device const uchar *letters [[buffer(1)]],
    device const uchar4 *racks [[buffer(2)]],
    constant uint &n_words [[buffer(3)]],
    constant uint &word_length [[buffer(4)]],
    constant ulong *cross_sets [[buffer(5)]],
    constant uchar *fixed_letters [[buffer(6)]],
    constant uchar4 *fixed_bitrack_chunks [[buffer(7)]],
    constant int *letter_scores [[buffer(8)]],   // 32 ints
    constant int *position_multipliers [[buffer(9)]], // L ints
    constant int &base_score [[buffer(10)]],
    constant int &bingo_to_add [[buffer(11)]],
    device atomic_uint *output [[buffer(12)]],
    uint2 gid [[thread_position_in_grid]]) {
  const uint word_idx = gid.x;
  const uint rack_idx = gid.y;
  if (word_idx >= n_words) {
    return;
  }
  const uint w_off = word_idx * 4;
  const uint r_off = rack_idx * 4;
  const uchar4 wa = bitracks[w_off + 0];
  const uchar4 wb = bitracks[w_off + 1];
  const uchar4 wc = bitracks[w_off + 2];
  const uchar4 wd = bitracks[w_off + 3];
  const uchar4 ra = racks[r_off + 0] + fixed_bitrack_chunks[0];
  const uchar4 rb = racks[r_off + 1] + fixed_bitrack_chunks[1];
  const uchar4 rc = racks[r_off + 2] + fixed_bitrack_chunks[2];
  const uchar4 rd = racks[r_off + 3] + fixed_bitrack_chunks[3];
  if (!(subset_chunk(wa, ra) && subset_chunk(wb, rb) &&
        subset_chunk(wc, rc) && subset_chunk(wd, rd))) {
    return;
  }
  device const uchar *word_letters = letters + word_idx * word_length;
  int score = base_score + bingo_to_add;
  for (uint i = 0; i < word_length; i++) {
    const uchar wl = word_letters[i];
    const uchar fl = fixed_letters[i];
    if (fl != 0) {
      if (wl != fl) {
        return;
      }
    } else {
      const ulong bit = ((ulong)1) << wl;
      if ((cross_sets[i] & bit) == 0) {
        return;
      }
    }
    score += letter_scores[wl] * position_multipliers[i];
  }
  atomic_fetch_add_explicit(&output[2 * rack_idx + 0], 1u,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&output[2 * rack_idx + 1], (uint)score,
                            memory_order_relaxed);
}

// Playthrough-aware count kernel. For each (rack, word) pair, counts a match
// iff:
//   1. word.bitrack ⊆ (rack.bitrack + fixed_bitrack)  [effective rack]
//   2. for each i in 0..word_length-1:
//        if fixed_letters[i] != 0: word.letters[i] == fixed_letters[i]
//        else: bit (1<<word.letters[i]) set in cross_sets[i]
// fixed_bitrack is the BitRack of the playthrough tiles in this slot.
// Empty-slot case: pass fixed_letters = all zero, fixed_bitrack = 0; this
// degenerates to the count_with_cross_kernel.
kernel void count_with_playthrough_kernel(
    device const uchar4 *bitracks [[buffer(0)]],
    device const uchar *letters [[buffer(1)]],
    device const uchar4 *racks [[buffer(2)]],
    constant uint &n_words [[buffer(3)]],
    constant uint &word_length [[buffer(4)]],
    constant ulong *cross_sets [[buffer(5)]],
    constant uchar *fixed_letters [[buffer(6)]],
    constant uchar4 *fixed_bitrack_chunks [[buffer(7)]], // 4 uchar4 = 16 bytes
    device atomic_uint *counts [[buffer(8)]],
    uint2 gid [[thread_position_in_grid]]) {
  const uint word_idx = gid.x;
  const uint rack_idx = gid.y;
  if (word_idx >= n_words) {
    return;
  }
  // Effective rack = rack + fixed_bitrack (byte-wise add; per-nibble sum is
  // bounded by rack_size + slot_length ≤ 14 for 7-tile rack and 7-cell slot,
  // so no nibble overflow into the next byte).
  const uint w_off = word_idx * 4;
  const uint r_off = rack_idx * 4;
  const uchar4 wa = bitracks[w_off + 0];
  const uchar4 wb = bitracks[w_off + 1];
  const uchar4 wc = bitracks[w_off + 2];
  const uchar4 wd = bitracks[w_off + 3];
  const uchar4 ra = racks[r_off + 0] + fixed_bitrack_chunks[0];
  const uchar4 rb = racks[r_off + 1] + fixed_bitrack_chunks[1];
  const uchar4 rc = racks[r_off + 2] + fixed_bitrack_chunks[2];
  const uchar4 rd = racks[r_off + 3] + fixed_bitrack_chunks[3];
  if (!(subset_chunk(wa, ra) && subset_chunk(wb, rb) &&
        subset_chunk(wc, rc) && subset_chunk(wd, rd))) {
    return;
  }
  device const uchar *word_letters = letters + word_idx * word_length;
  for (uint i = 0; i < word_length; i++) {
    const uchar wl = word_letters[i];
    const uchar fl = fixed_letters[i];
    if (fl != 0) {
      if (wl != fl) {
        return;
      }
    } else {
      const ulong bit = ((ulong)1) << wl;
      if ((cross_sets[i] & bit) == 0) {
        return;
      }
    }
  }
  atomic_fetch_add_explicit(&counts[rack_idx], 1u, memory_order_relaxed);
}
