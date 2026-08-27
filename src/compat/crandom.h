#ifndef COMPAT_CRANDOM_H
#define COMPAT_CRANDOM_H

// Cryptographically secure random bytes. The only consumer today is the
// contribution client's worker UUID, which must not be predictable across
// contributors.

#include <stddef.h>
#include <stdint.h>

#include "../util/io_util.h"
#include "../util/string_util.h"

#if defined(__wasm__)

static inline void crandom_bytes(uint8_t *buffer, size_t length,
                                 ErrorStack *error_stack) {
  (void)buffer;
  (void)length;
  error_stack_push(
      error_stack, ERROR_STATUS_RANDOM_UNAVAILABLE,
      string_duplicate("secure random bytes are not available in wasm builds"));
}

#elif defined(_WIN32)

#include <windows.h>
// bcrypt.h must follow windows.h.
#include <bcrypt.h>

static inline void crandom_bytes(uint8_t *buffer, size_t length,
                                 ErrorStack *error_stack) {
  if (BCryptGenRandom(NULL, buffer, (ULONG)length,
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    error_stack_push(error_stack, ERROR_STATUS_RANDOM_UNAVAILABLE,
                     string_duplicate("BCryptGenRandom failed"));
  }
}

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
    defined(__NetBSD__)

#include <stdlib.h>

static inline void crandom_bytes(uint8_t *buffer, size_t length,
                                 ErrorStack *error_stack) {
  (void)error_stack;
  arc4random_buf(buffer, length);
}

#else

#include <stdio.h>

static inline void crandom_bytes(uint8_t *buffer, size_t length,
                                 ErrorStack *error_stack) {
  // getrandom(2) is the preferred source but is not present on every libc we
  // build against, so /dev/urandom is the portable floor. It is seeded by the
  // same CSPRNG.
  FILE *stream = fopen("/dev/urandom", "rb");
  if (!stream) {
    error_stack_push(error_stack, ERROR_STATUS_RANDOM_UNAVAILABLE,
                     string_duplicate("could not open /dev/urandom"));
    return;
  }
  const size_t bytes_read = fread(buffer, 1, length, stream);
  fclose(stream);
  if (bytes_read != length) {
    error_stack_push(
        error_stack, ERROR_STATUS_RANDOM_UNAVAILABLE,
        string_duplicate("could not read enough bytes from /dev/urandom"));
  }
}

#endif

#endif
