#ifndef MATH_UTIL_H
#define MATH_UTIL_H

#include <complex.h>
#include <stdbool.h>

double odds_that_player_is_better(double sampled_win_pct, int total_games);
double zeta(const double s);
double lambertw(const double x, const int k);
bool cubic_roots(double a, double b, double c, double d, complex double *roots);

#endif
