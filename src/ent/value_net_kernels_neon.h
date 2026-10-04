#ifndef VALUE_NET_KERNELS_NEON_H
#define VALUE_NET_KERNELS_NEON_H

// The NEON vector layer (arm64, 4 lanes) for value_net_kernels.h.

#if defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>

#define VNK_NAME "neon"
#define VNK_W 4
#define VNK_NR_VECS 4
#define VNK_MR 6
typedef float32x4_t vnk_v;
static inline vnk_v vnk_load(const float *p) { return vld1q_f32(p); }
static inline void vnk_store(float *p, vnk_v v) { vst1q_f32(p, v); }
static inline vnk_v vnk_set1(float x) { return vdupq_n_f32(x); }
static inline vnk_v vnk_zero(void) { return vdupq_n_f32(0.0F); }
static inline vnk_v vnk_fma(vnk_v acc, vnk_v a, vnk_v b) {
  return vfmaq_f32(acc, a, b);
}
static inline vnk_v vnk_add(vnk_v a, vnk_v b) { return vaddq_f32(a, b); }
static inline vnk_v vnk_sub(vnk_v a, vnk_v b) { return vsubq_f32(a, b); }
static inline vnk_v vnk_mul(vnk_v a, vnk_v b) { return vmulq_f32(a, b); }
static inline vnk_v vnk_div(vnk_v a, vnk_v b) { return vdivq_f32(a, b); }
static inline vnk_v vnk_max(vnk_v a, vnk_v b) { return vmaxq_f32(a, b); }
static inline vnk_v vnk_min(vnk_v a, vnk_v b) { return vminq_f32(a, b); }
static inline vnk_v vnk_abs(vnk_v a) { return vabsq_f32(a); }
static inline vnk_v vnk_copysign(vnk_v a, vnk_v b) {
  return vbslq_f32(vdupq_n_u32(0x80000000U), b, a);
}
static inline float vnk_sum(vnk_v v) { return vaddvq_f32(v); }
static inline float vnk_hmax(vnk_v v) { return vmaxvq_f32(v); }
static inline vnk_v vnk_exp(vnk_v x) {
  x = vminq_f32(vmaxq_f32(x, vdupq_n_f32(-87.3F)), vdupq_n_f32(88.3F));
  const vnk_v n = vrndnq_f32(vmulq_f32(x, vdupq_n_f32(1.44269504088896341F)));
  vnk_v r = vfmsq_f32(x, n, vdupq_n_f32(0.693359375F));
  r = vfmsq_f32(r, n, vdupq_n_f32(-2.12194440e-4F));
  vnk_v p = vdupq_n_f32(1.9875691500E-4F);
  p = vfmaq_f32(vdupq_n_f32(1.3981999507E-3F), p, r);
  p = vfmaq_f32(vdupq_n_f32(8.3334519073E-3F), p, r);
  p = vfmaq_f32(vdupq_n_f32(4.1665795894E-2F), p, r);
  p = vfmaq_f32(vdupq_n_f32(1.6666665459E-1F), p, r);
  p = vfmaq_f32(vdupq_n_f32(5.0000001201E-1F), p, r);
  p = vfmaq_f32(r, p, vmulq_f32(r, r));
  p = vaddq_f32(p, vdupq_n_f32(1.0F));
  const int32x4_t scale = vshlq_n_s32(vcvtq_s32_f32(n), 23);
  return vreinterpretq_f32_s32(vaddq_s32(vreinterpretq_s32_f32(p), scale));
}
#endif

#endif
