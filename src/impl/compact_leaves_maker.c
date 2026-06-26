#include "compact_leaves_maker.h"

#include "../ent/equity.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  CLM_VC_DIM = COMPACT_LEAVES_VC_DIM,          // 5
  CLM_VC_CLAMP = COMPACT_LEAVES_VC_CLAMP,      // 4
  CLM_VC_CELLS = COMPACT_LEAVES_VC_TABLE_SIZE, // 25
  CLM_MAX_LEAVE = (RACK_SIZE)-1,               // 6
  // Open-addressing table for distinct sub-multiset candidates. 2^22 covers the
  // largest shipped distribution (Polish ~2.64M distinct size-2..6 combos) at a
  // safe load factor; cand_hash_add aborts rather than spin if ever full.
  CLM_HASH_BITS = 22,
  CLM_HASH_SIZE = 1 << CLM_HASH_BITS,
  // Most synergy sub-multisets a single leave (<= 6 tiles) can contain is
  // C(6,2)+...+C(6,6) = 57; round up for headroom.
  CLM_MAX_ACTIVE = 64,
  // Cap on conjugate-gradient iterations when jointly fitting synergy coefs.
  // Large synergy sets (tens of thousands, e.g. bit-packed 256 KB) are
  // ill-conditioned and need many iterations; matvecs are cheap (CSR) and the
  // relative-residual tolerance stops early once converged.
  CLM_CG_MAX_ITERS = 1000,
};

// Conjugate-gradient convergence: stop when the residual norm drops to this
// fraction of the initial right-hand-side norm.
static const double CLM_CG_TOL = 1e-4;

// ---- open-addressing hash: packed-multiset key -> candidate index ----
typedef struct CandHash {
  uint64_t *keys; // 0 = empty
  double *s1;     // sum_L w_L * residual_L over leaves containing the combo
  double *s2;     // sum_L w_L
  uint8_t *ntiles;
  uint32_t count;
} CandHash;

static void cand_hash_create(CandHash *h) {
  h->keys = (uint64_t *)calloc_or_die(CLM_HASH_SIZE, sizeof(uint64_t));
  h->s1 = (double *)calloc_or_die(CLM_HASH_SIZE, sizeof(double));
  h->s2 = (double *)calloc_or_die(CLM_HASH_SIZE, sizeof(double));
  h->ntiles = (uint8_t *)calloc_or_die(CLM_HASH_SIZE, 1);
  h->count = 0;
}

static void cand_hash_destroy(CandHash *h) {
  free(h->keys);
  free(h->s1);
  free(h->s2);
  free(h->ntiles);
}

static void cand_hash_add(CandHash *h, uint64_t key, uint8_t ntiles, double w,
                          double residual) {
  uint64_t slot = (key * 0x9E3779B97F4A7C15ULL) >> (64 - CLM_HASH_BITS);
  uint32_t probes = 0;
  for (;;) {
    if (h->keys[slot] == 0) {
      h->keys[slot] = key;
      h->ntiles[slot] = ntiles;
      h->count++;
      break;
    }
    if (h->keys[slot] == key) {
      break;
    }
    slot = (slot + 1) & (CLM_HASH_SIZE - 1);
    if (++probes >= CLM_HASH_SIZE) {
      log_fatal(
          "compact leaves candidate hash is full; increase CLM_HASH_BITS");
    }
  }
  h->s1[slot] += w * residual;
  h->s2[slot] += w;
}

// ---- dense SPD solve via Cholesky (G is p x p, row-major) ----
static void cholesky_solve(double *g, const double *r, double *x, int p) {
  // In-place Cholesky factor of g (lower), then forward/back substitution.
  for (int i = 0; i < p; i++) {
    for (int j = 0; j <= i; j++) {
      double sum = g[i * p + j];
      for (int k = 0; k < j; k++) {
        sum -= g[i * p + k] * g[j * p + k];
      }
      if (i == j) {
        g[i * p + j] = sqrt(sum > 1e-12 ? sum : 1e-12);
      } else {
        g[i * p + j] = sum / g[j * p + j];
      }
    }
  }
  for (int i = 0; i < p; i++) {
    double sum = r[i];
    for (int k = 0; k < i; k++) {
      sum -= g[i * p + k] * x[k];
    }
    x[i] = sum / g[i * p + i];
  }
  for (int i = p - 1; i >= 0; i--) {
    double sum = x[i];
    for (int k = i + 1; k < p; k++) {
      sum -= g[k * p + i] * x[k];
    }
    x[i] = sum / g[i * p + i];
  }
}

// ---- vowel/consonant cell (0..24), with (0,0) pinned (returns -1) ----
static int vc_cell(int num_vowels, int num_consonants) {
  const int v = num_vowels > CLM_VC_CLAMP ? CLM_VC_CLAMP : num_vowels;
  const int c = num_consonants > CLM_VC_CLAMP ? CLM_VC_CLAMP : num_consonants;
  const int cell = v * CLM_VC_DIM + c;
  return cell; // caller treats cell 0 as the pinned/absorbed reference
}

