#ifndef VALUE_NET_KERNELS_SCALAR_H
#define VALUE_NET_KERNELS_SCALAR_H

// The scalar fallback vector layer (1 lane) for value_net_kernels.h.

#if !(defined(__AVX2__) && defined(__FMA__)) &&                                \
    !(defined(__ARM_NEON) && defined(__aarch64__))
#include <math.h>

#define VNK_NAME "scalar"
#define VNK_W 1
#define VNK_NR_VECS 16
#define VNK_MR 4
typedef float vnk_v;
static inline vnk_v vnk_load(const float *p) { return *p; }
static inline void vnk_store(float *p, vnk_v v) { *p = v; }
static inline vnk_v vnk_set1(float x) { return x; }
static inline vnk_v vnk_zero(void) { return 0.0F; }
static inline vnk_v vnk_fma(vnk_v acc, vnk_v a, vnk_v b) {
  return acc + (a * b);
}
static inline vnk_v vnk_add(vnk_v a, vnk_v b) { return a + b; }
static inline vnk_v vnk_sub(vnk_v a, vnk_v b) { return a - b; }
static inline vnk_v vnk_mul(vnk_v a, vnk_v b) { return a * b; }
static inline vnk_v vnk_div(vnk_v a, vnk_v b) { return a / b; }
static inline vnk_v vnk_max(vnk_v a, vnk_v b) { return a > b ? a : b; }
static inline vnk_v vnk_min(vnk_v a, vnk_v b) { return a < b ? a : b; }
static inline vnk_v vnk_abs(vnk_v a) { return fabsf(a); }
static inline vnk_v vnk_copysign(vnk_v a, vnk_v b) { return copysignf(a, b); }
static inline float vnk_sum(vnk_v v) { return v; }
static inline float vnk_hmax(vnk_v v) { return v; }
static inline vnk_v vnk_exp(vnk_v x) { return expf(x); }
#endif

#endif
