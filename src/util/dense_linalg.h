#ifndef DENSE_LINALG_H
#define DENSE_LINALG_H

#include <stdbool.h>

// Small dense linear algebra for the Gaussian process (gp.h). Matrices are
// row-major n x n arrays of doubles.

// Factors the symmetric positive definite matrix a into L L^T in place: on
// success the lower triangle of a holds L and the upper triangle is zeroed.
// Returns false, leaving a partly overwritten, when a is not positive
// definite to working precision.
bool dense_cholesky(double *a, int n);

// Solves L y = b for y, with L lower triangular from dense_cholesky. b and y
// may be the same array.
void dense_forward_solve(const double *lower, int n, const double *b,
                         double *y);

// Solves L^T x = y for x. y and x may be the same array.
void dense_backward_solve(const double *lower, int n, const double *y,
                          double *x);

#endif