// Per-leave fixed-point quantizer: points -> ticks at the given radix.
static int16_t quantize_ticks(double coef_points, int32_t radix_millipoints) {
  long t = lround(coef_points * 1000.0 / (double)radix_millipoints);
  if (t > 32767) {
    t = 32767;
  } else if (t < -32768) {
    t = -32768;
  }
  return (int16_t)t;
}

// Context threaded through the leave enumeration.
typedef struct FitState {
  const KLV *klv;
  const LetterDistribution *ld;
  const uint64_t *weights; // by KLV word index; NULL => uniform
  uint8_t dist_size;
  uint64_t vowel_bits;
  Rack *leave;
  int base_p;
} FitState;

static inline double leave_weight(const FitState *st, uint32_t word_index) {
  if (st->weights == NULL) {
    return 1.0;
  }
  return (double)st->weights[word_index];
}

// Builds the sparse base feature row (col,val pairs) for the current leave and
// returns its length; also returns nv/nc. cols/vals must hold >= 2 + dist_size.
static int base_row(const FitState *st, const Rack *leave, int *cols,
                    double *vals, int *out_nv, int *out_nc) {
  const int A = st->dist_size;
  int len = 0;
  cols[len] = 0;
  vals[len] = 1.0; // intercept
  len++;
  int nv = 0;
  int nc = 0;
  for (int ml = 0; ml < A; ml++) {
    const int n = rack_get_letter(leave, ml);
    if (n == 0) {
      continue;
    }
    cols[len] = 1 + ml;
    vals[len] = n;
    len++;
    if (n > 1) {
      cols[len] = 1 + A + ml;
      vals[len] = n - 1;
      len++;
    }
    if (ml != 0) {
      if ((st->vowel_bits >> ml) & 1U) {
        nv += n;
      } else {
        nc += n;
      }
    }
  }
  const int cell = vc_cell(nv, nc);
  if (cell != 0) {
    cols[len] = 1 + 2 * A + (cell - 1);
    vals[len] = 1.0;
    len++;
  }
  *out_nv = nv;
  *out_nc = nc;
  return len;
}

// Recursively enumerate every leave (multiset of size 1..CLM_MAX_LEAVE)
// drawable from bag, invoking cb for each. cb reads st->leave.
typedef void (*leave_cb_t)(FitState *, void *);
static void enum_leaves(FitState *st, const Rack *bag, int ml, int size,
                        leave_cb_t cb, void *cb_data) {
  if (ml >= st->dist_size) {
    if (size >= 1) {
      cb(st, cb_data);
    }
    return;
  }
  int max_here = rack_get_letter(bag, ml);
  if (max_here > CLM_MAX_LEAVE - size) {
    max_here = CLM_MAX_LEAVE - size;
  }
  for (int c = 0; c <= max_here; c++) {
    if (c > 0) {
      rack_add_letter(st->leave, ml);
    }
    enum_leaves(st, bag, ml + 1, size + c, cb, cb_data);
  }
  // Restore: remove the copies of ml we added in this frame.
  while (rack_get_letter(st->leave, ml) > 0) {
    rack_take_letter(st->leave, ml);
  }
}

// ---- pass 1: accumulate base Gram + r ----
typedef struct GramAcc {
  double *gram;
  double *rvec;
  int p;
} GramAcc;
static void cb_gram(FitState *st, void *data) {
  GramAcc *acc = (GramAcc *)data;
  int cols[2 + MAX_ALPHABET_SIZE];
  double vals[2 + MAX_ALPHABET_SIZE];
  int nv;
  int nc;
  const int len = base_row(st, st->leave, cols, vals, &nv, &nc);
  const uint32_t idx = klv_get_word_index(st->klv, st->leave);
  const double w = leave_weight(st, idx);
  if (w == 0.0) {
    return;
  }
  const double y = equity_to_double(klv_get_indexed_leave_value(st->klv, idx));
  const int p = acc->p;
  for (int a = 0; a < len; a++) {
    acc->rvec[cols[a]] += w * vals[a] * y;
    for (int b = 0; b < len; b++) {
      acc->gram[cols[a] * p + cols[b]] += w * vals[a] * vals[b];
    }
  }
}

// Collects the (machine-letter, count) pairs of the present tiles in a leave
// into pml/pcnt and returns how many distinct tiles are present.
static int present_tiles(const Rack *leave, int dist_size, int *pml,
                         int *pcnt) {
  int num_present = 0;
  for (int ml = 0; ml < dist_size; ml++) {
    const int n = rack_get_letter(leave, ml);
    if (n > 0) {
      pml[num_present] = ml;
      pcnt[num_present] = n;
      num_present++;
    }
  }
  return num_present;
}

