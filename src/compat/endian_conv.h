#ifndef ENDIAN_CONV
#define ENDIAN_CONV

#if defined(_WIN32)

#include <windows.h>
// Assumes Windows is little-endian
#define IS_LITTLE_ENDIAN 1
#define htobe16(x) _byteswap_ushort(x)
#define htole16(x) (x)
#define be16toh(x) _byteswap_ushort(x)
#define le16toh(x) (x)

#define htobe32(x) _byteswap_ulong(x)
#define htole32(x) (x)
#define be32toh(x) _byteswap_ulong(x)
#define le32toh(x) (x)

#define htobe64(x) _byteswap_uint64(x)
#define htole64(x) (x)
#define be64toh(x) _byteswap_uint64(x)
#define le64toh(x) (x)

#elif defined(__APPLE__)

#include <libkern/OSByteOrder.h>
#include <machine/endian.h> // For __DARWIN_BYTE_ORDER

#if __DARWIN_BYTE_ORDER == __DARWIN_LITTLE_ENDIAN
#define IS_LITTLE_ENDIAN 1
#else
#define IS_LITTLE_ENDIAN 0
#endif

#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)

#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)

#define htobe64(x) OSSwapHostToBigInt64(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)

#elif defined(__linux__)

#include <endian.h>

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define IS_LITTLE_ENDIAN 1
#else
#define IS_LITTLE_ENDIAN 0
#endif

#else
#error "Unsupported platform"
#endif

static inline float convert_float_to_le(const float input_float) {
#if IS_LITTLE_ENDIAN
  return input_float;
#else
  union {
    float f;
    uint32_t i;
  } u;
  u.f = input_float;
  u.i = ((u.i & 0xFF000000) >> 24) | ((u.i & 0x00FF0000) >> 8) |
        ((u.i & 0x0000FF00) << 8) | ((u.i & 0x000000FF) << 24);
  return u.f;
#endif
}

#endif /* ENDIAN_CONV */