#ifndef VALUE_NET_KERNELS_H
#define VALUE_NET_KERNELS_H

// SIMD kernels for the value net's CPU forward pass (value_net.c): a packed
// matrix product, layer norm, GELU and softmax, written once over a small
// vector layer with three implementations chosen at compile time: AVX2 with
// FMA and F16C (x86-64, 8 lanes), NEON (arm64, 4 lanes) and scalar (1 lane).
//
// Weights are fp16 everywhere. The matrix product computes y = x W^T + bias
// with W packed at load time (vnk_pack) from its [out, in] layout into fp16
// [in, out_padded], out_padded a multiple of VNK_NR, so a tile of rows by
// VNK_NR outputs is a register block of accumulators updated with one
// broadcast of x and a few weight loads per input. With NEON fp16
// arithmetic (VNK_HALF_GEMM) the product runs in fp16, 8 lanes, its sums
// moved into fp32 every VNK_HALF_BLOCK inputs; elsewhere the weights widen
// to fp32 as they load. Layer norm, softmax, GELU and the residual stream
// stay fp32.

#include "value_net_kernels_avx2.h"
#include "value_net_kernels_neon.h"
#include "value_net_kernels_scalar.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Outputs per register tile.
#ifndef VNK_GEMM_NR
#define VNK_GEMM_NR (VNK_W * VNK_NR_VECS)
#endif
#define VNK_NR VNK_GEMM_NR
// Packed inputs are padded to a multiple of this.
#define VNK_IN_ALIGN 8

static inline int vnk_padded(int out_dim) {
  return ((out_dim + VNK_NR - 1) / VNK_NR) * VNK_NR;
}

static inline int vnk_in_padded(int in_dim) {
  return ((in_dim + VNK_IN_ALIGN - 1) / VNK_IN_ALIGN) * VNK_IN_ALIGN;
}

// Packs W ([out_dim, in_dim], row-major) into fp16 [vnk_in_padded(in_dim),
// vnk_padded(out_dim)], zero padded; the caller frees it.
static inline vnk_half *vnk_pack(const float *weight, int out_dim, int in_dim) {
  const int padded = vnk_padded(out_dim);
  vnk_half *packed = (vnk_half *)calloc(
      (size_t)vnk_in_padded(in_dim) * (size_t)padded, sizeof(vnk_half));
  if (packed == NULL) {
    abort();
  }
  for (int out_idx = 0; out_idx < out_dim; out_idx++) {
    for (int in_idx = 0; in_idx < in_dim; in_idx++) {
      packed[((size_t)in_idx * padded) + out_idx] =
          vnk_to_half(weight[((size_t)out_idx * in_dim) + in_idx]);
    }
  }
  return packed;
}

#if defined(VNK_HALF_GEMM)

// Rows per register tile, inputs per fp16 accumulation block (each block's
// sums are added into fp32, so long products do not drift), and the most
// inputs a product takes.
#define VNK_HALF_MR 4
#define VNK_HALF_BLOCK 32
#define VNK_HALF_MAX_IN 2048

// One input of a tile: lane of each row's 8 inputs times that input's row of
// weights.
#define VNK_HALF_STEP(lane)                                                    \
  do {                                                                         \
    const float16_t *w_row = w + ((size_t)(lane) * (size_t)padded);            \
    const float16x8_t w0 = vld1q_f16(w_row);                                   \
    const float16x8_t w1 = vld1q_f16(w_row + 8);                               \
    const float16x8_t w2 = vld1q_f16(w_row + 16);                              \
    const float16x8_t w3 = vld1q_f16(w_row + 24);                              \
    for (int r = 0; r < VNK_HALF_MR; r++) {                                    \
      acc[r][0] = vfmaq_laneq_f16(acc[r][0], w0, x_v[r], lane);                \
      acc[r][1] = vfmaq_laneq_f16(acc[r][1], w1, x_v[r], lane);                \
      acc[r][2] = vfmaq_laneq_f16(acc[r][2], w2, x_v[r], lane);                \
      acc[r][3] = vfmaq_laneq_f16(acc[r][3], w3, x_v[r], lane);                \
    }                                                                          \
  } while (0)