// Generic enumeration of every sub-multiset (size 2..CLM_MAX_LEAVE) of the
// present tiles, invoking cb(packed_key, ntiles, ctx) for each.
typedef void (*submultiset_cb)(uint64_t key, uint8_t ntiles, void *ctx);
static void walk_submultisets(const int *pml, const int *pcnt, int num_present,
                              int idx, int subsize, uint64_t key,
                              submultiset_cb cb, void *ctx) {
  if (idx == num_present) {
    if (subsize >= 2) {
      cb(key, (uint8_t)subsize, ctx);
    }
    return;
  }
  // choose how many of present tile idx to include (0..pcnt[idx]), capped so
  // total stays <= CLM_MAX_LEAVE.
  int maxc = pcnt[idx];
  if (maxc > CLM_MAX_LEAVE - subsize) {
    maxc = CLM_MAX_LEAVE - subsize;
  }
  for (int count = 0; count <= maxc; count++) {
    uint64_t new_key = key;
    for (int copy = 0; copy < count; copy++) {
      new_key = (new_key << 6) | (uint64_t)(pml[idx] + 1);
    }
    walk_submultisets(pml, pcnt, num_present, idx + 1, subsize + count, new_key,
                      cb, ctx);
  }
}

// ---- pass 2: residual (vs quantized base) + candidate synergy accumulation --
typedef struct ResAcc {
  const CompactLeaves *cl; // base-only model (num_synergies == 0)
  CandHash *hash;
} ResAcc;
typedef struct HashSink {
  CandHash *hash;
  double w;
  double e;
} HashSink;
static void hash_sink_cb(uint64_t key, uint8_t ntiles, void *ctx) {
  HashSink *sink = (HashSink *)ctx;
  cand_hash_add(sink->hash, key, ntiles, sink->w, sink->e);
}

static void cb_residual(FitState *st, void *data) {
  const ResAcc *acc = (const ResAcc *)data;
  const uint32_t idx = klv_get_word_index(st->klv, st->leave);
  const double w = leave_weight(st, idx);
  if (w == 0.0) {
    return;
  }
  const double y = equity_to_double(klv_get_indexed_leave_value(st->klv, idx));
  const double base =
      equity_to_double(compact_leaves_get_leave_value(acc->cl, st->leave));
  const double e = y - base;
  int pml[MAX_ALPHABET_SIZE];
  int pcnt[MAX_ALPHABET_SIZE];
  const int num_present = present_tiles(st->leave, st->dist_size, pml, pcnt);
  HashSink sink = {.hash = acc->hash, .w = w, .e = e};
  walk_submultisets(pml, pcnt, num_present, 0, 0, 0, hash_sink_cb, &sink);
}

// ---- selected-synergy hash: packed key -> index in the selected array ----
typedef struct SelHash {
  uint64_t *keys; // 0 = empty (a real synergy key is always nonzero)
  int32_t *idx;
  uint32_t cap;
  int shift;
} SelHash;
static void sel_hash_create(SelHash *h, uint32_t num_selected) {
  uint32_t cap = 16;
  while (cap < num_selected * 2 + 1) {
    cap <<= 1;
  }
  h->keys = (uint64_t *)calloc_or_die(cap, sizeof(uint64_t));
  h->idx = (int32_t *)malloc_or_die((size_t)cap * sizeof(int32_t));
  h->cap = cap;
  int bits = 0;
  while ((1u << bits) < cap) {
    bits++;
  }
  h->shift = 64 - bits;
}

static void sel_hash_destroy(SelHash *h) {
  free(h->keys);
  free(h->idx);
}

static void sel_hash_put(SelHash *h, uint64_t key, int32_t value) {
  uint64_t slot = (key * 0x9E3779B97F4A7C15ULL) >> h->shift;
  while (h->keys[slot] != 0) {
    slot = (slot + 1) & (h->cap - 1);
  }
  h->keys[slot] = key;
  h->idx[slot] = value;
}

static int32_t sel_hash_get(const SelHash *h, uint64_t key) {
  uint64_t slot = (key * 0x9E3779B97F4A7C15ULL) >> h->shift;
  while (h->keys[slot] != 0) {
    if (h->keys[slot] == key) {
      return h->idx[slot];
    }
    slot = (slot + 1) & (h->cap - 1);
  }
  return -1;
}

// ---- backfitting: collect the selected synergies present in a leave ----
typedef struct CollectSink {
  const SelHash *sel;
  int *active;
  int num_active;
} CollectSink;
static void collect_sink_cb(uint64_t key, uint8_t ntiles, void *ctx) {
  (void)ntiles;
  CollectSink *sink = (CollectSink *)ctx;
  const int32_t s = sel_hash_get(sink->sel, key);
  if (s >= 0 && sink->num_active < CLM_MAX_ACTIVE) {
    sink->active[sink->num_active++] = s;
  }
}

// Returns the selected synergies present in the current leave (indices into the
// selected array) via sink, and the leave's weight (0 => skip).
static double leave_active(const FitState *st, const SelHash *sel,
                           CollectSink *sink) {
  const uint32_t idx = klv_get_word_index(st->klv, st->leave);
  const double w = leave_weight(st, idx);
  if (w == 0.0) {
    return 0.0;
  }
  int pml[MAX_ALPHABET_SIZE];
  int pcnt[MAX_ALPHABET_SIZE];
  const int num_present = present_tiles(st->leave, st->dist_size, pml, pcnt);
  sink->sel = sel;
  sink->num_active = 0;
  walk_submultisets(pml, pcnt, num_present, 0, 0, 0, collect_sink_cb, sink);
  return w;
}

