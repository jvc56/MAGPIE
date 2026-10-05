#ifndef LEAVE_ODDS_H
#define LEAVE_ODDS_H

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "klv.h"
#include "rack.h"
#include "xoshiro.h"
#include <stdbool.h>

// A distribution over the leave an opponent kept: size tiles drawn without
// replacement from the unseen tiles, a tile of type l carrying odds
// exp(theta[l]) against the others (Fisher's noncentral multivariate
// hypergeometric). A leave with n[l] tiles of each type has probability
// proportional to prod over l of C(unseen[l], n[l]) * exp(theta[l] * n[l]).
// theta = 0 is the uniform draw.
typedef struct LeaveOdds {
  int letters;
  int size;
  int unseen[MAX_ALPHABET_SIZE];
  // weight[l][n] = C(unseen[l], n) * exp(theta[l] * n), and suffix[l][r] the
  // total weight of every way to take r tiles from the types l onward.
  double weight[MAX_ALPHABET_SIZE][RACK_SIZE + 1];
  double suffix[MAX_ALPHABET_SIZE + 1][RACK_SIZE + 1];
  // With pool_size > 0 (leave_odds_reweight), leave_odds_sample draws one of
  // the pool's leaves instead, leave i with probability proportional to
  // pool_cumulative[i] - pool_cumulative[i - 1]. Not owned.
  const Rack *pool;
  const double *pool_cumulative;
  int pool_size;
} LeaveOdds;

// Prepares odds for leaves of size tiles from unseen (letters counts by
// machine letter) with log odds theta (letters values). Returns false when
// no such leave exists (size out of range or too few unseen tiles).
bool leave_odds_prepare(LeaveOdds *odds, const int *unseen, const float *theta,
                        int letters, int size);

// The probability of the leave with counts (letters values).
double leave_odds_probability(const LeaveOdds *odds, const int *counts);

// Expected count of each tile type in a leave (letters values into mean).
void leave_odds_expected_counts(const LeaveOdds *odds, double *mean);

// Draws a leave into leave (reset first, with odds->letters as its
// distribution size).
void leave_odds_sample(const LeaveOdds *odds, XoshiroPRNG *prng, Rack *leave);

// Tilts odds' distribution by exp(beta * KLV(L)) (the leave's value in
// points from klv; Macondo's value term for long leaves) by
// sampling-importance-resampling: draws pool_size leaves from odds into
// pool, weights each by exp(beta * value), and has leave_odds_sample draw
// from the pool by weight (cumulative receives the running sums; both
// pool_size long and kept by the caller). The pool must outlive the odds'
// use.
void leave_odds_reweight(LeaveOdds *odds, XoshiroPRNG *prng, const KLV *klv,
                         double beta, Rack *pool, double *cumulative,
                         int pool_size);

#endif
