#ifndef CCACHE_H
#define CCACHE_H

// The size of a CPU cache line in bytes: 128 on Apple silicon, 64 on every
// other target MAGPIE builds for.
#if defined(__APPLE__) && defined(__aarch64__)
#define CCACHE_LINE_BYTES 128
#else
#define CCACHE_LINE_BYTES 64
#endif

#endif
