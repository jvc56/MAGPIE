#include "xs_bits.h"

#include "../compat/xs_compress.h"
#include "io_util.h"
#include "string_util.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void xs_error(ErrorStack *errors, const char *message) {
  error_stack_push(errors, ERROR_STATUS_CONVERT_INPUT_FILE_ERROR,
                   get_formatted_string("XS: %s", message));
}
void xs_buffer_init(XSBuffer *buffer) { *buffer = (XSBuffer){.valid = true}; }
void xs_buffer_destroy(XSBuffer *buffer) {
  free(buffer->bytes);
  *buffer = (XSBuffer){0};
}
void xs_put(XSBuffer *buffer, uint32_t value, unsigned width) {
  if (!buffer->valid) {
    return;
  }
  if (width > 32 || (width < 32 && value >= (UINT32_C(1) << width)) ||
      buffer->bits + width > (size_t)XS_MAX_BYTES * 8) {
    buffer->valid = false;
    return;
  }
  size_t needed = (buffer->bits + width + 7) / 8;
  if (needed > buffer->capacity) {
    size_t capacity = (needed * 2) + 64;
    buffer->bytes = realloc_or_die(buffer->bytes, capacity);
    memset(buffer->bytes + buffer->capacity, 0, capacity - buffer->capacity);
    buffer->capacity = capacity;
  }
  for (unsigned bit = width; bit > 0; bit--) {
    buffer->bytes[buffer->bits / 8] |= ((value >> (bit - 1)) & 1)
                                       << (7 - buffer->bits % 8);
    buffer->bits++;
  }
}
uint32_t xs_get(XSBuffer *buffer, unsigned width) {
  if (!buffer->valid || width > 32 || buffer->cursor + width > buffer->bits) {
    buffer->valid = false;
    return 0;
  }
  uint32_t value = 0;
  for (unsigned bit = 0; bit < width; bit++) {
    value =
        (value << 1) |
        ((buffer->bytes[buffer->cursor / 8] >> (7 - buffer->cursor % 8)) & 1);
    buffer->cursor++;
  }
  return value;
}
void xs_append(XSBuffer *target, const XSBuffer *source) {
  if (!source->valid) {
    target->valid = false;
    return;
  }
  for (size_t bit = 0; bit < source->bits; bit++) {
    xs_put(target, (source->bytes[bit / 8] >> (7 - bit % 8)) & 1, 1);
  }
}
void xs_put_var(XSBuffer *buffer, uint32_t value) {
  while (value >= 128) {
    xs_put(buffer, (value & 127) | 128, 8);
    value >>= 7;
  }
  xs_put(buffer, value, 8);
}
uint32_t xs_get_var(XSBuffer *buffer) {
  uint32_t value = 0;
  for (unsigned index = 0; index < 5; index++) {
    uint32_t byte = xs_get(buffer, 8);
    if (index == 4 && byte > 15) {
      buffer->valid = false;
      return 0;
    }
    value |= (byte & 127) << (7 * index);
    if (byte < 128) {
      if (index && !byte) {
        buffer->valid = false;
      }
      return value;
    }
  }
  buffer->valid = false;
  return 0;
}
void xs_put_signed(XSBuffer *buffer, int32_t value) {
  xs_put_var(buffer, value >= 0 ? (uint32_t)value * 2
                                : (uint32_t)((-(int64_t)value * 2) - 1));
}
int32_t xs_get_signed(XSBuffer *buffer) {
  uint32_t value = xs_get_var(buffer);
  return value & 1 ? (int32_t)(-((int64_t)(value / 2)) - 1)
                   : (int32_t)(value / 2);
}
void xs_put_text(XSBuffer *buffer, const char *text) {
  size_t length = strlen(text);
  if (length > XS_MAX_TEXT) {
    buffer->valid = false;
    return;
  }
  xs_put_var(buffer, (uint32_t)length);
  for (size_t index = 0; index < length; index++) {
    xs_put(buffer, (unsigned char)text[index], 8);
  }
}
char *xs_get_text(XSBuffer *buffer) {
  uint32_t length = xs_get_var(buffer);
  if (!buffer->valid || length > XS_MAX_TEXT ||
      (size_t)length * 8 > buffer->bits - buffer->cursor) {
    buffer->valid = false;
    return string_duplicate("");
  }
  char *text = malloc_or_die(length + 1);
  for (uint32_t index = 0; index < length; index++) {
    text[index] = (char)xs_get(buffer, 8);
    if (!text[index]) {
      buffer->valid = false;
    }
  }
  text[length] = '\0';
  if (!xs_utf8(text)) {
    buffer->valid = false;
  }
  return text;
}
void xs_put_rank(XSBuffer *buffer, unsigned order, uint32_t rank) {
  if (order > 6 || (rank >> order) >= (1U << 20)) {
    buffer->valid = false;
    return;
  }
  uint32_t prefix = (rank >> order) + 1;
  unsigned width = 0;
  for (uint32_t rest = prefix; rest; rest >>= 1) {
    width++;
  }
  xs_put(buffer, 0, width - 1);
  xs_put(buffer, prefix, width);
  xs_put(buffer, rank & ((1U << order) - 1), order);
}
uint32_t xs_get_rank(XSBuffer *buffer, unsigned order) {
  if (order > 6) {
    buffer->valid = false;
    return 0;
  }
  unsigned zeros = 0;
  while (buffer->valid && xs_get(buffer, 1) == 0) {
    if (++zeros > 20) {
      buffer->valid = false;
    }
  }
  uint32_t prefix = (1U << zeros) | xs_get(buffer, zeros);
  return ((prefix - 1) << order) | xs_get(buffer, order);
}
bool xs_finish(XSBuffer *buffer) {
  size_t remain = buffer->bits - buffer->cursor;
  if (!buffer->valid || remain >= 8) {
    return false;
  }
  return xs_get(buffer, (unsigned)remain) == 0 && buffer->valid;
}
static const char xs_base64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
char *xs_token(const XSBuffer *buffer, const char *magic) {
  if (!buffer->valid) {
    return NULL;
  }
  XSBuffer packet;
  xs_buffer_init(&packet);
  for (int index = 0; index < 3; index++) {
    xs_put(&packet, (unsigned char)magic[index], 8);
  }
  xs_append(&packet, buffer);
  xs_put(&packet, 0, (unsigned)(-packet.bits & 7));
  xs_put(&packet, xs_crc32(packet.bytes, packet.bits / 8), 32);
  size_t count = (packet.bits + 5) / 6;
  char *text = malloc_or_die(count + 1);
  for (size_t index = 0; index < count; index++) {
    unsigned width = (unsigned)((packet.bits - packet.cursor) < 6
                                    ? packet.bits - packet.cursor
                                    : 6);
    text[index] = xs_base64[xs_get(&packet, width) << (6 - width)];
  }
  text[count] = '\0';
  xs_buffer_destroy(&packet);
  return text;
}
bool xs_untoken(XSBuffer *buffer, const char *token, const char *magic) {
  size_t length = strlen(token);
  if (length < 10 || length > XS_MAX_BYTES || length % 4 == 1) {
    return false;
  }
  XSBuffer packet;
  xs_buffer_init(&packet);
  for (size_t index = 0; index < length; index++) {
    const char *digit = strchr(xs_base64, token[index]);
    if (!digit) {
      packet.valid = false;
      break;
    }
    xs_put(&packet, (uint32_t)(digit - xs_base64), 6);
  }
  size_t bytes = packet.bits / 8;
  bool valid =
      packet.valid && bytes >= 8 && memcmp(packet.bytes, magic, 3) == 0;
  packet.cursor = bytes * 8;
  valid = valid && xs_finish(&packet);
  if (valid) {
    packet.cursor = (bytes - 4) * 8;
    valid = xs_get(&packet, 32) == xs_crc32(packet.bytes, bytes - 4);
  }
  if (valid) {
    for (size_t index = 3; index < bytes - 4; index++) {
      xs_put(buffer, packet.bytes[index], 8);
    }
  }
  xs_buffer_destroy(&packet);
  return valid;
}
char *xs_read_file(const char *path, size_t *length, ErrorStack *errors) {
  // Portable C mode; no spawned child owns this stream.
  // NOLINTNEXTLINE(android-cloexec-fopen)
  FILE *file = fopen(path, "rb");
  if (!file) {
    xs_error(errors, "cannot open input file");
    return NULL;
  }
  char *bytes = malloc_or_die(XS_MAX_BYTES + 1);
  *length = fread(bytes, 1, XS_MAX_BYTES, file);
  bool valid = !ferror(file);
  if (valid && *length == XS_MAX_BYTES) {
    valid = fgetc(file) == EOF && !ferror(file);
  }
  if (fclose(file) != 0) {
    valid = false;
  }
  if (!valid) {
    free(bytes);
    xs_error(errors, "input exceeds size limit or could not be read");
    return NULL;
  }
  bytes[*length] = '\0';
  return bytes;
}
bool xs_write_file(const char *path, const void *bytes, size_t length,
                   ErrorStack *errors) {
  // NOLINTNEXTLINE(android-cloexec-fopen): portable C mode.
  FILE *file = fopen(path, "wb");
  if (!file) {
    xs_error(errors, "cannot open output file");
    return false;
  }
  bool valid = fwrite(bytes, 1, length, file) == length;
  if (fclose(file) != 0) {
    valid = false;
  }
  if (!valid) {
    xs_error(errors, "cannot write output file");
  }
  return valid;
}
char *xs_asset_path(const char *data_paths, const char *name,
                    ErrorStack *errors) {
  StringSplitter *paths =
      split_string(data_paths ? data_paths : "data", ':', true);
  char *found = NULL;
  for (int index = 0;
       !found && index < string_splitter_get_number_of_items(paths); index++) {
    char *candidate = get_formatted_string(
        "%s/xs/english-v1/%s", string_splitter_get_item(paths, index), name);
    // NOLINTNEXTLINE(android-cloexec-fopen): portable C mode.
    FILE *file = fopen(candidate, "rb");
    if (file) {
      if (fclose(file) == 0) {
        found = candidate;
      } else {
        free(candidate);
      }
    } else {
      free(candidate);
    }
  }
  string_splitter_destroy(paths);
  if (!found) {
    xs_error(errors,
             "missing immutable XS asset (install MAGPIE-DATA XS profile)");
  }
  return found;
}
bool xs_int32(const char *text, int32_t *value) {
  if (!text || !*text) {
    return false;
  }
  errno = 0;
  char *end = NULL;
  long long parsed = strtoll(text, &end, 10);
  if (errno || *end || parsed < INT32_MIN || parsed > INT32_MAX) {
    return false;
  }
  *value = (int32_t)parsed;
  return true;
}

bool xs_utf8(const char *text) {
  const unsigned char *cursor = (const unsigned char *)text;
  while (*cursor) {
    unsigned first = *cursor++;
    if (first < 128) {
      continue;
    }
    unsigned count = 0;
    unsigned minimum = 0;
    unsigned value = 0;
    if (first >= 194 && first <= 223) {
      count = 1;
      minimum = 128;
      value = first & 31;
    } else if (first >= 224 && first <= 239) {
      count = 2;
      minimum = 2048;
      value = first & 15;
    } else if (first >= 240 && first <= 244) {
      count = 3;
      minimum = 65536;
      value = first & 7;
    } else {
      return false;
    }
    for (unsigned index = 0; index < count; index++) {
      if (*cursor < 128 || *cursor > 191) {
        return false;
      }
      value = (value << 6) | (*cursor++ & 63);
    }
    if (value < minimum || value > 1114111 ||
        (value >= 55296 && value <= 57343)) {
      return false;
    }
  }
  return true;
}
