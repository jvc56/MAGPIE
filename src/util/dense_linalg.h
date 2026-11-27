#ifndef DENSE_LINALG_H
#define DENSE_LINALG_H

#include <stdbool.h>

// Small dense linear algebra for the Gaussian process (gp.h). Matrices are
// row-major dim x dim arrays of doubles.

// Factors the symmetric positive definite matrix into L L^T in place: on
// success the lower triangle of matrix holds L and the upper triangle is
// zeroed. Returns false, leaving matrix partly overwritten, when it is not
// positive definite to working precision.
bool dense_cholesky(double *matrix, int dim);

// Solves L intermediate = rhs for intermediate, with L (lower) lower
// triangular from dense_cholesky. rhs and intermediate may be the same array.
void dense_forward_solve(const double *lower, int dim, const double *rhs,
                         double *intermediate);

// Solves L^T solution = intermediate for solution. intermediate and solution
// may be the same array.
void dense_backward_solve(const double *lower, int dim,
                          const double *intermediate, double *solution);

#endif
