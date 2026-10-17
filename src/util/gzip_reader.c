#include "gzip_reader.h"

#include "io_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// A streaming DEFLATE decoder after Mark Adler's puff (zlib's contrib/puff):
// canonical Huffman decoding a bit at a time, into a ring buffer of the last
// 32 KB, the furthest back a DEFLATE copy can reach. It decodes MAGPIE's data
// tarballs (about 270 MB) in a few seconds, which is all it is for.

enum {
  GZIP_WINDOW_BYTES = 32768,
  GZIP_INPUT_BUFFER_BYTES = 65536,
  GZIP_MAX_CODE_BITS = 15,
  GZIP_MAX_LITERAL_CODES = 288,
  GZIP_MAX_DISTANCE_CODES = 30,
  GZIP_FIXED_LITERAL_CODES = 288,
  GZIP_FIXED_DISTANCE_CODES = 30,
  GZIP_CODE_LENGTH_CODES = 19,
  GZIP_END_OF_BLOCK = 256,
  GZIP_ID1 = 31,
  GZIP_ID2 = 139,
  GZIP_METHOD_DEFLATE = 8,
  GZIP_FLAG_HEADER_CRC = 2,
  GZIP_FLAG_EXTRA = 4,
  GZIP_FLAG_NAME = 8,
  GZIP_FLAG_COMMENT = 16,
  GZIP_FLAG_RESERVED = 224,
  // ID1, ID2, CM, FLG, MTIME (4), XFL, OS.
  GZIP_HEADER_BYTES = 10,
};

typedef struct GzipHuffman {
  // How many codes have each length, and the symbols in canonical order.
  short count[GZIP_MAX_CODE_BITS + 1];
  short symbol[GZIP_MAX_LITERAL_CODES];
} GzipHuffman;

typedef struct GzipReader {
  FILE *input;
  unsigned char buffer[GZIP_INPUT_BUFFER_BYTES];
  size_t buffer_length;
  size_t buffer_position;
  bool at_end;
  // Bits read but not yet used, least significant first.
  uint32_t bits;
  unsigned bit_count;
  bool valid;
  gzip_reader_sink_t sink;
  void *context;
  unsigned char window[GZIP_WINDOW_BYTES];
  size_t window_position;
  // Of this member's output: its CRC-32 and length (mod 2^32).
  uint32_t crc;
  uint32_t length;
  uint32_t crc_table[256];
} GzipReader;

static void gzip_crc_table(uint32_t *table) {
  for (uint32_t index = 0; index < 256; index++) {
    uint32_t remainder = index;
    for (int bit = 0; bit < 8; bit++) {
      remainder = (remainder >> 1) ^ ((0U - (remainder & 1U)) & 0xedb88320U);
    }
    table[index] = remainder;
  }
}

// The next input byte, or -1 at the end of the input.
static int gzip_next_byte(GzipReader *reader) {
  if (reader->buffer_position == reader->buffer_length) {
    if (reader->at_end) {
      return -1;
    }
    reader->buffer_length =
        fread(reader->buffer, 1, sizeof(reader->buffer), reader->input);
    reader->buffer_position = 0;
    if (reader->buffer_length < sizeof(reader->buffer)) {
      reader->at_end = true;
      if (ferror(reader->input)) {
        reader->valid = false;
        return -1;
      }
    }
    if (reader->buffer_length == 0) {
      return -1;
    }
  }
  return reader->buffer[reader->buffer_position++];
}

static unsigned gzip_bits(GzipReader *reader, unsigned needed) {
  while (reader->bit_count < needed) {
    const int byte = gzip_next_byte(reader);
    if (byte < 0) {
      reader->valid = false;
      return 0;
    }
    reader->bits |= (uint32_t)byte << reader->bit_count;
    reader->bit_count += 8;
  }
  const unsigned value = reader->bits & ((1U << needed) - 1U);
  reader->bits >>= needed;
  reader->bit_count -= needed;
  return value;
}

// A whole byte, after discarding any bits left of the current one.
static int gzip_aligned_byte(GzipReader *reader) {
  reader->bits >>= reader->bit_count % 8;
  reader->bit_count -= reader->bit_count % 8;
  if (reader->bit_count >= 8) {
    const int byte = (int)(reader->bits & 255U);
    reader->bits >>= 8;
    reader->bit_count -= 8;
    return byte;
  }
  return gzip_next_byte(reader);
}