// We jointly fit the selected synergies by conjugate gradient on the ridge
// normal equations (Phi^T W Phi + ridge I) x = Phi^T W r, where r = y - base.
// CG is robust for this SPD system regardless of conditioning (plain Jacobi
// backfitting diverges because nested combos are far from diagonally dominant,
// and a dense Gram is infeasible at tens of thousands of synergies). The leaf x
// synergy indicator matrix is applied implicitly via a CSR of each leaf's
// active synergies, built once so CG iterations are pure arithmetic.

// ---- pass A: count contributing rows (w > 0 and >= 1 active synergy) + nnz --
typedef struct CsrCountAcc {
  const SelHash *sel;
  uint32_t num_rows;
  size_t nnz;
} CsrCountAcc;
static void cb_csr_count(FitState *st, void *data) {
  CsrCountAcc *acc = (CsrCountAcc *)data;
  int active[CLM_MAX_ACTIVE] = {0};
  CollectSink sink = {.sel = acc->sel, .active = active, .num_active = 0};
  const double w = leave_active(st, acc->sel, &sink);
  if (w == 0.0 || sink.num_active == 0) {
    return;
  }
  acc->num_rows++;
  acc->nnz += (size_t)sink.num_active;
}

// ---- pass B: fill the CSR (row weight, residual r = y - base, active set)
// ----
typedef struct CsrFillAcc {
  const CompactLeaves *cl; // base-only model
  const SelHash *sel;
  double *row_w;
  double *row_rhs;
  size_t *row_off;
  int *act;
  uint32_t nr;
  size_t cur;
} CsrFillAcc;
static void cb_csr_fill(FitState *st, void *data) {
  CsrFillAcc *acc = (CsrFillAcc *)data;
  int active[CLM_MAX_ACTIVE] = {0};
  CollectSink sink = {.sel = acc->sel, .active = active, .num_active = 0};
  const double w = leave_active(st, acc->sel, &sink);
  if (w == 0.0 || sink.num_active == 0) {
    return;
  }
  const uint32_t idx = klv_get_word_index(st->klv, st->leave);
  const double y = equity_to_double(klv_get_indexed_leave_value(st->klv, idx));
  const double base =
      equity_to_double(compact_leaves_get_leave_value(acc->cl, st->leave));
  acc->row_w[acc->nr] = w;
  acc->row_rhs[acc->nr] = y - base;
  acc->row_off[acc->nr] = acc->cur;
  for (int a = 0; a < sink.num_active; a++) {
    acc->act[acc->cur++] = active[a];
  }
  acc->nr++;
}

