// Standalone Metal hello-world for MAGPIE phase-1 GPU bring-up.
// Verifies the toolchain works, that unified memory really is zero-copy,
// and measures dispatch round-trip latency.
//
// Build: make metal_hello
// Run:   ./bin/metal_hello

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static double mach_seconds(uint64_t ticks) {
  static mach_timebase_info_data_t tb = {0, 0};
  if (tb.denom == 0) {
    mach_timebase_info(&tb);
  }
  return (double)ticks * (double)tb.numer / (double)tb.denom / 1e9;
}

static NSString *metallib_path(const char *argv0) {
  NSString *exec = [[NSBundle mainBundle] executablePath];
  NSString *base = exec;
  if (base == nil) {
    base = [NSString stringWithUTF8String:argv0];
  }
  return [[base stringByDeletingLastPathComponent]
      stringByAppendingPathComponent:@"hello.metallib"];
}

int main(int argc, const char **argv) {
  (void)argc;
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      fprintf(stderr, "no Metal device\n");
      return 1;
    }
    printf("device:          %s\n", [[device name] UTF8String]);
    printf("unified memory:  %s\n", device.hasUnifiedMemory ? "yes" : "no");
    printf("max threads/tg:  %lu\n",
           (unsigned long)device.maxThreadsPerThreadgroup.width);

    NSString *libPath = metallib_path(argv[0]);
    NSError *err = nil;
    id<MTLLibrary> lib =
        [device newLibraryWithURL:[NSURL fileURLWithPath:libPath] error:&err];
    if (lib == nil) {
      fprintf(stderr, "failed to load %s: %s\n", [libPath UTF8String],
              err ? [[err localizedDescription] UTF8String] : "(no error)");
      return 1;
    }

    id<MTLFunction> vecAddFn = [lib newFunctionWithName:@"vec_add"];
    id<MTLFunction> noopFn = [lib newFunctionWithName:@"noop"];
    id<MTLComputePipelineState> vecAdd =
        [device newComputePipelineStateWithFunction:vecAddFn error:&err];
    id<MTLComputePipelineState> noop =
        [device newComputePipelineStateWithFunction:noopFn error:&err];
    if (vecAdd == nil || noop == nil) {
      fprintf(stderr, "pipeline creation failed: %s\n",
              err ? [[err localizedDescription] UTF8String] : "(no error)");
      return 1;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];

    // ---- Vector add correctness ----
    const NSUInteger N = 1u << 20; // 1M
    const size_t bytes = N * sizeof(float);
    id<MTLBuffer> bufA =
        [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> bufB =
        [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> bufC =
        [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    float *a = (float *)bufA.contents;
    float *b = (float *)bufB.contents;
    for (NSUInteger i = 0; i < N; i++) {
      a[i] = (float)i;
      b[i] = 2.0f * (float)i;
    }

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:vecAdd];
    [enc setBuffer:bufA offset:0 atIndex:0];
    [enc setBuffer:bufB offset:0 atIndex:1];
    [enc setBuffer:bufC offset:0 atIndex:2];
    NSUInteger tg = vecAdd.maxTotalThreadsPerThreadgroup;
    if (tg > 256) {
      tg = 256;
    }
    [enc dispatchThreads:MTLSizeMake(N, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    const float *c = (const float *)bufC.contents;
    int errors = 0;
    for (NSUInteger i = 0; i < N; i += (N / 16)) {
      const float expected = a[i] + b[i];
      if (c[i] != expected) {
        if (errors < 3) {
          fprintf(stderr, "mismatch at %lu: %f vs %f\n", (unsigned long)i,
                  (double)c[i], (double)expected);
        }
        errors++;
      }
    }
    printf("vec_add:         %s (N=%lu, sample errors=%d)\n",
           errors == 0 ? "ok" : "FAIL", (unsigned long)N, errors);

    // ---- Dispatch latency probe ----
    id<MTLBuffer> noopBuf =
        [device newBufferWithLength:sizeof(uint32_t)
                            options:MTLResourceStorageModeShared];

    for (int i = 0; i < 100; i++) {
      id<MTLCommandBuffer> cb2 = [queue commandBuffer];
      id<MTLComputeCommandEncoder> e2 = [cb2 computeCommandEncoder];
      [e2 setComputePipelineState:noop];
      [e2 setBuffer:noopBuf offset:0 atIndex:0];
      [e2 dispatchThreads:MTLSizeMake(1, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
      [e2 endEncoding];
      [cb2 commit];
      [cb2 waitUntilCompleted];
    }

    const int N_DISPATCH = 2000;
    const uint64_t t0 = mach_absolute_time();
    for (int i = 0; i < N_DISPATCH; i++) {
      id<MTLCommandBuffer> cb2 = [queue commandBuffer];
      id<MTLComputeCommandEncoder> e2 = [cb2 computeCommandEncoder];
      [e2 setComputePipelineState:noop];
      [e2 setBuffer:noopBuf offset:0 atIndex:0];
      [e2 dispatchThreads:MTLSizeMake(1, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
      [e2 endEncoding];
      [cb2 commit];
      [cb2 waitUntilCompleted];
    }
    const double dt = mach_seconds(mach_absolute_time() - t0);
    printf("noop round-trip: %.1f us avg over %d (total %.3fs => %.0f /s)\n",
           1e6 * dt / N_DISPATCH, N_DISPATCH, dt, (double)N_DISPATCH / dt);

    return errors == 0 ? 0 : 1;
  }
}
