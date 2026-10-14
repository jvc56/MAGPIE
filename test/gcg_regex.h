#ifndef GCG_REGEX_H
#define GCG_REGEX_H

// POSIX <regex.h> for the GCG tokenizer differential test, where the platform
// has it. Windows does not, and there GCG_REGEX_AVAILABLE stays undefined.

#if defined(__has_include)
#if __has_include(<regex.h>)
#include <regex.h> // IWYU pragma: export
#define GCG_REGEX_AVAILABLE 1
#endif
#endif

#endif
