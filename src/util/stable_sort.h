#ifndef STABLE_SORT_H
#define STABLE_SORT_H

#include <stddef.h>

// Sorts like qsort, but stably: elements that compare equal keep their
// relative order. The C standard leaves the order of equal elements under
// qsort unspecified, and C libraries differ (glibc merge-sorts, UCRT and
// macOS do not), so any qsort whose comparator can tie on distinguishable
// elements produces output that depends on the platform. Use this instead
// wherever that output is visible: derived files, search order, display.
//
// The algorithm is glibc's qsort merge sort, step for step, so on Linux the
// result is identical to what qsort produced before, even for a comparator
// that is not transitive.
void stable_sort(void *base, size_t count, size_t size,
                 int (*compare)(const void *, const void *));

#endif