// y[r][o] = bias[o] + sum_i x[r][i] * packed[i][o] for rows rows and the
// out_dim outputs (bias may be NULL), in fp16 with fp32 block sums. Rows of
// x (fp32) are x_stride floats apart and rows of y y_stride; packed is from
// vnk_pack, padded = vnk_padded(out_dim) columns.
static inline void vnk_gemm(const float *x, size_t x_stride, int rows,
                            int in_dim, const vnk_half *packed, int padded,
                            const float *bias, int out_dim, float *y,
                            size_t y_stride) {
  const int in_padded = vnk_in_padded(in_dim);
  if (in_padded > VNK_HALF_MAX_IN) {
    abort();
  }
  float16_t x_half[VNK_HALF_MR][VNK_HALF_MAX_IN];
  float sums[VNK_HALF_MR][VNK_NR];
  for (int row = 0; row < rows; row += VNK_HALF_MR) {
    const int height = rows - row < VNK_HALF_MR ? rows - row : VNK_HALF_MR;
    // This tile's rows of x in fp16, zero padded.
    for (int r = 0; r < VNK_HALF_MR; r++) {
      float16_t *dst = x_half[r];
      int idx = 0;
      if (r < height) {
        const float *src = x + ((size_t)(row + r) * x_stride);
        for (; idx + 4 <= in_dim; idx += 4) {
          vst1_f16(dst + idx, vcvt_f16_f32(vld1q_f32(src + idx)));
        }
        for (; idx < in_dim; idx++) {
          dst[idx] = (float16_t)src[idx];
        }
      }
      for (; idx < in_padded; idx++) {
        dst[idx] = 0;
      }
    }
    for (int col = 0; col < out_dim; col += VNK_NR) {
      const int width = out_dim - col < VNK_NR ? out_dim - col : VNK_NR;
      for (int r = 0; r < VNK_HALF_MR; r++) {
        for (int out_idx = 0; out_idx < VNK_NR; out_idx++) {
          sums[r][out_idx] =
              bias != NULL && out_idx < width ? bias[col + out_idx] : 0.0F;
        }
      }
      for (int block = 0; block < in_padded; block += VNK_HALF_BLOCK) {
        const int block_end = block + VNK_HALF_BLOCK < in_padded
                                  ? block + VNK_HALF_BLOCK
                                  : in_padded;
        float16x8_t acc[VNK_HALF_MR][4];
        for (int r = 0; r < VNK_HALF_MR; r++) {
          for (int vec = 0; vec < 4; vec++) {
            acc[r][vec] = vdupq_n_f16(0);
          }
        }
        for (int in_idx = block; in_idx < block_end; in_idx += 8) {
          float16x8_t x_v[VNK_HALF_MR];
          for (int r = 0; r < VNK_HALF_MR; r++) {
            x_v[r] = vld1q_f16(x_half[r] + in_idx);
          }
          const float16_t *w = packed + ((size_t)in_idx * padded) + col;
          VNK_HALF_STEP(0);
          VNK_HALF_STEP(1);
          VNK_HALF_STEP(2);
          VNK_HALF_STEP(3);
          VNK_HALF_STEP(4);
          VNK_HALF_STEP(5);
          VNK_HALF_STEP(6);
          VNK_HALF_STEP(7);
        }
        for (int r = 0; r < VNK_HALF_MR; r++) {
          for (int vec = 0; vec < 4; vec++) {
            float *sum = sums[r] + (vec * 8);
            vst1q_f32(sum, vaddq_f32(vld1q_f32(sum),
                                     vcvt_f32_f16(vget_low_f16(acc[r][vec]))));
            vst1q_f32(sum + 4, vaddq_f32(vld1q_f32(sum + 4),
                                         vcvt_high_f32_f16(acc[r][vec])));
          }
        }
      }
      for (int r = 0; r < height; r++) {
        memcpy(y + ((size_t)(row + r) * y_stride) + col, sums[r],
               sizeof(float) * (size_t)width);
      }
    }
  }
}

#else

