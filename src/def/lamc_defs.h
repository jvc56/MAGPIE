#ifndef LAMC_DEFS_H
#define LAMC_DEFS_H

// The kernel of the Gaussian process (gp.h) that Locally Adaptive Monte Carlo
// (lamc.h) fits.
typedef enum {
  GP_KERNEL_MATERN52,
  GP_KERNEL_SQUARED_EXPONENTIAL,
} gp_kernel_t;

// How Locally Adaptive Monte Carlo proposes the plays that replace those it
// does not retain from one round to the next.
typedef enum {
  // From each play it drops, a step up the gradient of the surrogate's mean,
  // then the nearest unevaluated play.
  LAMC_PROPOSAL_ASCENT,
  // The unevaluated plays with the highest upper confidence bound.
  LAMC_PROPOSAL_ACQUIRE,
} lamc_proposal_t;

#define LAMC_PROPOSAL_ASCENT_STRING "ascent"
#define LAMC_PROPOSAL_ACQUIRE_STRING "acquire"
#define GP_KERNEL_MATERN52_STRING "matern52"
#define GP_KERNEL_SQUARED_EXPONENTIAL_STRING "se"

enum {
  LAMC_DEFAULT_POOL_SIZE = 16,
  LAMC_DEFAULT_KEEP = 4,
  LAMC_DEFAULT_SEED_PLAYS = 4,
  LAMC_DEFAULT_SHALLOW_SAMPLES = 4,
  LAMC_DEFAULT_MAX_ROUNDS = 12,
  LAMC_DEFAULT_STABLE_ROUNDS = 3,
  LAMC_DEFAULT_MAX_UNIVERSE = 400,
  LAMC_DEFAULT_MAX_EVALUATED = 128,
  // The most plays retained from one round to the next.
  LAMC_MAX_KEEP = 64,
  // Bounds on the other settings.
  LAMC_MAX_POOL_SIZE = 256,
  LAMC_MAX_SHALLOW_SAMPLES = 1000,
  LAMC_MAX_ROUNDS = 1000,
  LAMC_MAX_UNIVERSE = 100000,
  LAMC_MAX_EVALUATED = 2048,
};

#define LAMC_DEFAULT_EXPLORE_SHARE 0.3
#define LAMC_DEFAULT_EQUITY_FLOOR 30.0
#define LAMC_DEFAULT_LENGTH_SCALE 1.5
#define LAMC_DEFAULT_TRUST_RADIUS 1.0
#define LAMC_DEFAULT_UCB_BETA 1.0

#endif
