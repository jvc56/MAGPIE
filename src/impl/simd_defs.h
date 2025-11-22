#ifndef SIMD_DEFS_H
#define SIMD_DEFS_H

#include <stdint.h>
#include <string.h> // for memcpy

#if defined(__GNUC__) || defined(__clang__) || defined(__cppcheck__)
typedef uint8_t v16u8 __attribute__((vector_size(16)));
typedef uint64_t v2u64 __attribute__((vector_size(16)));
#else
// Fallback for non-GCC/Clang compilers (though the project seems to use
// GCC/Clang flags) In a real scenario, we might define a struct or use other
// intrinsics. For now, we assume GCC/Clang extensions are available as per
// Makefile.
#error "SIMD optimization requires GCC or Clang vector extensions"
#endif

static inline v16u8 load_v16u8(const void *p) {
  v16u8 ret;
  memcpy(&ret, p, sizeof(v16u8));
  return ret;
}

static inline void store_v16u8(void *p, v16u8 v) {
  memcpy(p, &v, sizeof(v16u8));
}

#endif
