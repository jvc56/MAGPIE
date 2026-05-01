// Flatten an in-memory WMP into a single contiguous buffer for GPU upload.
// See wmpg_maker.h for format spec (v2: word + blank-1 + blank-2).

#include "wmpg_maker.h"

#include "../def/board_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WMPG_MAGIC_0 'W'
#define WMPG_MAGIC_1 'M'
#define WMPG_MAGIC_2 'P'
#define WMPG_MAGIC_3 'G'
#define WMPG_VERSION 2
#define WMPG_BITRACK_BYTES 16
#define WMPG_HEADER_BYTES 32
#define WMPG_META_BYTES_PER_LENGTH 56
#define WMPG_ENTRY_BYTES 32

static void put_u32_le(uint8_t *dst, uint32_t v) {
  dst[0] = (uint8_t)(v & 0xff);
  dst[1] = (uint8_t)((v >> 8) & 0xff);
  dst[2] = (uint8_t)((v >> 16) & 0xff);
  dst[3] = (uint8_t)((v >> 24) & 0xff);
}

void wmpg_build(const WMP *wmp, uint8_t **out_bytes, size_t *out_size,
                ErrorStack *error_stack) {
  *out_bytes = NULL;
  *out_size = 0;
  if (wmp == NULL) {
    error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                     string_duplicate("wmpg_build: NULL WMP"));
    return;
  }
  const int max_len_plus_one = BOARD_DIM + 1;

  // Pass 1: tally section sizes across all 3 tables.
  uint32_t total_bucket_starts = 0;
  uint32_t total_entries = 0;
  uint32_t total_uninlined_letter_bytes = 0;
  for (int len = 0; len < max_len_plus_one; len++) {
    const WMPForLength *wfl = &wmp->wfls[len];
    if (wfl->num_word_entries > 0) {
      total_bucket_starts += wfl->num_word_buckets + 1;
      total_entries += wfl->num_word_entries;
      total_uninlined_letter_bytes += wfl->num_uninlined_words * (uint32_t)len;
    }
    if (wfl->num_blank_entries > 0) {
      total_bucket_starts += wfl->num_blank_buckets + 1;
      total_entries += wfl->num_blank_entries;
    }
    if (wfl->num_double_blank_entries > 0) {
      total_bucket_starts += wfl->num_double_blank_buckets + 1;
      total_entries += wfl->num_double_blank_entries;
    }
  }

  const size_t meta_bytes =
      (size_t)max_len_plus_one * WMPG_META_BYTES_PER_LENGTH;
  const size_t buckets_bytes = (size_t)total_bucket_starts * sizeof(uint32_t);
  const size_t entries_bytes = (size_t)total_entries * WMPG_ENTRY_BYTES;
  const size_t letters_bytes = (size_t)total_uninlined_letter_bytes;
  const size_t total_bytes = WMPG_HEADER_BYTES + meta_bytes + buckets_bytes +
                             entries_bytes + letters_bytes;

  uint8_t *buf = (uint8_t *)malloc_or_die(total_bytes);
  memset(buf, 0, total_bytes);

  // Header
  buf[0] = WMPG_MAGIC_0;
  buf[1] = WMPG_MAGIC_1;
  buf[2] = WMPG_MAGIC_2;
  buf[3] = WMPG_MAGIC_3;
  buf[4] = WMPG_VERSION;
  buf[5] = 0;
  buf[6] = (uint8_t)max_len_plus_one;
  buf[7] = WMPG_BITRACK_BYTES;
  put_u32_le(buf + 8, total_bucket_starts);
  put_u32_le(buf + 12, total_entries);
  put_u32_le(buf + 16, total_uninlined_letter_bytes);
  put_u32_le(buf + 20,
             (uint32_t)(WMPG_HEADER_BYTES + meta_bytes)); // sections_offset
  put_u32_le(buf + 24, (uint32_t)WMPG_META_BYTES_PER_LENGTH);

  const size_t buckets_section_offset = WMPG_HEADER_BYTES + meta_bytes;
  const size_t entries_section_offset = buckets_section_offset + buckets_bytes;
  const size_t letters_section_offset = entries_section_offset + entries_bytes;

  uint8_t *meta = buf + WMPG_HEADER_BYTES;
  size_t bucket_off = 0;
  size_t entry_off = 0;
  size_t letter_off = 0;
  for (int len = 0; len < max_len_plus_one; len++) {
    const WMPForLength *wfl = &wmp->wfls[len];
    uint8_t *m = meta + (size_t)len * WMPG_META_BYTES_PER_LENGTH;

    // Word (blank-0) sub-block.
    {
      const uint32_t nb =
          wfl->num_word_entries == 0 ? 0 : wfl->num_word_buckets;
      const uint32_t ne = wfl->num_word_entries;
      const uint32_t nu = wfl->num_uninlined_words;
      put_u32_le(m + 0, nb);
      put_u32_le(m + 4, (uint32_t)bucket_off);
      put_u32_le(m + 8, ne);
      put_u32_le(m + 12, (uint32_t)entry_off);
      put_u32_le(m + 16, nu);
      put_u32_le(m + 20, (uint32_t)letter_off);
      if (ne > 0) {
        const size_t n_starts = (size_t)nb + 1;
        memcpy(buf + buckets_section_offset + bucket_off,
               wfl->word_bucket_starts, n_starts * sizeof(uint32_t));
        bucket_off += n_starts * sizeof(uint32_t);
        memcpy(buf + entries_section_offset + entry_off, wfl->word_map_entries,
               (size_t)ne * WMPG_ENTRY_BYTES);
        entry_off += (size_t)ne * WMPG_ENTRY_BYTES;
        const size_t lbytes = (size_t)nu * (size_t)len;
        if (lbytes > 0) {
          memcpy(buf + letters_section_offset + letter_off, wfl->word_letters,
                 lbytes);
          letter_off += lbytes;
        }
      }
    }

    // Blank-1 (single-blank) sub-block.
    {
      const uint32_t nb =
          wfl->num_blank_entries == 0 ? 0 : wfl->num_blank_buckets;
      const uint32_t ne = wfl->num_blank_entries;
      put_u32_le(m + 24, nb);
      put_u32_le(m + 28, (uint32_t)bucket_off);
      put_u32_le(m + 32, ne);
      put_u32_le(m + 36, (uint32_t)entry_off);
      if (ne > 0) {
        const size_t n_starts = (size_t)nb + 1;
        memcpy(buf + buckets_section_offset + bucket_off,
               wfl->blank_bucket_starts, n_starts * sizeof(uint32_t));
        bucket_off += n_starts * sizeof(uint32_t);
        memcpy(buf + entries_section_offset + entry_off, wfl->blank_map_entries,
               (size_t)ne * WMPG_ENTRY_BYTES);
        entry_off += (size_t)ne * WMPG_ENTRY_BYTES;
      }
    }

    // Blank-2 (double-blank) sub-block.
    {
      const uint32_t nb = wfl->num_double_blank_entries == 0
                              ? 0
                              : wfl->num_double_blank_buckets;
      const uint32_t ne = wfl->num_double_blank_entries;
      put_u32_le(m + 40, nb);
      put_u32_le(m + 44, (uint32_t)bucket_off);
      put_u32_le(m + 48, ne);
      put_u32_le(m + 52, (uint32_t)entry_off);
      if (ne > 0) {
        const size_t n_starts = (size_t)nb + 1;
        memcpy(buf + buckets_section_offset + bucket_off,
               wfl->double_blank_bucket_starts, n_starts * sizeof(uint32_t));
        bucket_off += n_starts * sizeof(uint32_t);
        memcpy(buf + entries_section_offset + entry_off,
               wfl->double_blank_map_entries, (size_t)ne * WMPG_ENTRY_BYTES);
        entry_off += (size_t)ne * WMPG_ENTRY_BYTES;
      }
    }
  }

  *out_bytes = buf;
  *out_size = total_bytes;
}

void wmpg_make(const WMP *wmp, const char *output_path,
               ErrorStack *error_stack) {
  uint8_t *buf = NULL;
  size_t total = 0;
  wmpg_build(wmp, &buf, &total, error_stack);
  if (buf == NULL) {
    return;
  }
  FILE *fp = fopen(output_path, "wb");
  if (fp == NULL) {
    free(buf);
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     string_duplicate(output_path));
    return;
  }
  const size_t written = fwrite(buf, 1, total, fp);
  fclose(fp);
  free(buf);
  if (written != total) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     string_duplicate(output_path));
  }
}