static void gzip_flush(GzipReader *reader) {
  if (reader->valid && reader->window_position > 0 &&
      !reader->sink(reader->context, reader->window, reader->window_position)) {
    reader->valid = false;
  }
  reader->window_position = 0;
}

static void gzip_emit(GzipReader *reader, unsigned char byte) {
  reader->crc =
      (reader->crc >> 8) ^ reader->crc_table[(reader->crc ^ byte) & 255U];
  reader->length++;
  reader->window[reader->window_position++] = byte;
  if (reader->window_position == GZIP_WINDOW_BYTES) {
    gzip_flush(reader);
  }
}

// Copies `length` bytes from `distance` back. The window holds the last
// GZIP_WINDOW_BYTES bytes whether or not they have been flushed.
static void gzip_copy(GzipReader *reader, unsigned distance, unsigned length) {
  for (unsigned copied = 0; copied < length; copied++) {
    const size_t from =
        (reader->window_position + GZIP_WINDOW_BYTES - distance) %
        GZIP_WINDOW_BYTES;
    gzip_emit(reader, reader->window[from]);
  }
}

// Builds the decoding tables for `count` code lengths. Returns false if they
// describe an over-subscribed code.
static bool gzip_build(GzipHuffman *huffman, const short *lengths, int count) {
  for (int length = 0; length <= GZIP_MAX_CODE_BITS; length++) {
    huffman->count[length] = 0;
  }
  for (int symbol = 0; symbol < count; symbol++) {
    huffman->count[lengths[symbol]]++;
  }
  int left = 1;
  for (int length = 1; length <= GZIP_MAX_CODE_BITS; length++) {
    left <<= 1;
    left -= huffman->count[length];
    if (left < 0) {
      return false;
    }
  }
  short offsets[GZIP_MAX_CODE_BITS + 1];
  offsets[1] = 0;
  for (int length = 1; length < GZIP_MAX_CODE_BITS; length++) {
    offsets[length + 1] = (short)(offsets[length] + huffman->count[length]);
  }
  for (int symbol = 0; symbol < count; symbol++) {
    if (lengths[symbol] != 0) {
      huffman->symbol[offsets[lengths[symbol]]++] = (short)symbol;
    }
  }
  return true;
}