// Jointly fits the selected synergy coefficients (coef in/out: warm-started
// from the marginal estimates, overwritten with the CG solution).
static void solve_synergies_cg(FitState *st, const Rack *bag,
                               const CompactLeaves *cl, const SelHash *sel,
                               uint32_t num_synergies, double ridge,
                               double *coef) {
  CsrCountAcc count_acc = {.sel = sel, .num_rows = 0, .nnz = 0};
  enum_leaves(st, bag, 0, 0, cb_csr_count, &count_acc);
  const uint32_t num_rows = count_acc.num_rows;
  const size_t nnz = count_acc.nnz;
  if (num_rows == 0 || nnz == 0) {
    for (uint32_t s = 0; s < num_synergies; s++) {
      coef[s] = 0.0;
    }
    return;
  }
  double *row_w = (double *)malloc_or_die((size_t)num_rows * sizeof(double));
  double *row_rhs = (double *)malloc_or_die((size_t)num_rows * sizeof(double));
  size_t *row_off =
      (size_t *)malloc_or_die(((size_t)num_rows + 1) * sizeof(size_t));
  int *act = (int *)malloc_or_die(nnz * sizeof(int));
  CsrFillAcc fill_acc = {.cl = cl,
                         .sel = sel,
                         .row_w = row_w,
                         .row_rhs = row_rhs,
                         .row_off = row_off,
                         .act = act,
                         .nr = 0,
                         .cur = 0};
  enum_leaves(st, bag, 0, 0, cb_csr_fill, &fill_acc);
  row_off[fill_acc.nr] = fill_acc.cur;

  double *rhs = (double *)calloc_or_die(num_synergies, sizeof(double));
  for (uint32_t r = 0; r < num_rows; r++) {
    const double wr = row_w[r] * row_rhs[r];
    for (size_t k = row_off[r]; k < row_off[r + 1]; k++) {
      rhs[act[k]] += wr;
    }
  }
  double *res = (double *)malloc_or_die(num_synergies * sizeof(double));
  double *pvec = (double *)malloc_or_die(num_synergies * sizeof(double));
  double *avec = (double *)malloc_or_die(num_synergies * sizeof(double));
  // avec = A * coef (warm-start matvec), then res = rhs - avec.
  for (uint32_t s = 0; s < num_synergies; s++) {
    avec[s] = ridge * coef[s];
  }
  for (uint32_t r = 0; r < num_rows; r++) {
    double sp = 0.0;
    for (size_t k = row_off[r]; k < row_off[r + 1]; k++) {
      sp += coef[act[k]];
    }
    const double wsp = row_w[r] * sp;
    for (size_t k = row_off[r]; k < row_off[r + 1]; k++) {
      avec[act[k]] += wsp;
    }
  }
  double rs_old = 0.0;
  double rhs_norm2 = 0.0;
  for (uint32_t s = 0; s < num_synergies; s++) {
    res[s] = rhs[s] - avec[s];
    pvec[s] = res[s];
    rs_old += res[s] * res[s];
    rhs_norm2 += rhs[s] * rhs[s];
  }
  const double tol2 =
      CLM_CG_TOL * CLM_CG_TOL * (rhs_norm2 > 0.0 ? rhs_norm2 : 1.0);
  const uint32_t max_iters =
      num_synergies < CLM_CG_MAX_ITERS ? num_synergies : CLM_CG_MAX_ITERS;
  for (uint32_t it = 0; it < max_iters && rs_old > tol2; it++) {
    // avec = (Phi^T W Phi + ridge I) pvec
    for (uint32_t s = 0; s < num_synergies; s++) {
      avec[s] = ridge * pvec[s];
    }
    for (uint32_t r = 0; r < num_rows; r++) {
      double sp = 0.0;
      for (size_t k = row_off[r]; k < row_off[r + 1]; k++) {
        sp += pvec[act[k]];
      }
      const double wsp = row_w[r] * sp;
      for (size_t k = row_off[r]; k < row_off[r + 1]; k++) {
        avec[act[k]] += wsp;
      }
    }
    double pap = 0.0;
    for (uint32_t s = 0; s < num_synergies; s++) {
      pap += pvec[s] * avec[s];
    }
    if (pap <= 0.0) {
      break;
    }
    const double alpha = rs_old / pap;
    double rs_new = 0.0;
    for (uint32_t s = 0; s < num_synergies; s++) {
      coef[s] += alpha * pvec[s];
      res[s] -= alpha * avec[s];
      rs_new += res[s] * res[s];
    }
    const double beta_cg = rs_new / rs_old;
    for (uint32_t s = 0; s < num_synergies; s++) {
      pvec[s] = res[s] + beta_cg * pvec[s];
    }
    rs_old = rs_new;
  }
  free(rhs);
  free(res);
  free(pvec);
  free(avec);
  free(row_w);
  free(row_rhs);
  free(row_off);
  free(act);
}

// Decode a packed synergy key back into sorted machine letters.
static uint8_t unpack_key(uint64_t key, MachineLetter *tiles) {
  MachineLetter tmp[COMPACT_LEAVES_MAX_SYNERGY_TILES];
  uint8_t n = 0;
  while (key != 0 && n < COMPACT_LEAVES_MAX_SYNERGY_TILES) {
    tmp[n++] = (MachineLetter)((key & 0x3F) - 1);
    key >>= 6;
  }
  // tmp is high-to-low; the key was built ascending so reverse to sorted order.
  for (uint8_t i = 0; i < n; i++) {
    tiles[i] = tmp[n - 1 - i];
  }
  return n;
}

typedef struct ScoredCand {
  uint64_t key;
  double score; // weighted-error-reduction per byte
  double coef;  // points
  uint8_t ntiles;
} ScoredCand;

static int scored_cand_cmp(const void *a, const void *b) {
  const ScoredCand *x = (const ScoredCand *)a;
  const ScoredCand *y = (const ScoredCand *)b;
  if (x->score < y->score) {
    return 1;
  }
  if (x->score > y->score) {
    return -1;
  }
  return 0;
}

// Bit-width helpers for the optional bit-packed format (mirror the codec's
// zigzag + bits_needed so the maker can size the body in bits).
static int clm_bits_needed(uint32_t max_value) {
  int bits = 0;
  while (max_value > 0) {
    bits++;
    max_value >>= 1;
  }
  return bits < 1 ? 1 : bits;
}

static int clm_coef_bits(int32_t ticks) {
  const uint32_t sign = ticks < 0 ? 0xFFFFFFFFU : 0U;
  const uint32_t zigzag = ((uint32_t)ticks << 1) ^ sign;
  return clm_bits_needed(zigzag);
}

