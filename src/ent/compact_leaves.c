#include "compact_leaves.h"

#include "../def/equity_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/rack.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Explicit little-endian byte (de)serialization so a .clv is byte-identical on
// any CPU (it is meant to be transported to constrained systems).
static inline void put_u16(uint8_t *b, size_t *p, uint16_t v) {
  b[(*p)++] = (uint8_t)(v & 0xFF);
  b[(*p)++] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void put_u32(uint8_t *b, size_t *p, uint32_t v) {
  for (int byte_idx = 0; byte_idx < 4; byte_idx++) {
    b[(*p)++] = (uint8_t)((v >> (8 * byte_idx)) & 0xFF);
  }
}
static inline void put_u64(uint8_t *b, size_t *p, uint64_t v) {
  for (int byte_idx = 0; byte_idx < 8; byte_idx++) {
    b[(*p)++] = (uint8_t)((v >> (8 * byte_idx)) & 0xFF);
  }
}
static inline uint16_t get_u16(const uint8_t *b, size_t *p) {
  const uint16_t v = (uint16_t)(b[*p] | ((uint16_t)b[*p + 1] << 8));
  *p += 2;
  return v;
}
static inline uint32_t get_u32(const uint8_t *b, size_t *p) {
  uint32_t v = 0;
  for (int byte_idx = 0; byte_idx < 4; byte_idx++) {
    v |= (uint32_t)b[*p + byte_idx] << (8 * byte_idx);
  }
  *p += 4;
  return v;
}
static inline uint64_t get_u64(const uint8_t *b, size_t *p) {
  uint64_t v = 0;
  for (int byte_idx = 0; byte_idx < 8; byte_idx++) {
    v |= (uint64_t)b[*p + byte_idx] << (8 * byte_idx);
  }
  *p += 8;
  return v;
}

// ---- sub-byte bit packing (LSB-first) for the optional bit-packed body ----
// Self-contained (does not depend on the DAWG modules) so .clv stays
// orthogonal.
static int compact_leaves_bits_needed(uint32_t max_value) {
  int bits = 0;
  while (max_value > 0) {
    bits++;
    max_value >>= 1;
  }
  return bits < 1 ? 1 : bits;
}
// Map signed <-> unsigned so small-magnitude coefficients pack into few bits.
static inline uint32_t compact_leaves_zigzag(int32_t value) {
  const uint32_t sign = value < 0 ? 0xFFFFFFFFU : 0U;
  return ((uint32_t)value << 1) ^ sign;
}
static inline int32_t compact_leaves_unzigzag(uint32_t encoded) {
  return (int32_t)(encoded >> 1) ^ -(int32_t)(encoded & 1U);
}
// buf must be zero-initialized over the bit range (writes via OR).
static void put_bits(uint8_t *buf, size_t *bit_pos, uint32_t value, int nbits) {
  for (int i = 0; i < nbits; i++) {
    if ((value >> i) & 1U) {
      buf[(*bit_pos + (size_t)i) >> 3] |=
          (uint8_t)(1U << ((*bit_pos + (size_t)i) & 7U));
    }
  }
  *bit_pos += (size_t)nbits;
}
static uint32_t get_bits(const uint8_t *buf, size_t *bit_pos, int nbits) {
  uint32_t value = 0;
  for (int i = 0; i < nbits; i++) {
    if ((buf[(*bit_pos + (size_t)i) >> 3] >> ((*bit_pos + (size_t)i) & 7U)) &
        1U) {
      value |= (1U << i);
    }
  }
  *bit_pos += (size_t)nbits;
  return value;
}
// Bit widths derived identically by writer and reader from dist_size / the
// max-synergy-tiles constant (so they need not be stored).
static int compact_leaves_tile_bits(const CompactLeaves *cl) {
  return compact_leaves_bits_needed((uint32_t)cl->dist_size - 1);
}
static int compact_leaves_num_tiles_bits(void) {
  return compact_leaves_bits_needed(COMPACT_LEAVES_MAX_SYNERGY_TILES);
}

static inline int compact_leaves_vc_count(const CompactLeaves *cl) {
  return (cl->flags & COMPACT_LEAVES_FLAG_VC_TABLE)
             ? COMPACT_LEAVES_VC_TABLE_SIZE
             : COMPACT_LEAVES_VC_POLY_SIZE;
}

// Number of coefficients in the base block (intercept + per-tile + optional
// duplicate + vowel/consonant cells).
static size_t compact_leaves_base_coef_count(const CompactLeaves *cl) {
  size_t count =
      1 + (size_t)cl->dist_size + (size_t)compact_leaves_vc_count(cl);
  if (cl->flags & COMPACT_LEAVES_FLAG_HAS_DUP) {
    count += (size_t)cl->dist_size;
  }
  return count;
}

// Serialized byte length of cl.
static size_t compact_leaves_serialized_size(const CompactLeaves *cl) {
  if (cl->flags & COMPACT_LEAVES_FLAG_BITPACKED) {
    const int tile_bits = compact_leaves_tile_bits(cl);
    const int nt_bits = compact_leaves_num_tiles_bits();
    size_t body_bits =
        compact_leaves_base_coef_count(cl) * (size_t)cl->coef_bits;
    for (uint32_t syn_idx = 0; syn_idx < cl->num_synergies; syn_idx++) {
      body_bits +=
          (size_t)nt_bits +
          (size_t)cl->synergies[syn_idx].num_tiles * (size_t)tile_bits +
          (size_t)cl->coef_bits;
    }
    // header + vowel_bits(8) + coef_bits(1) + ceil(body_bits / 8)
    return COMPACT_LEAVES_HEADER_BYTES + 8 + 1 + (body_bits + 7) / 8;
  }
  size_t size = COMPACT_LEAVES_HEADER_BYTES;
  size += 8;                         // vowel_bits
  size += 2;                         // base_ticks
  size += (size_t)cl->dist_size * 2; // tile_ticks
  if (cl->flags & COMPACT_LEAVES_FLAG_HAS_DUP) {
    size += (size_t)cl->dist_size * 2; // dup_ticks
  }
  size += (size_t)compact_leaves_vc_count(cl) * 2; // vc_ticks
  for (uint32_t syn_idx = 0; syn_idx < cl->num_synergies; syn_idx++) {
    size += 1 + cl->synergies[syn_idx].num_tiles + 2; // k + tiles + coef
  }
  return size;
}

void compact_leaves_destroy(CompactLeaves *cl) {
  if (cl == NULL) {
    return;
  }
  free(cl->tile_ticks);
  free(cl->dup_ticks);
  free(cl->synergies);
  free(cl);
}

void compact_leaves_write_to_file(const CompactLeaves *cl, const char *filename,
                                  ErrorStack *error_stack) {
  const size_t num_bytes = compact_leaves_serialized_size(cl);
  uint8_t *bytes = (uint8_t *)calloc_or_die(num_bytes, 1);
  size_t pos = 0;
  for (int magic_idx = 0; magic_idx < 4; magic_idx++) {
    bytes[pos++] = (uint8_t)COMPACT_LEAVES_MAGIC[magic_idx];
  }
  bytes[pos++] = COMPACT_LEAVES_VERSION;
  bytes[pos++] = cl->radix_code;
  bytes[pos++] = cl->dist_size;
  bytes[pos++] = cl->flags;
  put_u32(bytes, &pos, cl->num_synergies);
  put_u64(bytes, &pos, cl->vowel_bits);
  const int vc_count = compact_leaves_vc_count(cl);
  if (cl->flags & COMPACT_LEAVES_FLAG_BITPACKED) {
    bytes[pos++] = cl->coef_bits;
    const int cb = cl->coef_bits;
    const int tile_bits = compact_leaves_tile_bits(cl);
    const int nt_bits = compact_leaves_num_tiles_bits();
    size_t bit_pos = pos * 8; // body begins byte-aligned, packed LSB-first
    put_bits(bytes, &bit_pos, compact_leaves_zigzag(cl->base_ticks), cb);
    for (uint8_t ml = 0; ml < cl->dist_size; ml++) {
      put_bits(bytes, &bit_pos, compact_leaves_zigzag(cl->tile_ticks[ml]), cb);
    }
    if (cl->flags & COMPACT_LEAVES_FLAG_HAS_DUP) {
      for (uint8_t ml = 0; ml < cl->dist_size; ml++) {
        put_bits(bytes, &bit_pos, compact_leaves_zigzag(cl->dup_ticks[ml]), cb);
      }
    }
    for (int vc_idx = 0; vc_idx < vc_count; vc_idx++) {
      put_bits(bytes, &bit_pos, compact_leaves_zigzag(cl->vc_ticks[vc_idx]),
               cb);
    }
    for (uint32_t syn_idx = 0; syn_idx < cl->num_synergies; syn_idx++) {
      const CompactLeavesSynergy *syn = &cl->synergies[syn_idx];
      put_bits(bytes, &bit_pos, syn->num_tiles, nt_bits);
      for (uint8_t tile_idx = 0; tile_idx < syn->num_tiles; tile_idx++) {
        put_bits(bytes, &bit_pos, syn->tiles[tile_idx], tile_bits);
      }
      put_bits(bytes, &bit_pos, compact_leaves_zigzag(syn->value_ticks), cb);
    }
  } else {
    put_u16(bytes, &pos, (uint16_t)cl->base_ticks);
    for (uint8_t ml = 0; ml < cl->dist_size; ml++) {
      put_u16(bytes, &pos, (uint16_t)cl->tile_ticks[ml]);
    }
    if (cl->flags & COMPACT_LEAVES_FLAG_HAS_DUP) {
      for (uint8_t ml = 0; ml < cl->dist_size; ml++) {
        put_u16(bytes, &pos, (uint16_t)cl->dup_ticks[ml]);
      }
    }
    for (int vc_idx = 0; vc_idx < vc_count; vc_idx++) {
      put_u16(bytes, &pos, (uint16_t)cl->vc_ticks[vc_idx]);
    }
    for (uint32_t syn_idx = 0; syn_idx < cl->num_synergies; syn_idx++) {
      const CompactLeavesSynergy *syn = &cl->synergies[syn_idx];
      bytes[pos++] = syn->num_tiles;
      for (uint8_t tile_idx = 0; tile_idx < syn->num_tiles; tile_idx++) {
        bytes[pos++] = syn->tiles[tile_idx];
      }
      put_u16(bytes, &pos, (uint16_t)syn->value_ticks);
    }
  }

  FILE *stream = fopen_safe(filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(bytes);
    return;
  }
  fwrite_or_die(bytes, 1, num_bytes, stream, "compact leaves");
  fclose_or_die(stream);
  free(bytes);
}

CompactLeaves *compact_leaves_read_from_file(const char *filename,
                                             ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  fseek(stream, 0, SEEK_END);
  const long file_len = ftell(stream);
  fseek(stream, 0, SEEK_SET);
  if (file_len < COMPACT_LEAVES_HEADER_BYTES + 10) {
    fclose_or_die(stream);
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
        get_formatted_string("compact leaves file too small: %s", filename));
    return NULL;
  }
  uint8_t *bytes = (uint8_t *)malloc_or_die((size_t)file_len);
  if (fread(bytes, 1, (size_t)file_len, stream) != (size_t)file_len) {
    fclose_or_die(stream);
    free(bytes);
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
        get_formatted_string("truncated compact leaves file: %s", filename));
    return NULL;
  }
  fclose_or_die(stream);

  if (memcmp(bytes, COMPACT_LEAVES_MAGIC, 4) != 0 ||
      bytes[4] != COMPACT_LEAVES_VERSION) {
    free(bytes);
    error_stack_push(error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
                     get_formatted_string(
                         "bad compact leaves header in file: %s", filename));
    return NULL;
  }
  CompactLeaves *cl = (CompactLeaves *)calloc_or_die(1, sizeof(CompactLeaves));
  size_t pos = 4;
  pos++; // version (validated)
  cl->radix_code = bytes[pos++];
  cl->radix_millipoints = compact_leaves_radix_millipoints(cl->radix_code);
  cl->dist_size = bytes[pos++];
  cl->flags = bytes[pos++];
  cl->num_synergies = get_u32(bytes, &pos);
  // Validate sizes before allocating, then bound every read against file_len.
  if (cl->dist_size == 0 || cl->dist_size > MAX_ALPHABET_SIZE) {
    free(bytes);
    free(cl);
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
        get_formatted_string("invalid compact leaves dist_size in file: %s",
                             filename));
    return NULL;
  }
  cl->vowel_bits = get_u64(bytes, &pos);
  const int vc_count = compact_leaves_vc_count(cl);
  if (cl->flags & COMPACT_LEAVES_FLAG_BITPACKED) {
    // --- bit-packed body: coef_bits byte, then a LSB-first bit stream ---
    if (pos + 1 > (size_t)file_len) {
      compact_leaves_destroy(cl);
      free(bytes);
      error_stack_push(error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
                       get_formatted_string(
                           "truncated compact leaves header: %s", filename));
      return NULL;
    }
    cl->coef_bits = bytes[pos++];
    if (cl->coef_bits < 1 || cl->coef_bits > 16) {
      compact_leaves_destroy(cl);
      free(bytes);
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
          get_formatted_string("invalid compact leaves coef_bits in file: %s",
                               filename));
      return NULL;
    }
    const int cb = cl->coef_bits;
    const int tile_bits = compact_leaves_tile_bits(cl);
    const int nt_bits = compact_leaves_num_tiles_bits();
    const size_t total_bits = (size_t)file_len * 8;
    size_t bit_pos = pos * 8;
    const size_t base_bits = compact_leaves_base_coef_count(cl) * (size_t)cb;
    if (bit_pos + base_bits > total_bits) {
      compact_leaves_destroy(cl);
      free(bytes);
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
          get_formatted_string("truncated compact leaves body: %s", filename));
      return NULL;
    }
    cl->base_ticks =
        (int16_t)compact_leaves_unzigzag(get_bits(bytes, &bit_pos, cb));
    cl->tile_ticks = (int16_t *)malloc_or_die(sizeof(int16_t) * cl->dist_size);
    for (uint8_t ml = 0; ml < cl->dist_size; ml++) {
      cl->tile_ticks[ml] =
          (int16_t)compact_leaves_unzigzag(get_bits(bytes, &bit_pos, cb));
    }
    if (cl->flags & COMPACT_LEAVES_FLAG_HAS_DUP) {
      cl->dup_ticks = (int16_t *)malloc_or_die(sizeof(int16_t) * cl->dist_size);
      for (uint8_t ml = 0; ml < cl->dist_size; ml++) {
        cl->dup_ticks[ml] =
            (int16_t)compact_leaves_unzigzag(get_bits(bytes, &bit_pos, cb));
      }
    }
    for (int vc_idx = 0; vc_idx < vc_count; vc_idx++) {
      cl->vc_ticks[vc_idx] =
          (int16_t)compact_leaves_unzigzag(get_bits(bytes, &bit_pos, cb));
    }
    // Reject a num_synergies that cannot possibly fit the remaining bits, so a
    // crafted file cannot trigger a huge allocation (calloc_or_die aborts).
    // Each synergy needs at least nt_bits + one tile + cb bits.
    const size_t min_syn_bits =
        (size_t)nt_bits + (size_t)tile_bits + (size_t)cb;
    if ((size_t)cl->num_synergies > (total_bits - bit_pos) / min_syn_bits) {
      compact_leaves_destroy(cl);
      free(bytes);
      error_stack_push(
          error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
          get_formatted_string("invalid compact leaves num_synergies: %s",
                               filename));
      return NULL;
    }
    if (cl->num_synergies > 0) {
      cl->synergies = (CompactLeavesSynergy *)calloc_or_die(
          cl->num_synergies, sizeof(CompactLeavesSynergy));
      for (uint32_t syn_idx = 0; syn_idx < cl->num_synergies; syn_idx++) {
        if (bit_pos + (size_t)nt_bits > total_bits) {
          compact_leaves_destroy(cl);
          free(bytes);
          error_stack_push(
              error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
              get_formatted_string("truncated compact leaves synergies: %s",
                                   filename));
          return NULL;
        }
        CompactLeavesSynergy *syn = &cl->synergies[syn_idx];
        syn->num_tiles = (uint8_t)get_bits(bytes, &bit_pos, nt_bits);
        if (syn->num_tiles == 0 ||
            syn->num_tiles > COMPACT_LEAVES_MAX_SYNERGY_TILES ||
            bit_pos + (size_t)syn->num_tiles * (size_t)tile_bits + (size_t)cb >
                total_bits) {
          compact_leaves_destroy(cl);
          free(bytes);
          error_stack_push(
              error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
              get_formatted_string("invalid compact leaves synergy in file: %s",
                                   filename));
          return NULL;
        }
        for (uint8_t tile_idx = 0; tile_idx < syn->num_tiles; tile_idx++) {
          const uint32_t ml = get_bits(bytes, &bit_pos, tile_bits);
          if (ml >= cl->dist_size) {
            compact_leaves_destroy(cl);
            free(bytes);
            error_stack_push(
                error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
                get_formatted_string("invalid compact leaves synergy tile: %s",
                                     filename));
            return NULL;
          }
          syn->tiles[tile_idx] = (MachineLetter)ml;
        }
        syn->value_ticks =
            (int16_t)compact_leaves_unzigzag(get_bits(bytes, &bit_pos, cb));
      }
    }
    free(bytes);
    return cl;
  }
  // Bound the fixed base block before reading it.
  size_t base_block_bytes =
      2 + (size_t)cl->dist_size * 2 + (size_t)vc_count * 2;
  if (cl->flags & COMPACT_LEAVES_FLAG_HAS_DUP) {
    base_block_bytes += (size_t)cl->dist_size * 2;
  }
  if (pos + base_block_bytes > (size_t)file_len) {
    compact_leaves_destroy(cl);
    free(bytes);
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
        get_formatted_string("truncated compact leaves body: %s", filename));
    return NULL;
  }
  cl->base_ticks = (int16_t)get_u16(bytes, &pos);
  cl->tile_ticks = (int16_t *)malloc_or_die(sizeof(int16_t) * cl->dist_size);
  for (uint8_t ml = 0; ml < cl->dist_size; ml++) {
    cl->tile_ticks[ml] = (int16_t)get_u16(bytes, &pos);
  }
  if (cl->flags & COMPACT_LEAVES_FLAG_HAS_DUP) {
    cl->dup_ticks = (int16_t *)malloc_or_die(sizeof(int16_t) * cl->dist_size);
    for (uint8_t ml = 0; ml < cl->dist_size; ml++) {
      cl->dup_ticks[ml] = (int16_t)get_u16(bytes, &pos);
    }
  }
  for (int vc_idx = 0; vc_idx < vc_count; vc_idx++) {
    cl->vc_ticks[vc_idx] = (int16_t)get_u16(bytes, &pos);
  }
  // Reject a num_synergies that cannot possibly fit (each synergy is >= 1 byte
  // num_tiles + 1 tile + 2 value), so a crafted file cannot force a huge
  // calloc.
  if ((size_t)cl->num_synergies > ((size_t)file_len - pos) / 4) {
    compact_leaves_destroy(cl);
    free(bytes);
    error_stack_push(error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
                     get_formatted_string(
                         "invalid compact leaves num_synergies: %s", filename));
    return NULL;
  }
  // Bound-check the synergy section before trusting num_synergies.
  if (cl->num_synergies > 0) {
    cl->synergies = (CompactLeavesSynergy *)calloc_or_die(
        cl->num_synergies, sizeof(CompactLeavesSynergy));
    for (uint32_t syn_idx = 0; syn_idx < cl->num_synergies; syn_idx++) {
      if (pos + 1 > (size_t)file_len) {
        compact_leaves_destroy(cl);
        free(bytes);
        error_stack_push(
            error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
            get_formatted_string("truncated compact leaves synergies: %s",
                                 filename));
        return NULL;
      }
      CompactLeavesSynergy *syn = &cl->synergies[syn_idx];
      syn->num_tiles = bytes[pos++];
      if (syn->num_tiles == 0 ||
          syn->num_tiles > COMPACT_LEAVES_MAX_SYNERGY_TILES ||
          pos + syn->num_tiles + 2 > (size_t)file_len) {
        compact_leaves_destroy(cl);
        free(bytes);
        error_stack_push(
            error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
            get_formatted_string("invalid compact leaves synergy in file: %s",
                                 filename));
        return NULL;
      }
      for (uint8_t tile_idx = 0; tile_idx < syn->num_tiles; tile_idx++) {
        syn->tiles[tile_idx] = bytes[pos++];
      }
      syn->value_ticks = (int16_t)get_u16(bytes, &pos);
    }
  }
  free(bytes);
  return cl;
}

