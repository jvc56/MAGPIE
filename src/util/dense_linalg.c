#include "dense_linalg.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

bool dense_cholesky(double *a, const int n) {
  for (int col = 0; col < n; col++) {
    double diagonal = a[(ptrdiff_t)col * n + col];
    for (int inner = 0; inner < col; inner++) {
      const double value = a[(ptrdiff_t)col * n + inner];
      diagonal -= value * value;
    }
    if (!(diagonal > 0.0)) {
      return false;
    }
    const double pivot = sqrt(diagonal);
    a[(ptrdiff_t)col * n + col] = pivot;
    for (int row = col + 1; row < n; row++) {
      double sum = a[(ptrdiff_t)row * n + col];
      for (int inner = 0; inner < col; inner++) {
        sum -= a[(ptrdiff_t)row * n + inner] * a[(ptrdiff_t)col * n + inner];
      }
      a[(ptrdiff_t)row * n + col] = sum / pivot;
    }
    for (int row = 0; row < col; row++) {
      a[(ptrdiff_t)row * n + col] = 0.0;
    }
  }
  return true;
}

void dense_forward_solve(const double *lower, const int n, const double *b,
                         double *y) {
  for (int row = 0; row < n; row++) {
    double sum = b[row];
    for (int col = 0; col < row; col++) {
      sum -= lower[(ptrdiff_t)row * n + col] * y[col];
    }
    y[row] = sum / lower[(ptrdiff_t)row * n + row];
  }
}

void dense_backward_solve(const double *lower, const int n, const double *y,
                          double *x) {
  for (int row = n - 1; row >= 0; row--) {
    double sum = y[row];
    for (int col = row + 1; col < n; col++) {
      sum -= lower[(ptrdiff_t)col * n + row] * x[col];
    }
    x[row] = sum / lower[(ptrdiff_t)row * n + row];
  }
}