CompactLeaves *
compact_leaves_create_from_klv(const KLV *klv, const LetterDistribution *ld,
                               const uint64_t *weights, size_t target_bytes,
                               uint8_t radix_code, bool bit_packed) {
  const int A = ld_get_size(ld);
  const int radix_mp = compact_leaves_radix_millipoints(radix_code);
  const double synergy_ridge = 1.0;

  FitState st = {.klv = klv,
                 .ld = ld,
                 .weights = weights,
                 .dist_size = (uint8_t)A,
                 .vowel_bits = 0,
                 .leave = rack_create((uint16_t)A),
                 .base_p = 1 + 2 * A + (CLM_VC_CELLS - 1)};
  for (int ml = 1; ml < A; ml++) {
    if (ld_get_is_vowel(ld, ml)) {
      st.vowel_bits |= (uint64_t)1 << ml;
    }
  }
  const int p = st.base_p;
  Rack *bag = get_new_bag_as_rack(ld);

  // Pass 1: base Gram + r, then ridge + Cholesky solve.
  GramAcc gram_acc = {
      .gram = (double *)calloc_or_die((size_t)p * p, sizeof(double)),
      .rvec = (double *)calloc_or_die(p, sizeof(double)),
      .p = p};
  enum_leaves(&st, bag, 0, 0, cb_gram, &gram_acc);
  double diag_mean = 0.0;
  for (int i = 0; i < p; i++) {
    diag_mean += gram_acc.gram[i * p + i];
  }
  diag_mean /= p;
  const double ridge = diag_mean * 1e-4 + 1e-9;
  for (int i = 0; i < p; i++) {
    gram_acc.gram[i * p + i] += ridge;
  }
  double *beta = (double *)calloc_or_die(p, sizeof(double));
  cholesky_solve(gram_acc.gram, gram_acc.rvec, beta, p);

  // Quantize the base model into cl now, with no synergies yet, so that pass 2
  // and backfitting both fit synergies against the *quantized* base that eval
  // will actually use (keeps fit and eval consistent).
  CompactLeaves *cl = (CompactLeaves *)calloc_or_die(1, sizeof(CompactLeaves));
  cl->dist_size = (uint8_t)A;
  cl->radix_code = radix_code;
  cl->radix_millipoints = radix_mp;
  cl->flags = COMPACT_LEAVES_FLAG_HAS_DUP | COMPACT_LEAVES_FLAG_VC_TABLE;
  if (bit_packed) {
    cl->flags |= COMPACT_LEAVES_FLAG_BITPACKED;
  }
  cl->vowel_bits = st.vowel_bits;
  cl->base_ticks = quantize_ticks(beta[0], radix_mp);
  cl->tile_ticks = (int16_t *)calloc_or_die((size_t)A, sizeof(int16_t));
  cl->dup_ticks = (int16_t *)calloc_or_die((size_t)A, sizeof(int16_t));
  for (int ml = 0; ml < A; ml++) {
    cl->tile_ticks[ml] = quantize_ticks(beta[1 + ml], radix_mp);
    cl->dup_ticks[ml] = quantize_ticks(beta[1 + A + ml], radix_mp);
  }
  cl->vc_ticks[0] = 0; // pinned reference cell
  for (int cell = 1; cell < CLM_VC_CELLS; cell++) {
    cl->vc_ticks[cell] = quantize_ticks(beta[1 + 2 * A + (cell - 1)], radix_mp);
  }
  cl->num_synergies = 0;
  cl->synergies = NULL;

  // Pass 2: residuals vs the quantized base + candidate synergy stats.
  CandHash hash;
  cand_hash_create(&hash);
  ResAcc res_acc = {.cl = cl, .hash = &hash};
  enum_leaves(&st, bag, 0, 0, cb_residual, &res_acc);

  // Score candidates by marginal weighted-error reduction per byte.
  ScoredCand *scored =
      (ScoredCand *)malloc_or_die((size_t)hash.count * sizeof(ScoredCand));
  uint32_t num_scored = 0;
  for (uint32_t slot = 0; slot < CLM_HASH_SIZE; slot++) {
    if (hash.keys[slot] == 0 || hash.s2[slot] <= 0.0) {
      continue;
    }
    const double denom = hash.s2[slot] + synergy_ridge;
    const double coef = hash.s1[slot] / denom;
    const double gain = (hash.s1[slot] * hash.s1[slot]) / denom;
    const uint8_t nt = hash.ntiles[slot];
    const double bytes = 1.0 + nt + 2.0;
    scored[num_scored].key = hash.keys[slot];
    scored[num_scored].score = gain / bytes;
    scored[num_scored].coef = coef;
    scored[num_scored].ntiles = nt;
    num_scored++;
  }
  qsort(scored, num_scored, sizeof(ScoredCand), scored_cand_cmp);

  // Size accounting for synergy selection. Byte mode: a synergy costs
  // 1 (num_tiles) + ntiles + 2 (value) bytes against target_bytes, starting
  // from the fixed base block. Bit mode: synergies and coefficients are
  // sub-byte packed, so cost is in bits against a bit budget -- more fit.
  const size_t base_bytes = (size_t)COMPACT_LEAVES_HEADER_BYTES + 8 + 2 +
                            (size_t)A * 2 + (size_t)A * 2 + CLM_VC_CELLS * 2;
  int coef_bits = 0;
  int tile_bits = 0;
  int nt_bits = 0;
  if (bit_packed) {
    // coef_bits: fixed zigzag width holding every coefficient. Derive a bound
    // from the base coefficients and all candidate marginals so the jointly-fit
    // values (clamped to this width on emit) round-trip.
    coef_bits = clm_coef_bits(cl->base_ticks);
    for (int ml = 0; ml < A; ml++) {
      const int tb = clm_coef_bits(cl->tile_ticks[ml]);
      const int db = clm_coef_bits(cl->dup_ticks[ml]);
      coef_bits = tb > coef_bits ? tb : coef_bits;
      coef_bits = db > coef_bits ? db : coef_bits;
    }
    for (int cell = 0; cell < CLM_VC_CELLS; cell++) {
      const int vcb = clm_coef_bits(cl->vc_ticks[cell]);
      coef_bits = vcb > coef_bits ? vcb : coef_bits;
    }
    for (uint32_t i = 0; i < num_scored; i++) {
      const int scb = clm_coef_bits(quantize_ticks(scored[i].coef, radix_mp));
      coef_bits = scb > coef_bits ? scb : coef_bits;
    }
    tile_bits = clm_bits_needed((uint32_t)A - 1);
    nt_bits = clm_bits_needed(COMPACT_LEAVES_MAX_SYNERGY_TILES);
    cl->coef_bits = (uint8_t)coef_bits;
  }
  const size_t base_coef_count = 1 + (size_t)A + (size_t)A + CLM_VC_CELLS;
  const size_t prefix_bytes = (size_t)COMPACT_LEAVES_HEADER_BYTES + 8 + 1;
  const size_t used0 =
      bit_packed ? base_coef_count * (size_t)coef_bits : base_bytes;
  const size_t budget = bit_packed ? (target_bytes > prefix_bytes
                                          ? (target_bytes - prefix_bytes) * 8
                                          : 0)
                                   : target_bytes;

  // Greedy by score (already sorted desc); a smaller later term can still fit
  // after a larger earlier one was skipped. First count how many fit.
  size_t used = used0;
  uint32_t num_synergies = 0;
  for (uint32_t i = 0; i < num_scored; i++) {
    const size_t cost = bit_packed
                            ? (size_t)nt_bits +
                                  (size_t)scored[i].ntiles * (size_t)tile_bits +
                                  (size_t)coef_bits
                            : 1 + (size_t)scored[i].ntiles + 2;
    if (used + cost <= budget) {
      used += cost;
      num_synergies++;
    }
  }

  // Materialize the selected set: keys, warm-start coefs (marginal estimate),
  // and a key -> selected-index hash for the joint refit.
  const uint32_t sel_alloc = num_synergies > 0 ? num_synergies : 1;
  uint64_t *sel_key = (uint64_t *)malloc_or_die(sel_alloc * sizeof(uint64_t));
  double *sel_coef = (double *)calloc_or_die(sel_alloc, sizeof(double));
  SelHash sel;
  sel_hash_create(&sel, num_synergies);
  used = used0;
  uint32_t selected = 0;
  for (uint32_t i = 0; i < num_scored && selected < num_synergies; i++) {
    const size_t cost = bit_packed
                            ? (size_t)nt_bits +
                                  (size_t)scored[i].ntiles * (size_t)tile_bits +
                                  (size_t)coef_bits
                            : 1 + (size_t)scored[i].ntiles + 2;
    if (used + cost > budget) {
      continue;
    }
    used += cost;
    sel_key[selected] = scored[i].key;
    sel_coef[selected] = scored[i].coef;
    sel_hash_put(&sel, scored[i].key, (int32_t)selected);
    selected++;
  }
  num_synergies = selected;

  // Jointly refit the selected synergy coefficients so overlapping / nested
  // combos share the residual instead of each independently claiming it (the
  // marginal estimates above systematically overshoot when applied additively).
  if (num_synergies > 0) {
    solve_synergies_cg(&st, bag, cl, &sel, num_synergies, synergy_ridge,
                       sel_coef);
  }

  // Emit the jointly fit synergies into cl. In bit mode the value is clamped to
  // coef_bits so the serialized size matches the selection budget exactly.
  cl->synergies = (CompactLeavesSynergy *)calloc_or_die(
      sel_alloc, sizeof(CompactLeavesSynergy));
  const int32_t value_max = bit_packed ? (1 << (coef_bits - 1)) - 1 : 0;
  const int32_t value_min = bit_packed ? -(1 << (coef_bits - 1)) : 0;
  for (uint32_t s = 0; s < num_synergies; s++) {
    CompactLeavesSynergy *syn = &cl->synergies[s];
    syn->num_tiles = unpack_key(sel_key[s], syn->tiles);
    int32_t value = quantize_ticks(sel_coef[s], radix_mp);
    if (bit_packed) {
      value = value > value_max ? value_max
                                : (value < value_min ? value_min : value);
    }
    syn->value_ticks = (int16_t)value;
  }
  cl->num_synergies = num_synergies;

  free(sel_key);
  free(sel_coef);
  sel_hash_destroy(&sel);
  free(scored);
  cand_hash_destroy(&hash);
  free(beta);
  free(gram_acc.gram);
  free(gram_acc.rvec);
  rack_destroy(bag);
  rack_destroy(st.leave);
  return cl;
}

