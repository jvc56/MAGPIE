#include "bai_rvs.h"

struct BAIRVS {
  void **data;
  int num_bairvs;
  bairv_sample_func_t sample_func;
};

// The creation of best arm identification random variables
// does not take ownership of the data. Free'ing the data
// is the responsibility of the caller.
BAIRVS *bai_rvs_create(void **data, int num_bairvs,
                       bairv_sample_func_t sample_func) {
  BAIRVS *bairvs = malloc_or_die(sizeof(BAIRVS));
  bairvs->data = data;
  bairvs->num_bairvs = num_bairvs;
  bairvs->sample_func = sample_func;
  return bairvs;
}

void bai_rvs_destroy(BAIRVS *bairvs) { free(bairvs->data); }

double bai_rvs_sample(BAIRVS *bairvs, int k) {
  return bairvs->sample_func(bairvs->data[k]);
}