// y[r][o] = bias[o] + sum_i x[r][i] * packed[i][o] for rows rows and the
// out_dim outputs (bias may be NULL), in fp32 over fp16 weights. Rows of x
// are x_stride floats apart and rows of y y_stride; packed is from vnk_pack,
// padded = vnk_padded(out_dim) columns.
static inline void vnk_gemm(const float *x, size_t x_stride, int rows,
                            int in_dim, const vnk_half *packed, int padded,
                            const float *bias, int out_dim, float *y,
                            size_t y_stride) {
  float tail[VNK_MR][VNK_NR];
  for (int col = 0; col < out_dim; col += VNK_NR) {
    const int width = out_dim - col < VNK_NR ? out_dim - col : VNK_NR;
    vnk_v bias_v[VNK_NR_VECS];
    for (int vec = 0; vec < VNK_NR_VECS; vec++) {
      if (bias != NULL && width == VNK_NR) {
        bias_v[vec] = vnk_load(bias + col + (vec * VNK_W));
      } else if (bias != NULL) {
        float lanes[VNK_NR] = {0};
        memcpy(lanes, bias + col, sizeof(float) * (size_t)width);
        bias_v[vec] = vnk_load(lanes + (vec * VNK_W));
      } else {
        bias_v[vec] = vnk_zero();
      }
    }
    int row = 0;
    for (; row + VNK_MR <= rows; row += VNK_MR) {
      vnk_v acc[VNK_MR][VNK_NR_VECS];
      for (int r = 0; r < VNK_MR; r++) {
        for (int vec = 0; vec < VNK_NR_VECS; vec++) {
          acc[r][vec] = bias_v[vec];
        }
      }
      const float *x_rows = x + ((size_t)row * x_stride);
      const vnk_half *w = packed + col;
      for (int in_idx = 0; in_idx < in_dim; in_idx++) {
        vnk_v w_v[VNK_NR_VECS];
        for (int vec = 0; vec < VNK_NR_VECS; vec++) {
          w_v[vec] = vnk_load_half(w + (vec * VNK_W));
        }
        for (int r = 0; r < VNK_MR; r++) {
          const vnk_v x_v = vnk_set1(x_rows[((size_t)r * x_stride) + in_idx]);
          for (int vec = 0; vec < VNK_NR_VECS; vec++) {
            acc[r][vec] = vnk_fma(acc[r][vec], x_v, w_v[vec]);
          }
        }
        w += padded;
      }
      for (int r = 0; r < VNK_MR; r++) {
        float *y_row = y + ((size_t)(row + r) * y_stride) + col;
        if (width == VNK_NR) {
          for (int vec = 0; vec < VNK_NR_VECS; vec++) {
            vnk_store(y_row + (vec * VNK_W), acc[r][vec]);
          }
        } else {
          for (int vec = 0; vec < VNK_NR_VECS; vec++) {
            vnk_store(tail[r] + (vec * VNK_W), acc[r][vec]);
          }
          memcpy(y_row, tail[r], sizeof(float) * (size_t)width);
        }
      }
    }
    for (; row < rows; row++) {
      vnk_v acc[VNK_NR_VECS];
      for (int vec = 0; vec < VNK_NR_VECS; vec++) {
        acc[vec] = bias_v[vec];
      }
      const float *x_row = x + ((size_t)row * x_stride);
      const vnk_half *w = packed + col;
      for (int in_idx = 0; in_idx < in_dim; in_idx++) {
        const vnk_v x_v = vnk_set1(x_row[in_idx]);
        for (int vec = 0; vec < VNK_NR_VECS; vec++) {
          acc[vec] = vnk_fma(acc[vec], x_v, vnk_load_half(w + (vec * VNK_W)));
        }
        w += padded;
      }
      float *y_row = y + ((size_t)row * y_stride) + col;
      for (int vec = 0; vec < VNK_NR_VECS; vec++) {
        vnk_store(tail[0] + (vec * VNK_W), acc[vec]);
      }
      memcpy(y_row, tail[0], sizeof(float) * (size_t)width);
    }
  }
}

#endif