// ---- expand the model into a KLV (lossy) ----
typedef struct OverwriteAcc {
  const CompactLeaves *cl;
  KLV *klv;
} OverwriteAcc;
static void cb_overwrite(FitState *st, void *data) {
  const OverwriteAcc *acc = (const OverwriteAcc *)data;
  const uint32_t idx = klv_get_word_index(st->klv, st->leave);
  klv_set_indexed_leave_value(
      acc->klv, idx, compact_leaves_get_leave_value(acc->cl, st->leave));
}

void compact_leaves_overwrite_klv(const CompactLeaves *cl, KLV *klv,
                                  const LetterDistribution *ld) {
  const int A = ld_get_size(ld);
  FitState st = {.klv = klv,
                 .ld = ld,
                 .weights = NULL,
                 .dist_size = (uint8_t)A,
                 .vowel_bits = cl->vowel_bits,
                 .leave = rack_create((uint16_t)A),
                 .base_p = 0};
  Rack *bag = get_new_bag_as_rack(ld);
  OverwriteAcc acc = {.cl = cl, .klv = klv};
  enum_leaves(&st, bag, 0, 0, cb_overwrite, &acc);
  rack_destroy(bag);
  rack_destroy(st.leave);
}

// ---- RMS ----
typedef struct RmsAcc {
  const CompactLeaves *cl;
  const uint64_t *weights;
  double wsse;
  double wsum;
  double usse;
  double ucount;
} RmsAcc;
static void cb_rms(FitState *st, void *data) {
  RmsAcc *acc = (RmsAcc *)data;
  const uint32_t idx = klv_get_word_index(st->klv, st->leave);
  const double y = equity_to_double(klv_get_indexed_leave_value(st->klv, idx));
  const double f =
      equity_to_double(compact_leaves_get_leave_value(acc->cl, st->leave));
  const double d = f - y;
  acc->usse += d * d;
  acc->ucount += 1.0;
  if (acc->weights != NULL) {
    const double w = (double)acc->weights[idx];
    acc->wsse += w * d * d;
    acc->wsum += w;
  }
}

