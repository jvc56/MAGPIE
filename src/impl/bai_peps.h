

typedef struct GLRTVars {
  double *vals;
  double *θs;
  int k;
  double *λ;
  double *ϕ2;
  // FIXME: determine if these are needed
  int astar;
  double *μ;
  double *σ2;
} GLRTVars;

GLRTVars *bai_glrt_vars_create(int K);
void bai_glrt_vars_destroy(GLRTVars *glrt_vars);
void bai_glrt(int K, int *w, double *μ, double *σ2, GLRTVars *glrt_vars);
double *bai_oracle(double *ξ, double *ϕ2);