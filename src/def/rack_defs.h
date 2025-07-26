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
  MAX_RACK_SIZE = 10000,
  WORD_ALIGNING_RACK_SIZE = (((RACK_SIZE) + 7) & ~7)
};

#endif