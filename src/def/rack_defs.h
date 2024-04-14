#ifndef RACK_DEFS_H
#define RACK_DEFS_H

#define DEFAULT_RACK_SIZE 7

// This should be defined in the Makefile
// but is conditionally defined here
// as a fallback and so the IDE doesn't
// complain about a missing definition.
#ifndef RACK_SIZE
#define RACK_SIZE DEFAULT_RACK_SIZE
#endif

#define ROUND_UP_TO_NEXT_MULTIPLE_OF_8(x) (((x) + 7) & ~7)
#define WORD_ALIGNING_RACK_SIZE ROUND_UP_TO_NEXT_MULTIPLE_OF_8(RACK_SIZE)
#define MAX_RACK_SIZE 10000

#endif