Equity compact_leaves_get_leave_value(const CompactLeaves *cl,
                                      const Rack *leave) {
  if (leave == NULL || rack_get_total_letters(leave) == 0) {
    return 0;
  }
  int64_t ticks = cl->base_ticks;
  int num_vowels = 0;
  int num_consonants = 0;
  for (uint8_t ml = 0; ml < cl->dist_size; ml++) {
    const int count = rack_get_letter(leave, ml);
    if (count == 0) {
      continue;
    }
    ticks += (int64_t)cl->tile_ticks[ml] * count;
    if (cl->dup_ticks != NULL && count > 1) {
      ticks += (int64_t)cl->dup_ticks[ml] * (count - 1);
    }
    if (ml != 0) { // blank (ml 0) is neither vowel nor consonant
      if ((cl->vowel_bits >> ml) & 1U) {
        num_vowels += count;
      } else {
        num_consonants += count;
      }
    }
  }
  if (cl->flags & COMPACT_LEAVES_FLAG_VC_TABLE) {
    const int vowel_idx = num_vowels > COMPACT_LEAVES_VC_CLAMP
                              ? COMPACT_LEAVES_VC_CLAMP
                              : num_vowels;
    const int cons_idx = num_consonants > COMPACT_LEAVES_VC_CLAMP
                             ? COMPACT_LEAVES_VC_CLAMP
                             : num_consonants;
    ticks += cl->vc_ticks[vowel_idx * COMPACT_LEAVES_VC_DIM + cons_idx];
  } else {
    const int imbalance = num_vowels - num_consonants;
    ticks += (int64_t)cl->vc_ticks[0] * imbalance +
             (int64_t)cl->vc_ticks[1] * imbalance * imbalance;
  }
  for (uint32_t syn_idx = 0; syn_idx < cl->num_synergies; syn_idx++) {
    const CompactLeavesSynergy *syn = &cl->synergies[syn_idx];
    int occurrences = INT_MAX;
    uint8_t tile_idx = 0;
    while (tile_idx < syn->num_tiles) {
      const MachineLetter ml = syn->tiles[tile_idx];
      int needed = 1;
      tile_idx++;
      while (tile_idx < syn->num_tiles && syn->tiles[tile_idx] == ml) {
        needed++;
        tile_idx++;
      }
      const int can = rack_get_letter(leave, ml) / needed;
      if (can < occurrences) {
        occurrences = can;
      }
    }
    // Indicator: the combo's value is added once if it is present in the leave
    // (matches how the fitter scores synergy terms).
    if (occurrences > 0) {
      ticks += syn->value_ticks;
    }
  }
  int64_t millipoints = ticks * cl->radix_millipoints;
  if (millipoints > EQUITY_MAX_VALUE) {
    millipoints = EQUITY_MAX_VALUE;
  } else if (millipoints < EQUITY_MIN_VALUE) {
    millipoints = EQUITY_MIN_VALUE;
  }
  return (Equity)millipoints;
}
