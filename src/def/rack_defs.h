#ifndef RACK_DEFS_H
#define RACK_DEFS_H

// This should be defined in the Makefile
// but is conditionally defined here
// as a fallback and so the IDE doesn't
// complain about a missing definition.
#ifndef RACK_SIZE
#define RACK_SIZE 7
#endif

enum {
  // The rack size the shipped data files (KLV, WMP, RIT) and most of the
  // test suite assume. Builds with another RACK_SIZE run a reduced,
  // data-independent test set; see test/test.c.
  DEFAULT_RACK_SIZE = 7,
  MAX_RACK_SIZE = 100,
  WORD_ALIGNING_RACK_SIZE = (((RACK_SIZE) + 7) & ~7)
};

#endif