// Layer norm of each of rows rows of width floats (width a multiple of
// VNK_W), into out.
static inline void vnk_layer_norm(const float *in, int rows, int width,
                                  const float *weight, const float *bias,
                                  float eps, float *out) {
  for (int row = 0; row < rows; row++) {
    const float *x = in + ((size_t)row * width);
    float *y = out + ((size_t)row * width);
    vnk_v sum = vnk_zero();
    for (int idx = 0; idx < width; idx += VNK_W) {
      sum = vnk_add(sum, vnk_load(x + idx));
    }
    const float mean = vnk_sum(sum) / (float)width;
    const vnk_v mean_v = vnk_set1(mean);
    vnk_v squares = vnk_zero();
    for (int idx = 0; idx < width; idx += VNK_W) {
      const vnk_v centered = vnk_sub(vnk_load(x + idx), mean_v);
      squares = vnk_fma(squares, centered, centered);
    }
    const float scale = 1.0F / sqrtf((vnk_sum(squares) / (float)width) + eps);
    const vnk_v scale_v = vnk_set1(scale);
    for (int idx = 0; idx < width; idx += VNK_W) {
      const vnk_v normed = vnk_mul(vnk_sub(vnk_load(x + idx), mean_v), scale_v);
      vnk_store(y + idx,
                vnk_fma(vnk_load(bias + idx), normed, vnk_load(weight + idx)));
    }
  }
}

// x * 0.5 * (1 + erf(x / sqrt 2)) in place over count floats (a multiple of
// VNK_W), erf by Abramowitz and Stegun 7.1.26 (absolute error under 1.5e-7).
static inline void vnk_gelu(float *x, size_t count) {
  const vnk_v inv_sqrt2 = vnk_set1(0.70710678118654752F);
  const vnk_v half = vnk_set1(0.5F);
  const vnk_v one = vnk_set1(1.0F);
  const vnk_v p = vnk_set1(0.3275911F);
  const vnk_v a1 = vnk_set1(0.254829592F);
  const vnk_v a2 = vnk_set1(-0.284496736F);
  const vnk_v a3 = vnk_set1(1.421413741F);
  const vnk_v a4 = vnk_set1(-1.453152027F);
  const vnk_v a5 = vnk_set1(1.061405429F);
  for (size_t idx = 0; idx < count; idx += VNK_W) {
    const vnk_v v = vnk_load(x + idx);
    const vnk_v z = vnk_mul(v, inv_sqrt2);
    const vnk_v az = vnk_abs(z);
    const vnk_v t = vnk_div(one, vnk_fma(one, p, az));
    vnk_v poly = vnk_fma(a4, a5, t);
    poly = vnk_fma(a3, poly, t);
    poly = vnk_fma(a2, poly, t);
    poly = vnk_fma(a1, poly, t);
    poly = vnk_mul(poly, t);
    const vnk_v e = vnk_exp(vnk_mul(vnk_set1(-1.0F), vnk_mul(az, az)));
    const vnk_v erf_abs = vnk_sub(one, vnk_mul(poly, e));
    const vnk_v erf_v = vnk_copysign(erf_abs, z);
    vnk_store(x + idx, vnk_mul(vnk_mul(half, v), vnk_add(one, erf_v)));
  }
}

// Softmax of scale * x over count floats in place (x padded to a multiple of
// VNK_W; the padding is overwritten with zeros).
static inline void vnk_softmax(float *x, int count, float scale) {
  const int padded = ((count + VNK_W - 1) / VNK_W) * VNK_W;
  for (int idx = count; idx < padded; idx++) {
    x[idx] = -INFINITY;
  }
  const vnk_v scale_v = vnk_set1(scale);
  vnk_v highest = vnk_set1(-INFINITY);
  for (int idx = 0; idx < padded; idx += VNK_W) {
    highest = vnk_max(highest, vnk_mul(vnk_load(x + idx), scale_v));
  }
  const vnk_v top = vnk_set1(vnk_hmax(highest));
  vnk_v total = vnk_zero();
  for (int idx = 0; idx < padded; idx += VNK_W) {
    const vnk_v e = vnk_exp(vnk_sub(vnk_mul(vnk_load(x + idx), scale_v), top));
    vnk_store(x + idx, e);
    total = vnk_add(total, e);
  }
  const vnk_v inv = vnk_set1(1.0F / vnk_sum(total));
  for (int idx = 0; idx < padded; idx += VNK_W) {
    vnk_store(x + idx, vnk_mul(vnk_load(x + idx), inv));
  }
  for (int idx = count; idx < padded; idx++) {
    x[idx] = 0.0F;
  }
}

#endif
