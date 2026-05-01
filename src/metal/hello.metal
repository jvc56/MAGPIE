// Phase-1 hello-world kernels. Two purposes:
//   vec_add — correctness check that shared/unified memory works end-to-end.
//   noop    — measures Metal command-buffer dispatch round-trip latency,
//             which is the empirical floor for any per-position GPU movegen.

#include <metal_stdlib>
using namespace metal;

kernel void vec_add(device const float *a [[buffer(0)]],
                    device const float *b [[buffer(1)]],
                    device float *c       [[buffer(2)]],
                    uint gid              [[thread_position_in_grid]]) {
  c[gid] = a[gid] + b[gid];
}

kernel void noop(device uint *out [[buffer(0)]],
                 uint gid         [[thread_position_in_grid]]) {
  if (gid == 0) {
    out[0] = 1;
  }
}
