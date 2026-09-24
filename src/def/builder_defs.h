#ifndef BUILDER_DEFS_H
#define BUILDER_DEFS_H

// Versions of the builders that derive one data file from another.
//
// A wordmap and a rack info table are never shipped: every machine builds its
// own from files it already has. birdtest turns that into something it can
// check by building a reference copy on the server and sending its SHA-256 to
// workers, which build their own and refuse to use one whose bytes differ (see
// birdtest's MAGPIE_DEPENDENCY.md).
//
// That only works while a builder's output is a function of its inputs, and a
// builder's output is not a function of its inputs across MAGPIE versions. A
// CSW24 wordmap built in December 2025 and one built in September 2026 differ
// in 72,852,152 bytes while both carry wordmap format version 3: the builder
// changed and the format did not, because the format did not have to. So a
// hash means something only together with the builder that produced it, and
// these are that identifier.
//
// **Bump the version whenever a change alters what the builder writes**, with
// or without a format change. Nothing infers this from the code, so
// test/builder_hash_test.c pins the hash of a small lexicon's wordmap and rack
// info table: a change that alters either one fails that test until someone
// bumps the version here and updates the pinned hash. Without it, "bump when
// the output changes" would depend on somebody noticing.
//
// A format version bump implies a builder bump. The reverse does not hold,
// which is the entire reason these exist separately.
#define WMP_BUILDER_VERSION 1
#define RIT_BUILDER_VERSION 1

// The KLV builder is versioned alongside them because birdtest's server now
// builds every leave-generation KLV with `convert rackequity2klv` rather than
// with its own port of this code, and records which builder wrote each
// artifact. A rebuild under a later builder then reports "built by a different
// builder" instead of "differs", which would otherwise read as corruption.
#define KLV_BUILDER_VERSION 1

// The instruction-set target this binary was compiled for, from the Makefile.
// It travels with the builder versions because -march decides how the compiler
// may vectorize, and a vectorized floating-point reduction can associate
// differently from a scalar one. Two builds of one commit for two targets are
// therefore not automatically interchangeable, and a hash mismatch between a
// server and a worker on different targets should say so rather than look like
// a corrupt file.
//
// Measured, on x86-64 with GCC 10: `-march=native` (i7-10750H) and
// `-march=nehalem` produce byte-identical wordmaps and rack info tables, for
// both a two-letter test lexicon and NWL23. So this is recorded and reported,
// and deliberately *not* used to refuse work -- see contribute.c, where a
// worker whose target differs builds the file and compares the hash rather
// than declining unseen.
#ifndef MAGPIE_BUILD_TARGET
#define MAGPIE_BUILD_TARGET "unknown"
#endif

#endif
