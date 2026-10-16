#ifndef XS_COMPRESS_H
#define XS_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

uint32_t xs_crc32(const void *bytes, size_t length);
// Caller frees the result. Bounds compressed input and expanded output.
char *xs_gunzip(const void *bytes, size_t length, size_t *output_length);
char *xs_gzip(const void *bytes, size_t length, size_t *output_length);

#endif