void compact_leaves_rms(const CompactLeaves *cl, const KLV *klv,
                        const LetterDistribution *ld, const uint64_t *weights,
                        double *weighted_rms_out, double *unweighted_rms_out) {
  const int A = ld_get_size(ld);
  FitState st = {.klv = klv,
                 .ld = ld,
                 .weights = weights,
                 .dist_size = (uint8_t)A,
                 .vowel_bits = cl->vowel_bits,
                 .leave = rack_create((uint16_t)A),
                 .base_p = 0};
  Rack *bag = get_new_bag_as_rack(ld);
  RmsAcc acc = {.cl = cl, .weights = weights, 0, 0, 0, 0};
  enum_leaves(&st, bag, 0, 0, cb_rms, &acc);
  rack_destroy(bag);
  rack_destroy(st.leave);
  if (unweighted_rms_out != NULL) {
    *unweighted_rms_out = acc.ucount > 0 ? sqrt(acc.usse / acc.ucount) : 0.0;
  }
  if (weighted_rms_out != NULL) {
    *weighted_rms_out = acc.wsum > 0 ? sqrt(acc.wsse / acc.wsum) : 0.0;
  }
}

uint64_t *compact_leaves_read_weights_csv(const KLV *klv,
                                          const LetterDistribution *ld,
                                          const char *csv_path,
                                          ErrorStack *error_stack) {
  FILE *file = fopen(csv_path, "r");
  if (file == NULL) {
    error_stack_push(error_stack, ERROR_STATUS_CONVERT_INPUT_FILE_ERROR,
                     get_formatted_string(
                         "could not open leave-frequency csv: %s", csv_path));
    return NULL;
  }
  const uint32_t num_leaves = klv_get_number_of_leaves(klv);
  uint64_t *weights = (uint64_t *)calloc_or_die(num_leaves, sizeof(uint64_t));
  Rack rack;
  rack_set_dist_size(&rack, ld_get_size(ld));
  ErrorStack *scratch = error_stack_create();
  char line[256];
  while (fgets(line, sizeof(line), file)) {
    const char *leave_str = strtok(line, ",");
    const char *count_str = strtok(NULL, "\n");
    if (leave_str == NULL || count_str == NULL) {
      continue;
    }
    const uint64_t count = string_to_uint64(count_str, scratch);
    if (!error_stack_is_empty(scratch) || count == 0) {
      error_stack_reset(scratch);
      continue;
    }
    if (rack_set_to_string(ld, &rack, leave_str) < 0) {
      continue;
    }
    const uint32_t idx = klv_get_word_index(klv, &rack);
    if (idx != KLV_UNFOUND_INDEX && idx < num_leaves) {
      weights[idx] = count;
    }
  }
  fclose(file);
  error_stack_destroy(scratch);
  return weights;
}
