#include "xs_compress.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <zconf.h>
#include <zlib.h>

enum { XS_MAX_BYTES = 8 * 1024 * 1024 };
uint32_t xs_crc32(const void *bytes, size_t length) {
  return (uint32_t)crc32(0, bytes, (uInt)length);
}
char *xs_gunzip(const void *bytes, size_t length, size_t *output_length) {
  if (length > XS_MAX_BYTES) {
    return NULL;
  }
  z_stream stream = {0};
  stream.next_in = (Bytef *)bytes;
  stream.avail_in = (uInt)length;
  if (inflateInit2(&stream, 15 + 16) != Z_OK) {
    return NULL;
  }
  char *result = malloc_or_die(XS_MAX_BYTES + 1);
  stream.next_out = (Bytef *)result;
  stream.avail_out = XS_MAX_BYTES;
  int status = inflate(&stream, Z_FINISH);
  bool valid = status == Z_STREAM_END && stream.avail_in == 0;
  *output_length = stream.total_out;
  inflateEnd(&stream);
  if (!valid) {
    free(result);
    return NULL;
  }
  result[*output_length] = '\0';
  return result;
}
char *xs_gzip(const void *bytes, size_t length, size_t *output_length) {
  if (length > XS_MAX_BYTES) {
    return NULL;
  }
  z_stream stream = {0};
  if (deflateInit2(&stream, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
      Z_OK) {
    return NULL;
  }
  gz_header header = {0};
  header.os = 255;
  deflateSetHeader(&stream, &header);
  size_t capacity = deflateBound(&stream, (uLong)length);
  char *result = malloc_or_die(capacity);
  stream.next_in = (Bytef *)bytes;
  stream.avail_in = (uInt)length;
  stream.next_out = (Bytef *)result;
  stream.avail_out = (uInt)capacity;
  int status = deflate(&stream, Z_FINISH);
  *output_length = stream.total_out;
  deflateEnd(&stream);
  if (status != Z_STREAM_END) {
    free(result);
    return NULL;
  }
  return result;
}
