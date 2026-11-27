#include "dense_linalg.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

bool dense_cholesky(double *matrix, const int dim) {
  for (int col = 0; col < dim; col++) {
    double diagonal = matrix[(ptrdiff_t)col * dim + col];
    for (int inner = 0; inner < col; inner++) {
      const double value = matrix[(ptrdiff_t)col * dim + inner];
      diagonal -= value * value;
    }
    if (!(diagonal > 0.0)) {
      return false;
    }
    const double pivot = sqrt(diagonal);
    matrix[(ptrdiff_t)col * dim + col] = pivot;
    for (int row = col + 1; row < dim; row++) {
      double sum = matrix[(ptrdiff_t)row * dim + col];
      for (int inner = 0; inner < col; inner++) {
        sum -= matrix[(ptrdiff_t)row * dim + inner] *
               matrix[(ptrdiff_t)col * dim + inner];
      }
      matrix[(ptrdiff_t)row * dim + col] = sum / pivot;
    }
    for (int row = 0; row < col; row++) {
      matrix[(ptrdiff_t)row * dim + col] = 0.0;
    }
  }
  return true;
}

void dense_forward_solve(const double *lower, const int dim, const double *rhs,
                         double *intermediate) {
  for (int row = 0; row < dim; row++) {
    double sum = rhs[row];
    for (int col = 0; col < row; col++) {
      sum -= lower[(ptrdiff_t)row * dim + col] * intermediate[col];
    }
    intermediate[row] = sum / lower[(ptrdiff_t)row * dim + row];
  }
}

void dense_backward_solve(const double *lower, const int dim,
                          const double *intermediate, double *solution) {
  for (int row = dim - 1; row >= 0; row--) {
    double sum = intermediate[row];
    for (int col = row + 1; col < dim; col++) {
      sum -= lower[(ptrdiff_t)col * dim + row] * solution[col];
    }
    solution[row] = sum / lower[(ptrdiff_t)row * dim + row];
  }
}
