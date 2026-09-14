# Building a PAT champion file for a lexicon (the v5 recipe)

pat_dls_champion_v5 (CSW21) is v4 plus an opening table; v4 is v3 plus
the run-keyed floater through table and a 14-weight residual refit; v3
is the iterative recipe below with the corrected floater semantics.
Every step is a magpie command; run them from a repository root with
`bin/magpie` (`make BUILD=no_pgo_release magpie`) and `bin/magpie_test`
(`make BUILD=vlg magpie_test`) built, and `data/strategy/` writable.
For the 21x21 super board build both with `BOARD_DIM=21`; the recipe is
the same (the quad-word/quad-letter channels train from the same rows).

Substitute the lexicon for `LEX` and a name for `NAME` throughout.

## 0. Bootstrap file: `pat_zero_lexsigned_nofit`

An all-zero file with the semantic flags the recipe was validated under.
Every weight row is `<name>,0`; the option rows are

    gamma,0.500000
    own_asset_discount,0.000000
    lexicon_floaters,1
    signed_through,1
    fit_scaled,0

(A copy of CSW21's `pat_zero_lexsigned_nofit.pat` is exactly this; the
file is lexicon-independent.) `pat_write` of a zeroed PATWeights with
those flags set produces one too.

## 1. Iterative training (v3 stage)

    ./bin/magpie patgen 30000,30000,30000,30000,30000 NAME_v3 -lex LEX \
      -gp true -threads 10 -seed 61000002 -wmp true \
      -pat pat_zero_lexsigned_nofit

Five generations of 30K games each, each generation's self-play under
the previous generation's weights; the 1-ply label (the opponent's
reply score); ridge with lambda = 1/observations; the unseen pool for
each training row excludes the mover's LEAVE. About 5 minutes on 10
threads for CSW21.

Seed variance is real: identical recipes at different seeds differ by
0.2-0.4 equity per disagreement, and CSW21's v3 is an above-median draw
of its own recipe. One draw is fine for a new lexicon (any of them is
about +3 points a game over no PAT); to pick the best of several, train
at a few seeds and run the whole-game match in step 4 between them.

## 2. Run-keyed through table and residual refit (v4 stage)

Copy `NAME_v3.pat` to `NAME_v3_runres.pat` and add, anywhere among the
option rows,

    run_through,1
    fit_residual,3

then

    ./bin/magpie patgen 150000 NAME_v4 -lex LEX -gp true -threads 10 \
      -seed 4242 -wmp true -pat NAME_v3_runres

One generation of 150K games under the v3 weights (frozen), refitting
only the 14 `float_through_*` weights against the run-keyed table. In
the output, set `fit_residual,0` so a later retrain from the file fits
every channel. About a minute.

## 3. Opening table (v5 stage)

    ./bin/magpie_test patopeningsim:LEX:NAME_v4

Sims the top 12 static candidates of 1000 seeded opening racks four
plies (400 iterations each, round robin) under NAME_v4 -- the file
WITHOUT an opening table, or the gap is measured against a static
equity that already carries one -- and prints, at the end, the rows to
paste into a copy of `NAME_v4.pat` (after `run_through,1`, say) as
`NAME_v5.pat`:

    opening_tiles_2,...
    ...
    opening_tiles_7,...
    opening_exchange,...

(milli-equity, relative to the best bin, all <= 0). About 20 minutes on
10 threads. The printed dev/eval split (even/odd racks) is a sanity
check that the table helps held-out choices; the file rows use every
rack.

## 4. Validation

Whole-game mirrored pairs are the arbiter; the 2-ply move-choice
harness is horizon-biased on strength questions and only for
which-move comparisons. Against no PAT:

    ./bin/magpie autoplay games 500000 -lex LEX -gp true -threads 10 \
      -seed 777000101 -wmp true -pat1 NAME_v5 -pat2 none

Read the `Player 1 spread per mirrored pair` line (mean, SE, 95% CI).
CSW21 v3 was +3.09 per pair over no PAT; v4 over v3 +0.37 and +0.25;
v5 over v4 +0.109 (500K pairs) and +0.071 (1M pairs, fresh seed). Use
1M pairs for effects near 0.1.

## What the file carries

The v5 files are format version 4 (`magpie_pat_v4` header): 150 weight
rows, the option rows above, `run_through,1`, and the seven opening
rows. Rows for the experimental premium-combination channels (format
version 5, `lm_*`) and the stage-scale rows are absent: absent reads as
zero / 1.0, and both were null in whole-game play (see the commit
history of `codex/pat-setup-value` for the evidence).
