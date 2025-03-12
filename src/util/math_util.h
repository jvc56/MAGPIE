#ifndef MATH_UTIL_H
#define MATH_UTIL_H

#include <stdbool.h>

double p_to_z(double p);
bool is_z_valid(double zval);
double odds_that_player_is_better(double sampled_win_pct, int total_games);
double zeta(const double s);
double lambertw(const double x, const int k, const double precision);

#endif
