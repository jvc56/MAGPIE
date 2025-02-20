
typedef struct BAIRVS BAIRVS;

typedef double (*bairv_sample_func_t)(void *);

BAIRVS *bai_rvs_create(void **data, int num_bairvs,
                       bairv_sample_func_t sample_func);
void bai_rvs_destroy(BAIRVS *bairvs);
double bai_rvs_sample(BAIRVS *bairvs, int k);