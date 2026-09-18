/* Public-domain SHA-256, after the reference implementation by Brad Conte
 * (https://github.com/B-Con/crypto-algorithms, released into the public
 * domain). Vendored here alongside the cjson precedent rather than taking a
 * dependency; MAGPIE code should use the wrapper in src/util/hash.h rather
 * than calling this directly. Do not edit: update by replacing this file. */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_BLOCK_SIZE 32

typedef struct {
  uint8_t data[64];
  uint32_t datalen;
  uint64_t bitlen;
  uint32_t state[8];
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len);
void sha256_final(SHA256_CTX *ctx, uint8_t hash[SHA256_BLOCK_SIZE]);

#endif
