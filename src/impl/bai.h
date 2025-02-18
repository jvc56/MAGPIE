typedef enum {
  BAI_THRESHOLD_HT,
} bai_threshold_t;

typedef double (*bairv_sample_func_t)(XoshiroPRNG *prng);

typedef struct BAIRV {
  void *data;
  bairv_sample_func_t sample_func;
} BAIRV;