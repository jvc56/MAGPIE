# Fresh PEG calibration corpora (2026-07-30)

These two panels were frozen before any PEG arm was run. Selection used only
source-game identity, event/turn order, and exact bag size. It did not inspect
PEG moves or values, disagreement, oracle outcomes, or elapsed time. Each
retained position comes from a different source game.

## Common generation protocol

- Build: `no_pgo_release`
- Lexicon/data: CSW24, `-rit true -ritmmap true`
- Hardware concurrency: 18 threads, detected with
  `getconf _NPROCESSORS_ONLN`
- Autoplay: deterministic short-sim equity play (`-pl 2`, `-np 5`,
  `-i 100`, `-mi 10`) with identical players
- Sampling: at most the first valid position for each exact bag size 1--4
  from a source game; a capacity matching then assigns at most one retained
  position to each source game
- Raw GCGs and extraction logs: `obj/peg_admission_20260730/` and
  `obj/peg_quality_20260730/` (ignored by git)

The exact commands, raw-input hashes, binary hashes, source-game hashes,
position CGP hashes, and final panel hash are recorded in each manifest.

## Admission/completion-tail panel

- Panel:
  `tools/peg_time_calibration/admission_positions_20260730.tsv`
- Manifest:
  `tools/peg_time_calibration/admission_positions_20260730.manifest.json`
- New seed family: `730410001` plus replacement seed `730410002`
- Generated source games: 2,000
- Source games yielding at least one valid exact-bag position: 1,722
- Raw candidate counts by bag 1/2/3/4: 620 / 521 / 549 / 487
- Frozen positions: 1,280 (320 per bag), all from independent source games
- Panel SHA-256:
  `fc7a5f64e4c3b02ed9a36dd06486383fe78bc291fb79005c0aa5e196ce666758`

The replacement batch was generated because the original 1,600 games had
enough raw rows per bag but not a feasible 1,280-source balanced matching.
No independence or bag-balance requirement was relaxed.

With no censoring, observing zero exceedances beyond the full-sample maximum
has coverage `1 - 0.99^320 = 95.99%` for a population p99 within each bag.
This does not by itself make a p99 deadline-safe; the analysis will report
the observed p99 and maximum separately and audit held-out false starts.

## Fresh quality panel

- Panel:
  `tools/peg_time_calibration/quality_positions_20260730.tsv`
- Manifest:
  `tools/peg_time_calibration/quality_positions_20260730.manifest.json`
- Distinct seed family: `730420001`
- Generated source games: 800
- Source games yielding at least one valid exact-bag position: 690
- Raw candidate counts by bag 1/2/3/4: 254 / 209 / 214 / 186
- Frozen positions: 200 (50 per bag), all from independent source games
- Panel SHA-256:
  `cac6348b3b6bef27d0886ee76286a0adef4d5bea549aa46599750e55695cfd17`

The panel is representative of the naturalistic exact-bag stream and is not
filtered by seed win percentage or by whether any policies disagree.
