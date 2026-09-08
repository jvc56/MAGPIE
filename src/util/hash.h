#ifndef HASH_H
#define HASH_H

// SHA-256 over a file, for verifying that the input data on this machine is
// the input data a job pins.
//
// birdtest sends the digest it expects for every file a task will load; a
// worker that cannot reproduce it declines the task rather than contributing
// results computed from different bytes. The hash itself is vendored in
// src/compat/sha256; this is the ErrorStack-aware wrapper MAGPIE code uses.

#include "io_util.h"

// SHA-256 of the file at `path`, as a newly allocated lowercase hex string the
// caller frees. Pushes onto `error_stack` and returns NULL if the file cannot
// be read.
//
// Streams the file in chunks rather than mapping it: a .kwg runs to 15 MB and
// more, and there is no reason to hold one in memory to hash it.
char *sha256_hash_file(const char *path, ErrorStack *error_stack);

// SHA-256 of a memory buffer, same hex form. Used for the wordmap sidecar and
// in tests.
char *sha256_hash_bytes(const void *data, size_t length);

#endif
