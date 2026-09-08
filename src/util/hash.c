#include "hash.h"

#include "../compat/sha256/sha256.h"
#include "io_util.h"

#include <stdio.h>
#include <stdlib.h>

enum {
  // Big enough that hashing a 15 MB lexicon is a few hundred reads, small
  // enough to stay off the stack pressure of a worker thread.
  HASH_CHUNK_BYTES = 64 * 1024,
};

static char *hex_digest(const uint8_t digest[SHA256_BLOCK_SIZE]) {
  static const char hex[] = "0123456789abcdef";
  char *out = (char *)malloc_or_die(SHA256_BLOCK_SIZE * 2 + 1);
  for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
    out[i * 2] = hex[(digest[i] >> 4) & 0xf];
    out[i * 2 + 1] = hex[digest[i] & 0xf];
  }
  out[SHA256_BLOCK_SIZE * 2] = '\0';
  return out;
}

char *sha256_hash_bytes(const void *data, size_t length) {
  SHA256_CTX ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, (const uint8_t *)data, length);
  uint8_t digest[SHA256_BLOCK_SIZE];
  sha256_final(&ctx, digest);
  return hex_digest(digest);
}

char *sha256_hash_file(const char *path, ErrorStack *error_stack) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
        get_formatted_string("could not open %s to hash it", path));
    return NULL;
  }

  SHA256_CTX ctx;
  sha256_init(&ctx);
  uint8_t *buffer = (uint8_t *)malloc_or_die(HASH_CHUNK_BYTES);
  size_t read_bytes;
  while ((read_bytes = fread(buffer, 1, HASH_CHUNK_BYTES, file)) > 0) {
    sha256_update(&ctx, buffer, read_bytes);
  }
  const bool read_failed = ferror(file) != 0;
  free(buffer);
  fclose(file);

  if (read_failed) {
    error_stack_push(error_stack, ERROR_STATUS_FILEPATH_FILE_NOT_FOUND,
                     get_formatted_string("could not read %s while hashing it",
                                          path));
    return NULL;
  }

  uint8_t digest[SHA256_BLOCK_SIZE];
  sha256_final(&ctx, digest);
  return hex_digest(digest);
}
