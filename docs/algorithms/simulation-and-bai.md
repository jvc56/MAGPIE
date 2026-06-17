# Simulation & Best-Arm Identification

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/simmer.c`, `src/impl/bai.h`, `src/impl/random_variable.c`, `src/impl/bai_logger.h`
> **References (cited in `bai.h`):**
> Jourdan et al., *Dealing with Unknown Variances in Best-Arm Identification*
> ([arXiv:2210.00974](https://arxiv.org/pdf/2210.00974));
> *Information-Directed Selection for Top-Two Algorithms*
> ([arXiv:2205.12086](https://arxiv.org/pdf/2205.12086)).

## Monte Carlo rollouts

<!-- NOTE: each candidate move is an "arm". A rollout samples the opponent's
     unseen tiles and plays forward a few plies, scoring the outcome (win% and
     spread) against the win-pct strategy table. Aggregate over many rollouts. -->

## Why best-arm identification

<!-- NOTE: the goal isn't to estimate every move's value precisely — it's to
     identify the *best* move with confidence using as few samples as possible.
     Frame as a fixed-confidence BAI problem. Contrast with fixed-iteration
     Monte Carlo. -->

## The algorithm

<!-- NOTE: top-two sampling with information-directed selection; unknown-variance
     handling per Jourdan et al. Two phases: uniform initial sampling, then
     variance-aware sampling concentrated on the contenders until the stopping
     rule fires. Define the sample statistics tracked per arm (mean, variance). -->

The per-arm sample mean and variance drive the stopping rule; e.g. for arm $a$
with $n_a$ samples,

$$
\hat{\mu}_a = \frac{1}{n_a}\sum_{i=1}^{n_a} X_{a,i}, \qquad
\hat{\sigma}_a^2 = \frac{1}{n_a}\sum_{i=1}^{n_a} (X_{a,i}-\hat{\mu}_a)^2 .
$$

<!-- NOTE: state the actual stopping/threshold rule used (GLR-style), referencing
     the papers, once written. -->

## Parallelism

<!-- NOTE: how arms/rollouts are distributed across threads, the shared sync
     data (`BAISyncData`) under a mutex, and arm pruning (swap-and-shrink). -->

## Acknowledgement

<!-- NOTE: note that reference code for both papers was kindly provided by the
     authors (Marc Jourdan) and adapted. -->
