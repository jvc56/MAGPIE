#include "xs_compress.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Bounded RFC 1951/1952 reader. The writer uses stored DEFLATE blocks: model
// construction needs portability and canonical JSON, not a compression library.
enum { XS_MAX_BYTES = 8 * 1024 * 1024, XS_DEFLATE_BITS = 15 };
typedef struct {
  const unsigned char *bytes;
  size_t size;
  size_t bit;
  bool valid;
} XSInflate;
typedef struct {
  unsigned count[16];
  unsigned symbol[288];
} XSHuffman;

uint32_t xs_crc32(const void *bytes, size_t length) {
  const unsigned char *input = bytes;
  uint32_t table[256];
  for (unsigned index = 0; index < 256; index++) {
    uint32_t remainder = index;
    for (unsigned bit = 0; bit < 8; bit++) {
      remainder =
          (remainder >> 1) ^ ((0U - (remainder & 1U)) & UINT32_C(0xedb88320));
    }
    table[index] = remainder;
  }
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0; index < length; index++) {
    crc = (crc >> 8) ^ table[(crc ^ input[index]) & 255];
  }
  return crc ^ UINT32_MAX;
}

static uint32_t xs_little32(const unsigned char *bytes) {
  return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
         (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void xs_store32(unsigned char *bytes, uint32_t value) {
  for (unsigned index = 0; index < 4; index++) {
    bytes[index] = (unsigned char)(value >> (index * 8));
  }
}

static unsigned xs_inflate_bits(XSInflate *input, unsigned width) {
  if (!input->valid || input->bit > input->size * 8 ||
      width > input->size * 8 - input->bit) {
    input->valid = false;
    return 0;
  }
  unsigned value = 0;
  for (unsigned index = 0; index < width; index++, input->bit++) {
    value |= ((input->bytes[input->bit / 8] >> (input->bit % 8)) & 1U) << index;
  }
  return value;
}

static bool xs_huffman_build(XSHuffman *tree, const unsigned *lengths,
                             unsigned count, bool code_lengths) {
  memset(tree, 0, sizeof(*tree));
  for (unsigned index = 0; index < count; index++) {
    if (lengths[index] > XS_DEFLATE_BITS) {
      return false;
    }
    tree->count[lengths[index]]++;
  }
  int remaining = 1;
  unsigned offsets[16] = {0};
  for (unsigned width = 1; width <= XS_DEFLATE_BITS; width++) {
    remaining = remaining * 2 - (int)tree->count[width];
    if (remaining < 0) {
      return false;
    }
    if (width < XS_DEFLATE_BITS) {
      offsets[width + 1] = offsets[width] + tree->count[width];
    }
  }
  // A one-symbol tree has a single one-bit code. A missing distance tree is
  // legal for literal-only blocks, and will fail if a match tries to use it.
  if (remaining && (code_lengths ||
                    (tree->count[0] != count &&
                     !(tree->count[0] == count - 1 && tree->count[1] == 1)))) {
    return false;
  }
  for (unsigned index = 0; index < count; index++) {
    if (lengths[index]) {
      tree->symbol[offsets[lengths[index]]++] = index;
    }
  }
  return true;
}

static unsigned xs_inflate_symbol(XSInflate *input, const XSHuffman *tree) {
  unsigned code = 0;
  unsigned first = 0;
  unsigned offset = 0;
  for (unsigned width = 1; input->valid && width <= XS_DEFLATE_BITS; width++) {
    code = (code << 1) | xs_inflate_bits(input, 1);
    if (code >= first && code - first < tree->count[width]) {
      return tree->symbol[offset + code - first];
    }
    offset += tree->count[width];
    first = (first + tree->count[width]) << 1;
  }
  input->valid = false;
  return 0;
}

static bool xs_inflate_trees(XSInflate *input, unsigned type,
                             XSHuffman *literal, XSHuffman *distance) {
  unsigned lengths[320] = {0};
  unsigned literals = 288;
  unsigned distances = 32;
  if (type == 1) {
    for (unsigned index = 0; index < 288; index++) {
      if (index < 144 || index >= 280) {
        lengths[index] = 8;
      } else {
        lengths[index] = index < 256 ? 9 : 7;
      }
    }
    for (unsigned index = 288; index < 320; index++) {
      lengths[index] = 5;
    }
  } else {
    literals = xs_inflate_bits(input, 5) + 257;
    distances = xs_inflate_bits(input, 5) + 1;
    unsigned codes = xs_inflate_bits(input, 4) + 4;
    static const unsigned order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                       11, 4,  12, 3, 13, 2, 14, 1, 15};
    unsigned code_lengths[19] = {0};
    for (unsigned index = 0; index < codes; index++) {
      code_lengths[order[index]] = xs_inflate_bits(input, 3);
    }
    XSHuffman code_tree;
    if (!input->valid || literals > 286 ||
        !xs_huffman_build(&code_tree, code_lengths, 19, true)) {
      return false;
    }
    unsigned used = 0;
    while (input->valid && used < literals + distances) {
      unsigned symbol = xs_inflate_symbol(input, &code_tree);
      unsigned repeat = 1;
      unsigned value = symbol;
      if (symbol == 16) {
        if (!used) {
          return false;
        }
        repeat = xs_inflate_bits(input, 2) + 3;
        value = lengths[used - 1];
      } else if (symbol == 17 || symbol == 18) {
        repeat = xs_inflate_bits(input, symbol == 17 ? 3 : 7) +
                 (symbol == 17 ? 3 : 11);
        value = 0;
      }
      if (repeat > literals + distances - used) {
        return false;
      }
      while (repeat--) {
        lengths[used++] = value;
      }
    }
  }
  return input->valid && lengths[256] &&
         xs_huffman_build(literal, lengths, literals, false) &&
         xs_huffman_build(distance, lengths + literals, distances, false);
}

static bool xs_inflate_block(XSInflate *input, unsigned type,
                             unsigned char *output, size_t *used) {
  if (type == 0) {
    input->bit = (input->bit + 7) & ~(size_t)7;
    unsigned length = xs_inflate_bits(input, 16);
    unsigned inverse = xs_inflate_bits(input, 16);
    if (!input->valid || (length ^ inverse) != 65535 ||
        length > XS_MAX_BYTES - *used ||
        length > input->size - input->bit / 8) {
      return false;
    }
    memcpy(output + *used, input->bytes + (input->bit / 8), length);
    *used += length;
    input->bit += (size_t)length * 8;
    return true;
  }
  XSHuffman literal;
  XSHuffman distance;
  if (type == 3 || !xs_inflate_trees(input, type, &literal, &distance)) {
    return false;
  }
  static const unsigned length_base[29] = {
      3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const unsigned length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                            1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                            4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const unsigned distance_base[30] = {
      1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
      33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const unsigned distance_extra[30] = {
      0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  while (input->valid) {
    unsigned symbol = xs_inflate_symbol(input, &literal);
    if (!input->valid) {
      return false;
    }
    if (symbol < 256) {
      if (*used == XS_MAX_BYTES) {
        return false;
      }
      output[(*used)++] = (unsigned char)symbol;
    } else if (symbol == 256) {
      return true;
    } else {
      if (symbol > 285) {
        return false;
      }
      unsigned length = length_base[symbol - 257] +
                        xs_inflate_bits(input, length_extra[symbol - 257]);
      unsigned slot = xs_inflate_symbol(input, &distance);
      if (slot >= 30) {
        return false;
      }
      unsigned back =
          distance_base[slot] + xs_inflate_bits(input, distance_extra[slot]);
      if (!input->valid || back > *used || length > XS_MAX_BYTES - *used) {
        return false;
      }
      for (unsigned index = 0; index < length; index++) {
        output[*used] = output[*used - back];
        (*used)++;
      }
    }
  }
  return false;
}

char *xs_gunzip(const void *bytes, size_t length, size_t *output_length) {
  *output_length = 0;
  const unsigned char *input = bytes;
  if (length < 18 || length > XS_MAX_BYTES || input[0] != 31 ||
      input[1] != 139 || input[2] != 8 || (input[3] & 224)) {
    return NULL;
  }
  size_t offset = 10;
  size_t trailer = length - 8;
  if (input[3] & 4) {
    if (offset + 2 > trailer) {
      return NULL;
    }
    unsigned extra = input[offset] | (unsigned)input[offset + 1] << 8;
    offset += 2;
    if (extra > trailer - offset) {
      return NULL;
    }
    offset += extra;
  }
  for (unsigned flag = 8; flag <= 16; flag <<= 1) {
    if (input[3] & flag) {
      while (offset < trailer && input[offset]) {
        offset++;
      }
      if (offset == trailer) {
        return NULL;
      }
      offset++;
    }
  }
  if (input[3] & 2) {
    if (offset + 2 > trailer ||
        (xs_crc32(input, offset) & 65535) !=
            (input[offset] | (unsigned)input[offset + 1] << 8)) {
      return NULL;
    }
    offset += 2;
  }
  XSInflate stream = {
      .bytes = input + offset, .size = trailer - offset, .valid = true};
  unsigned char *output = malloc_or_die(XS_MAX_BYTES + 1);
  size_t used = 0;
  bool final = false;
  bool valid = true;
  while (valid && !final) {
    final = xs_inflate_bits(&stream, 1) != 0;
    unsigned type = xs_inflate_bits(&stream, 2);
    valid = stream.valid && xs_inflate_block(&stream, type, output, &used);
  }
  valid = valid && stream.valid && (stream.bit + 7) / 8 == stream.size &&
          xs_little32(input + trailer) == xs_crc32(output, used) &&
          xs_little32(input + trailer + 4) == used;
  if (!valid) {
    free(output);
    return NULL;
  }
  output[used] = 0;
  *output_length = used;
  return (char *)output;
}

char *xs_gzip(const void *bytes, size_t length, size_t *output_length) {
  *output_length = 0;
  size_t blocks = length ? (length + 65534) / 65535 : 1;
  if (length > XS_MAX_BYTES || length + (blocks * 5) + 18 > XS_MAX_BYTES) {
    return NULL;
  }
  unsigned char *output = malloc_or_die(length + (blocks * 5) + 18);
  static const unsigned char header[10] = {31, 139, 8, 0, 0, 0, 0, 0, 0, 255};
  memcpy(output, header, sizeof(header));
  size_t used = 10;
  size_t consumed = 0;
  for (size_t index = 0; index < blocks; index++) {
    size_t count = length - consumed;
    if (count > 65535) {
      count = 65535;
    }
    output[used++] = (unsigned char)(index + 1 == blocks);
    output[used++] = (unsigned char)count;
    output[used++] = (unsigned char)(count >> 8);
    output[used++] = (unsigned char)(count ^ 65535);
    output[used++] = (unsigned char)((count ^ 65535) >> 8);
    if (count) {
      memcpy(output + used, (const unsigned char *)bytes + consumed, count);
    }
    used += count;
    consumed += count;
  }
  xs_store32(output + used, xs_crc32(bytes, length));
  xs_store32(output + used + 4, (uint32_t)length);
  *output_length = used + 8;
  return (char *)output;
}
