#include <assert.h>

#include "../../src/ent/wmp.h"

void benchmark_csw_wmp(void) {
  printf("benchmark csw21 wmp lookups");
  WMP *wmp = wmp_create("testdata", "CSW21");
  assert(wmp != NULL);
  wmp_destroy(wmp);
}

void test_wmp(void) {
  benchmark_csw_wmp();
}