static int gzip_decode(GzipReader *reader, const GzipHuffman *huffman) {
  int code = 0;
  int first = 0;
  int index = 0;
  for (int length = 1; length <= GZIP_MAX_CODE_BITS; length++) {
    code |= (int)gzip_bits(reader, 1);
    const int count = huffman->count[length];
    if (code - count < first) {
      return huffman->symbol[index + (code - first)];
    }
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  reader->valid = false;
  return -1;
}

static bool gzip_codes(GzipReader *reader, const GzipHuffman *literals,
                       const GzipHuffman *distances) {
  static const short length_base[29] = {
      3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const short length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                         1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                         4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const unsigned short distance_base[30] = {
      1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
      33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const short distance_extra[30] = {
      0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  while (reader->valid) {
    int symbol = gzip_decode(reader, literals);
    if (symbol < 0) {
      return false;
    }
    if (symbol < GZIP_END_OF_BLOCK) {
      gzip_emit(reader, (unsigned char)symbol);
      continue;
    }
    if (symbol == GZIP_END_OF_BLOCK) {
      return true;
    }
    symbol -= GZIP_END_OF_BLOCK + 1;
    if (symbol >= 29) {
      return false;
    }
    const unsigned length = (unsigned)length_base[symbol] +
                            gzip_bits(reader, (unsigned)length_extra[symbol]);
    const int distance_symbol = gzip_decode(reader, distances);
    if (distance_symbol < 0 || distance_symbol >= GZIP_MAX_DISTANCE_CODES) {
      return false;
    }
    const unsigned distance =
        distance_base[distance_symbol] +
        gzip_bits(reader, (unsigned)distance_extra[distance_symbol]);
    if (!reader->valid || distance > GZIP_WINDOW_BYTES) {
      return false;
    }
    gzip_copy(reader, distance, length);
  }
  return false;
}

static bool gzip_stored(GzipReader *reader) {
  const int low = gzip_aligned_byte(reader);
  const int high = gzip_aligned_byte(reader);
  const int inverse_low = gzip_aligned_byte(reader);
  const int inverse_high = gzip_aligned_byte(reader);
  if (low < 0 || high < 0 || inverse_low < 0 || inverse_high < 0) {
    return false;
  }
  const unsigned length = (unsigned)low | ((unsigned)high << 8);
  const unsigned inverse =
      (unsigned)inverse_low | ((unsigned)inverse_high << 8);
  if (length != (~inverse & 0xffffU)) {
    return false;
  }
  for (unsigned copied = 0; copied < length; copied++) {
    const int byte = gzip_aligned_byte(reader);
    if (byte < 0) {
      return false;
    }
    gzip_emit(reader, (unsigned char)byte);
  }
  return reader->valid;
}

// The fixed codes RFC 1951 section 3.2.6 defines.
static bool gzip_fixed(GzipReader *reader) {
  GzipHuffman literals;
  GzipHuffman distances;
  short lengths[GZIP_FIXED_LITERAL_CODES];
  for (int symbol = 0; symbol < GZIP_FIXED_LITERAL_CODES; symbol++) {
    if (symbol >= 144 && symbol < 256) {
      lengths[symbol] = 9;
    } else if (symbol >= 256 && symbol < 280) {
      lengths[symbol] = 7;
    } else {
      lengths[symbol] = 8;
    }
  }
  (void)gzip_build(&literals, lengths, GZIP_FIXED_LITERAL_CODES);
  for (int symbol = 0; symbol < GZIP_FIXED_DISTANCE_CODES; symbol++) {
    lengths[symbol] = 5;
  }
  (void)gzip_build(&distances, lengths, GZIP_FIXED_DISTANCE_CODES);
  return gzip_codes(reader, &literals, &distances);
}

static bool gzip_dynamic(GzipReader *reader) {
  static const short order[GZIP_CODE_LENGTH_CODES] = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  const int literal_count = (int)gzip_bits(reader, 5) + 257;
  const int distance_count = (int)gzip_bits(reader, 5) + 1;
  const int code_length_count = (int)gzip_bits(reader, 4) + 4;
  if (!reader->valid || literal_count > GZIP_MAX_LITERAL_CODES ||
      distance_count > GZIP_MAX_DISTANCE_CODES) {
    return false;
  }
  short lengths[GZIP_MAX_LITERAL_CODES + GZIP_MAX_DISTANCE_CODES];
  for (int index = 0; index < GZIP_CODE_LENGTH_CODES; index++) {
    lengths[order[index]] = 0;
    if (index < code_length_count) {
      lengths[order[index]] = (short)gzip_bits(reader, 3);
    }
  }
  GzipHuffman code_lengths;
  if (!reader->valid ||
      !gzip_build(&code_lengths, lengths, GZIP_CODE_LENGTH_CODES)) {
    return false;
  }
  int index = 0;
  while (index < literal_count + distance_count) {
    int symbol = gzip_decode(reader, &code_lengths);
    if (symbol < 0) {
      return false;
    }
    if (symbol < 16) {
      lengths[index++] = (short)symbol;
      continue;
    }
    short repeated = 0;
    int repeat = 0;
    if (symbol == 16) {
      if (index == 0) {
        return false;
      }
      repeated = lengths[index - 1];
      repeat = 3 + (int)gzip_bits(reader, 2);
    } else if (symbol == 17) {
      repeat = 3 + (int)gzip_bits(reader, 3);
    } else {
      repeat = 11 + (int)gzip_bits(reader, 7);
    }
    if (!reader->valid || index + repeat > literal_count + distance_count) {
      return false;
    }
    while (repeat-- > 0) {
      lengths[index++] = repeated;
    }
  }
  if (lengths[GZIP_END_OF_BLOCK] == 0) {
    return false;
  }
  GzipHuffman literals;
  GzipHuffman distances;
  if (!gzip_build(&literals, lengths, literal_count) ||
      !gzip_build(&distances, lengths + literal_count, distance_count)) {
    return false;
  }
  return gzip_codes(reader, &literals, &distances);
}

// Reads a gzip member's header. Returns false at a clean end of the input
// (*more is false) or on a malformed header.
static bool gzip_header(GzipReader *reader, bool *more) {
  *more = false;
  const int id1 = gzip_aligned_byte(reader);
  if (id1 < 0) {
    return false;
  }
  *more = true;
  unsigned char header[GZIP_HEADER_BYTES];
  header[0] = (unsigned char)id1;
  for (int index = 1; index < GZIP_HEADER_BYTES; index++) {
    const int byte = gzip_aligned_byte(reader);
    if (byte < 0) {
      return false;
    }
    header[index] = (unsigned char)byte;
  }
  const unsigned flags = header[3];
  if (header[0] != GZIP_ID1 || header[1] != GZIP_ID2 ||
      header[2] != GZIP_METHOD_DEFLATE || (flags & GZIP_FLAG_RESERVED)) {
    return false;
  }
  if (flags & GZIP_FLAG_EXTRA) {
    const int low = gzip_aligned_byte(reader);
    const int high = gzip_aligned_byte(reader);
    if (low < 0 || high < 0) {
      return false;
    }
    for (int skipped = 0; skipped < (low | (high << 8)); skipped++) {
      if (gzip_aligned_byte(reader) < 0) {
        return false;
      }
    }
  }
  for (unsigned flag = GZIP_FLAG_NAME; flag <= GZIP_FLAG_COMMENT; flag <<= 1) {
    if (flags & flag) {
      int byte;
      do {
        byte = gzip_aligned_byte(reader);
      } while (byte > 0);
      if (byte < 0) {
        return false;
      }
    }
  }
  if (flags & GZIP_FLAG_HEADER_CRC) {
    // Two bytes of CRC-16, which the trailer's CRC-32 makes redundant.
    for (int byte_idx = 0; byte_idx < 2; byte_idx++) {
      if (gzip_aligned_byte(reader) < 0) {
        return false;
      }
    }
  }
  return true;
}

static bool gzip_trailer(GzipReader *reader) {
  uint32_t values[2] = {0, 0};
  for (int value_idx = 0; value_idx < 2; value_idx++) {
    for (int byte_idx = 0; byte_idx < 4; byte_idx++) {
      const int byte = gzip_aligned_byte(reader);
      if (byte < 0) {
        return false;
      }
      values[value_idx] |= (uint32_t)byte << (8 * byte_idx);
    }
  }
  return values[0] == (reader->crc ^ 0xffffffffU) &&
         values[1] == reader->length;
}

// Decodes one member's blocks after its header, then checks its trailer.
static bool gzip_member(GzipReader *reader) {
  reader->crc = 0xffffffffU;
  reader->length = 0;
  bool final = false;
  while (!final) {
    final = gzip_bits(reader, 1) != 0;
    const unsigned type = gzip_bits(reader, 2);
    bool decoded = false;
    if (reader->valid) {
      if (type == 0) {
        decoded = gzip_stored(reader);
      } else if (type == 1) {
        decoded = gzip_fixed(reader);
      } else if (type == 2) {
        decoded = gzip_dynamic(reader);
      }
    }
    if (!decoded || !reader->valid) {
      return false;
    }
  }
  gzip_flush(reader);
  return reader->valid && gzip_trailer(reader);
}

bool gzip_reader_decompress(FILE *input, gzip_reader_sink_t sink,
                            void *context) {
  // Too big for the stack: the input buffer and the window are 96 KB.
  GzipReader *reader = malloc_or_die(sizeof(GzipReader));
  *reader = (GzipReader){
      .input = input, .valid = true, .sink = sink, .context = context};
  gzip_crc_table(reader->crc_table);
  bool decompressed = false;
  while (true) {
    bool more = false;
    if (!gzip_header(reader, &more)) {
      // Concatenated members end where the input does.
      decompressed = decompressed && !more && reader->valid;
      break;
    }
    if (!gzip_member(reader)) {
      decompressed = false;
      break;
    }
    decompressed = true;
  }
  free(reader);
  return decompressed;
}
