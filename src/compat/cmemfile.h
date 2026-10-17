#ifndef CMEMFILE_H
#define CMEMFILE_H

#include <stddef.h>
#include <stdio.h>

// Opens `size` bytes at `data` as a stream to read, as fmemopen(data, size,
// "rb") does. POSIX reads the bytes in place, so they must outlive the
// stream; Windows has no fmemopen and reads a copy, from a temporary file
// that is deleted when the stream closes. Returns NULL with errno set on
// failure.
FILE *cmemfile_open(const void *data, size_t size);

#endif
