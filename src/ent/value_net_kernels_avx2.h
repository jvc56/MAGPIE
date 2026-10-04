#ifndef VALUE_NET_KERNELS_AVX2_H
#define VALUE_NET_KERNELS_AVX2_H

// The AVX2 with FMA and F16C vector layer (x86-64, 8 lanes; fp16 weights
// widened to fp32 as they load: AVX2 has no fp16 arithmetic) for
// value_net_kernels.h.

#if defined(__AVX2__) && defined(__FMA__) && defined(__F16C__)
#include <immintrin.h>
#include <stdint.h>

#define VNK_NAME "avx2"
#define VNK_W 8
#define VNK_NR_VECS 2
#define VNK_MR 4
typedef __m256 vnk_v;
// An fp16 weight, as its bits.
typedef uint16_t vnk_half;
static inline vnk_half vnk_to_half(float x) {
  return (vnk_half)_cvtss_sh(x, _MM_FROUND_TO_NEAREST_INT);
}
static inline vnk_v vnk_load_half(const vnk_half *p) {
  return _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)p));
}
static inline vnk_v vnk_load(const float *p) { return _mm256_loadu_ps(p); }
static inline void vnk_store(float *p, vnk_v v) { _mm256_storeu_ps(p, v); }
static inline vnk_v vnk_set1(float x) { return _mm256_set1_ps(x); }
static inline vnk_v vnk_zero(void) { return _mm256_setzero_ps(); }
// acc + a * b
static inline vnk_v vnk_fma(vnk_v acc, vnk_v a, vnk_v b) {
  return _mm256_fmadd_ps(a, b, acc);
}
static inline vnk_v vnk_add(vnk_v a, vnk_v b) { return _mm256_add_ps(a, b); }
static inline vnk_v vnk_sub(vnk_v a, vnk_v b) { return _mm256_sub_ps(a, b); }
static inline vnk_v vnk_mul(vnk_v a, vnk_v b) { return _mm256_mul_ps(a, b); }
static inline vnk_v vnk_div(vnk_v a, vnk_v b) { return _mm256_div_ps(a, b); }
static inline vnk_v vnk_max(vnk_v a, vnk_v b) { return _mm256_max_ps(a, b); }
static inline vnk_v vnk_min(vnk_v a, vnk_v b) { return _mm256_min_ps(a, b); }
static inline vnk_v vnk_abs(vnk_v a) {
  return _mm256_andnot_ps(_mm256_set1_ps(-0.0F), a);
}
// Copies the sign of b onto a (|a| assumed).
static inline vnk_v vnk_copysign(vnk_v a, vnk_v b) {
  return _mm256_or_ps(a, _mm256_and_ps(_mm256_set1_ps(-0.0F), b));
}
static inline float vnk_sum(vnk_v v) {
  __m128 low = _mm256_castps256_ps128(v);
  __m128 high = _mm256_extractf128_ps(v, 1);
  low = _mm_add_ps(low, high);
  low = _mm_add_ps(low, _mm_movehl_ps(low, low));
  low = _mm_add_ss(low, _mm_shuffle_ps(low, low, 1));
  return _mm_cvtss_f32(low);
}
static inline float vnk_hmax(vnk_v v) {
  __m128 low = _mm256_castps256_ps128(v);
  __m128 high = _mm256_extractf128_ps(v, 1);
  low = _mm_max_ps(low, high);
  low = _mm_max_ps(low, _mm_movehl_ps(low, low));
  low = _mm_max_ss(low, _mm_shuffle_ps(low, low, 1));
  return _mm_cvtss_f32(low);
}
// exp(x) for x in [-87.3, 88.3] (clamped): Cephes' expf, about 1e-7 relative.
static inline vnk_v vnk_exp(vnk_v x) {
  x = _mm256_min_ps(_mm256_max_ps(x, _mm256_set1_ps(-87.3F)),
                    _mm256_set1_ps(88.3F));
  const vnk_v n =
      _mm256_round_ps(_mm256_mul_ps(x, _mm256_set1_ps(1.44269504088896341F)),
                      _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
  vnk_v r = _mm256_fnmadd_ps(n, _mm256_set1_ps(0.693359375F), x);
  r = _mm256_fnmadd_ps(n, _mm256_set1_ps(-2.12194440e-4F), r);
  vnk_v p = _mm256_set1_ps(1.9875691500E-4F);
  p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.3981999507E-3F));
  p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(8.3334519073E-3F));
  p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(4.1665795894E-2F));
  p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.6666665459E-1F));
  p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(5.0000001201E-1F));
  p = _mm256_fmadd_ps(p, _mm256_mul_ps(r, r), r);
  p = _mm256_add_ps(p, _mm256_set1_ps(1.0F));
  const __m256i scale = _mm256_slli_epi32(_mm256_cvtps_epi32(n), 23);
  return _mm256_castsi256_ps(_mm256_add_epi32(_mm256_castps_si256(p), scale));
}
#endif

#endif
