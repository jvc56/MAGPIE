#ifndef GZIP_READER_H
#define GZIP_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Receives the next `length` decompressed bytes. Returns false to stop.
typedef bool (*gzip_reader_sink_t)(void *context, const unsigned char *bytes,
                                   size_t length);

// Decompresses the gzip stream (RFC 1952, DEFLATE inside, RFC 1951) read from
// `input`, handing the bytes to `sink` in order, a window at a time, so a
// stream of any size needs only a few kilobytes of memory. Checks the
// trailer's CRC-32 and length. Returns false if the stream is malformed or
// truncated, a read fails, or the sink stops.
bool gzip_reader_decompress(FILE *input, gzip_reader_sink_t sink,
                            void *context);

#endif
