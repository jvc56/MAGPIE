#ifndef PEG_COMBINATORICS_H
#define PEG_COMBINATORICS_H

#include "../def/letter_distribution_defs.h"
#include <stdbool.h>
#include <stdint.h>

// Small combinatorial helpers shared by the PEG solver and its reference
// evaluators: counting and enumerating the ways a bag's tiles can be split and
// ordered. Kept in one place so the production solver and the test harnesses
// stay in lockstep.

// Binomial coefficient C(n, k), or 0 when k is outside [0, n]. Computed
// iteratively (dividing as it goes) so it stays exact for the small n that PEG
// scenario counting uses.
static inline int64_t peg_binomial(int n, int k) {
  if (k < 0 || k > n) {
    return 0;
  }
  if (k == 0 || k == n) {
    return 1;
  }
  if (k > n - k) {
    k = n - k;
  }
  int64_t result = 1;
  for (int i = 0; i < k; i++) {
    result = result * (n - i) / (i + 1);
  }
  return result;
}

// Advance arr[0..n-1] to the next lexicographic permutation in place, visiting
// only distinct orderings (duplicate tiles are skipped). The caller sorts the
// array ascending before the first call; returns false once the final
// permutation has been produced.
static inline bool peg_next_perm(MachineLetter *arr, int n) {
  if (n <= 1) {
    return false;
  }
  int pivot = n - 2;
  while (pivot >= 0 && arr[pivot] >= arr[pivot + 1]) {
    pivot--;
  }
  if (pivot < 0) {
    return false;
  }
  int swap_idx = n - 1;
  while (arr[pivot] >= arr[swap_idx]) {
    swap_idx--;
  }
  MachineLetter tmp = arr[pivot];
  arr[pivot] = arr[swap_idx];
  arr[swap_idx] = tmp;
  int lo = pivot + 1;
  int hi = n - 1;
  while (lo < hi) {
    tmp = arr[lo];
    arr[lo] = arr[hi];
    arr[hi] = tmp;
    lo++;
    hi--;
  }
  return true;
}

#endif // PEG_COMBINATORICS_H
