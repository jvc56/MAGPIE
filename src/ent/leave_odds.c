#include "leave_odds.h"

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "equity.h"
#include "klv.h"
#include "rack.h"
#include "xoshiro.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

static double leave_odds_choose(int n, int k) {
  if (k < 0 || k > n) {
    return 0.0;
  }
  double result = 1.0;
  for (int idx = 1; idx <= k; idx++) {
    result = result * (double)(n - k + idx) / (double)idx;
  }
  return result;
}

bool leave_odds_prepare(LeaveOdds *odds, const int *unseen, const float *theta,
                        int letters, int size) {
  memset(odds, 0, sizeof(*odds));
  int total = 0;
  for (int letter = 0; letter < letters; letter++) {
    total += unseen[letter];
  }
  if (letters < 1 || letters > MAX_ALPHABET_SIZE || size < 1 ||
      size > RACK_SIZE || size > total) {
    return false;
  }
  odds->letters = letters;
  odds->size = size;
  odds->pool = NULL;
  odds->pool_cumulative = NULL;
  odds->pool_size = 0;
  for (int letter = 0; letter < letters; letter++) {
    odds->unseen[letter] = unseen[letter];
    for (int count = 0; count <= size; count++) {
      odds->weight[letter][count] = leave_odds_choose(unseen[letter], count) *
                                    exp((double)theta[letter] * count);
    }
  }
  odds->suffix[letters][0] = 1.0;
  for (int letter = letters - 1; letter >= 0; letter--) {
    for (int remaining = 0; remaining <= size; remaining++) {
      double sum = 0.0;
      for (int count = 0; count <= remaining; count++) {
        sum += odds->weight[letter][count] *
               odds->suffix[letter + 1][remaining - count];
      }
      odds->suffix[letter][remaining] = sum;
    }
  }
  return odds->suffix[0][size] > 0.0;
}

double leave_odds_probability(const LeaveOdds *odds, const int *counts) {
  double weight = 1.0;
  int size = 0;
  for (int letter = 0; letter < odds->letters; letter++) {
    if (counts[letter] > odds->size) {
      return 0.0;
    }
    weight *= odds->weight[letter][counts[letter]];
    size += counts[letter];
  }
  return size == odds->size ? weight / odds->suffix[0][odds->size] : 0.0;
}

void leave_odds_expected_counts(const LeaveOdds *odds, double *mean) {
  // prefix[l][r]: the total weight of taking r tiles from the types before l.
  double prefix[MAX_ALPHABET_SIZE + 1][RACK_SIZE + 1];
  memset(prefix, 0, sizeof(prefix));
  prefix[0][0] = 1.0;
  for (int letter = 0; letter < odds->letters; letter++) {
    for (int taken = 0; taken <= odds->size; taken++) {
      double sum = 0.0;
      for (int count = 0; count <= taken; count++) {
        sum += prefix[letter][taken - count] * odds->weight[letter][count];
      }
      prefix[letter + 1][taken] = sum;
    }
  }
  const double total = odds->suffix[0][odds->size];
  for (int letter = 0; letter < odds->letters; letter++) {
    double sum = 0.0;
    for (int count = 1; count <= odds->size; count++) {
      for (int before = 0; before + count <= odds->size; before++) {
        sum += count * prefix[letter][before] * odds->weight[letter][count] *
               odds->suffix[letter + 1][odds->size - before - count];
      }
    }
    mean[letter] = sum / total;
  }
}

// Draws a leave from the Fisher distribution itself.
static void leave_odds_sample_exact(const LeaveOdds *odds, XoshiroPRNG *prng,
                                    Rack *leave) {
  rack_set_dist_size_and_reset(leave, odds->letters);
  int remaining = odds->size;
  for (int letter = 0; letter < odds->letters && remaining > 0; letter++) {
    const double total = odds->suffix[letter][remaining];
    double draw = (double)prng_get_random_number(prng, XOSHIRO_MAX) /
                  (double)XOSHIRO_MAX * total;
    int chosen = 0;
    for (int count = 0; count <= remaining; count++) {
      const double weight = odds->weight[letter][count] *
                            odds->suffix[letter + 1][remaining - count];
      if (weight <= 0.0) {
        continue;
      }
      chosen = count;
      if (draw < weight) {
        break;
      }
      draw -= weight;
    }
    for (int copy = 0; copy < chosen; copy++) {
      rack_add_letter(leave, (MachineLetter)letter);
    }
    remaining -= chosen;
  }
}

void leave_odds_sample(const LeaveOdds *odds, XoshiroPRNG *prng, Rack *leave) {
  if (odds->pool_size <= 0) {
    leave_odds_sample_exact(odds, prng, leave);
    return;
  }
  const double total = odds->pool_cumulative[odds->pool_size - 1];
  const double draw = (double)prng_get_random_number(prng, XOSHIRO_MAX) /
                      (double)XOSHIRO_MAX * total;
  int low = 0;
  int high = odds->pool_size - 1;
  while (low < high) {
    const int middle = (low + high) / 2;
    if (odds->pool_cumulative[middle] > draw) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }
  rack_copy(leave, &odds->pool[low]);
}

void leave_odds_reweight(LeaveOdds *odds, XoshiroPRNG *prng, const KLV *klv,
                         double beta, Rack *pool, double *cumulative,
                         int pool_size) {
  odds->pool_size = 0;
  if (pool_size <= 0 || klv == NULL) {
    return;
  }
  // cumulative holds each leave's log weight first, then the running sums.
  double top = -INFINITY;
  for (int idx = 0; idx < pool_size; idx++) {
    leave_odds_sample_exact(odds, prng, &pool[idx]);
    cumulative[idx] =
        beta * equity_to_double(klv_get_leave_value(klv, &pool[idx]));
    if (cumulative[idx] > top) {
      top = cumulative[idx];
    }
  }
  double sum = 0.0;
  for (int idx = 0; idx < pool_size; idx++) {
    sum += exp(cumulative[idx] - top);
    cumulative[idx] = sum;
  }
  odds->pool = pool;
  odds->pool_cumulative = cumulative;
  odds->pool_size = pool_size;
}
