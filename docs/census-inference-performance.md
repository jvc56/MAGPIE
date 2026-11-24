# Census inference integration — 2026-11-02

Ports the inference-specific changes from [draft PR #939](https://github.com/jvc56/MAGPIE/pull/939),
commit `d4061269`, into softmax inference. This includes threaded census play-score
construction, incremental rack-split ranks and KLV lookup, batched rack claims,
and the portable atomic maximum helper. It excludes the experimental census
simulator, expected-best apportionment, and changed-lane reuse APIs.

Census is enabled for at least five unknown tiles on qualifying classic boards. It builds board play scores once per inference and evaluates each
candidate rack through its played/kept subsets. Candidate racks still contain
the observed played tiles and any explicitly known target tiles. The table can
contain other playable multisets; each candidate only uses subsets of itself.
The additional opening builder enumerates words and legal placements, including
vowel placement penalties before maximizing each multiset's value. Assigned
blanks retain their letter's vowel identity; their face-score reduction is
handled by the existing blank-variant calculation. The raw-score census API used
by leave generation and the nonopening generator's hot path are unchanged
by this opening extension.
Exchanges also benefit from incremental KLV traversal when census is disabled.

For softmax, the census path exhausts all splits of admitted racks to obtain the
exact best equity. It can stop early only when the candidate will be rejected.
The empty-bag admission exception retains a full search. No scoring or posterior
model changes are intended.

## Controlled timing

Sequential optimized runs on the same machine, with no concurrent test/build
jobs, using `infbench:4:20:20261102:exchanges`: four game pairs per policy,
24 games total, six exchanges (three on empty boards and three on nonempty
boards), and ten inference configurations per exchange. The prior is scored
separately. Player policies and simulation budgets match [infbench](infbench.md).

| Build | Whole run | Opening inference | Nonopening inference |
|---|---:|---:|---:|
| Before port (`026a401f`) | 140.826 s | 111.629 s | 23.903 s |
| Port, census disabled | 132.324 s | 102.934 s | 23.817 s |
| Port, census enabled including openings | 26.424 s | 20.123 s | 0.821 s |

The inference columns sum the measured inference/reconstruction/scoring calls,
excluding the cheap prior construction. They do not include game generation.
Census makes nonopening inference about **29× faster** and opening inference
about **5.5× faster** than the original path in this sample. The entire run is
**81.2% shorter (5.3× faster)**. With census disabled, the measured nonopening
cost is essentially unchanged and the opening cost is lower, consistent with
the incremental exchange-leave lookup improvement. The disabled control was
measured before adding the opening builder, which only runs when census is
enabled. This is one sequential timing sample, not a confidence interval for
performance.

All kept-leave and discarded-tile decision metrics match across the three runs
within `1e-8` (largest printed difference approximately `1e-9`). The disabled
control uses `infbench:4:20:20261102:exchanges:movegen`.

## Full exchange replay

The final opening-enabled binary completed `infbench:20:20:20261102:exchanges`
(120 games, the same 20 exchange observations) in **94.737 seconds**. Measured
inference calls totaled 46.421 seconds on the six opening exchanges and 24.240
seconds on the 14 nonopening exchanges. The remaining approximately 24 seconds
covers game generation, simulation, and other overhead.

All 440 kept/discarded decision records (20 observations × 11 modes × two
prediction targets) match the original run within `1e-8`; the largest printed
metric difference is approximately `1e-9`. The original run took about 651
seconds, but that historical wall time was not collected under the same isolated
conditions as the controlled comparison above. Use the controlled 5.3× result
for the direct speed comparison.

## Validation

- Census/movegen parity on 24 qualifying CSW24 self-play positions (including empty boards), including
  four exchanges, independently for hard-margin and temperature-2 inference.
  Draw counts, weighted statistics, and complete alias tables match; alias
  entries and probabilities are exactly equal.
- WMP and KWG cutoff comparisons each cover 11 games, 204 scoring plays, and
  both exchange categories.
- Opening adjusted-score tables match ordinary move generation on 40 racks,
  including a two-blank rack and a vowel-heavy rack. Threaded opening tables
  exactly match serial tables across four work-partition sizes.
- Optimized and 15×15 sanitizer suites: `census compat infer infbenchmetrics`.
- 21×21 sanitizer suites: `census compat infer`.
- Formatting, cppcheck (including Windows analysis), portability, and project
  dependency checks pass. Native clang-tidy is clean for changed code; macOS
  include-cleaner reports three existing system-header warnings in unchanged
  `test_util.c` code despite the required headers already being included.
  The newer math-parentheses check, absent from CI's clang-tidy 18, was excluded.
- Windows cross-compilation is left to CI because its compiler is not installed
  locally.
