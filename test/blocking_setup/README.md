# Blocking/setup parameters: data, fitting and studies

Tools for producing per-lexicon blocking/setup parameters (`.bsp` files) and
for measuring what they do. The runtime pieces live in `src/`:

- `src/impl/blocking_setup.[ch]` — the pass-relative teacher.
- `src/ent/blocking_setup_params.[ch]` — the `.bsp` format.
- `src/impl/sim_nomination.[ch]` — sim root-candidate nomination.

## Definitions and units

Both checks compare a candidate with passing on the same sampled opponent
racks, dealt from the on-turn player's unseen pool (bag plus opponent rack as
one multiset; the opponent's actual rack is never read):

- `blocking_delta = mean(best opponent placement score after our pass
  - best opponent placement score after the candidate)`
- `setup_delta = mean(our best placement score after candidate + opponent
  reply - our best placement score after pass + opponent reply)`, with our
  rack in both branches equal to the candidate's leave plus the same refill
  tiles, drawn from the pool less the sampled opponent rack. A reply that ends
  the game leaves a follow-up of 0. Exchanges are never replies or follow-ups.

Deltas are score points. A `.bsp` weight is equity points per delta point:
`adjusted = static_equity + double_to_equity(wb * blocking_delta + ws *
setup_delta)`. Engine `Equity` is millipoints.

There are three separate things, which must not be confused:

1. **Teacher labels** — the sampled deltas above (expensive: about 0.5 s per
   position for ~60 candidates at 64 racks on an M4 with 8 busy processes).
2. **Cheap approximations** — so far only fewer racks; `bs_fit.py teacher`
   reports how well 16/64 racks reproduce 256.
3. **Weights and policies** — what a `.bsp` file holds, fitted for a stated
   objective (`objective` row):
   - `static_choice`: pick `argmax(adjusted)`; utility = reference WP of the
     pick minus that of the plain static pick (pp). Selection never reads a
     reference, so a single reference is unbiased.
   - `sim_admission`: nominate static top-N plus top-N by adjusted value;
     utility = reference WP of the best admitted play minus the best of an
     equal-count static pool. Choose with reference A and value with an
     independent reference B; with one reference the max is biased up.
   Weights fitted for one objective are not weights for the other.

## `.bsp` files

`data/strategy/<name>.bsp`; see `src/ent/blocking_setup_params.h` for every
row. Rows: `lexicon`, `model_version`, `objective`, `teacher_*` (the teacher
the weights were fitted under), `blocking_weight`, `setup_weight`, optional
`bin,<min_bag>,<max_bag>,<min_lead>,<max_lead>,<wb>,<ws>` (first match wins;
conditional policies), and free-text `provenance` rows (seeds, hashes,
split). **Missing files are an error**; nothing falls back silently. A
nomination request with blocking or setup nominees and no parameters is an
error too.

No `.bsp` is published yet. The default for experiments is `research_v1`:
the `equity_reply` teacher (opponent replies valued by static equity, our
follow-ups by score) with weights 0.7 / 0.4. It is at least as strong as
score mode at 1.4 / 0.75 in CSW24, NWL23 and FRA20 (+0.85 ± 0.37 pp pooled,
null in NWL23), ties equity mode (+0.48 ± 0.37 pp) at about 60% of its time,
and is cheaper per decision than score mode. The weights were tuned on CSW24
only (see #747's `notes/static-ish-studies-20261001/REPORT.md`, P37–P48):

```
magpie_bsp_v1
lexicon,CSW24
model_version,research-v1
objective,sim_admission
teacher_racks,64
teacher_value,equity_reply
blocking_weight,0.7
setup_weight,0.4
provenance,<tuning and confirmation runs>
```

A file without a `teacher_value` row still means `score`, so earlier files
(such as `research_v0`, score mode 1.4 / 0.75 from the September 2026
candidate-diversity studies, not fitted) keep their meaning. Always write
the row in new files.

## Pipeline

Build an optimized test binary and copy it, so rebuilding mid-run cannot mix
builds:

```sh
make magpie_test BUILD=no_pgo_release
cp bin/magpie_test /some/where/magpie_test_run
```

1. Generate (independent games, one position per game; teacher at nested rack
   counts; two independent references):

   ```sh
   BIN=/some/where/magpie_test_run test/blocking_setup/generate.sh \
       CSW24 ~/sources/bs-data/CSW24-run1 3000 <seed> 8 10000 CSW24
   ```

   Stages can also be run by hand: `bsgen:positions`, `bsgen:labels`,
   `bsgen:refs` (see `test/blocking_setup_gen_test.c`). Positions keep both
   racks in their CGP for auditing; the teacher and features never use the
   opponent's rack. Leaves default to the lexicon name (`LEAVES=`), and
   `WMP=false` for lexica without a word map.

2. Freeze the split before tuning (by game; refuses to overwrite):

   ```sh
   test/blocking_setup/bs_fit.py split --positions $D/positions.csv \
       --lexicon CSW24 --seed <seed> --out $D/split.json
   ```

3. For `static_choice`, concentrate references on the grid's picks:

   ```sh
   test/blocking_setup/bs_fit.py choices --labels $D/labels.w[0-9].csv \
       --out $D/choices.csv
   # then bsgen:refs ... :cands=$D/choices.csv
   ```

4. Fit on train, report validation, write a `.bsp`:

   ```sh
   test/blocking_setup/bs_fit.py fit --labels $D/labels.w[0-9].csv \
       --refs $D/refsA.w[0-9].csv --refs-b $D/refsB.w[0-9].csv \
       --split-file $D/split.json --lexicon CSW24 --model-version v1 \
       --objective static_choice --out $D/CSW24.bsp --report $D/fit.json
   ```

5. `bs_fit.py teacher` (rack-count precision), `volatility` (does a fixed
   policy's gain vary with lead, bag, PAT's pre-board value, the spread or
   extremes of the candidates' deltas; train + validation only), and finally
   `evaluate --split test --final` once.

Use exact globs (`labels.w[0-9].csv`); each labels shard has a
`<name>.timing` side file with per-position movegen and teacher times.

## Studies

- `pools_study.sh` + `pools_analyze.py` + `build_pool_explorer.py`: root pool
  arms (A static+checks, B PAT+checks, Bp PAT-ranked checks, C
  static+PAT+checks) against exactly equal-size static pools, top-two
  selection sims and an independent round-robin reference over the union,
  no-PAT static rollouts. `sims=0` gives an untimed nomination audit.
- `games_study.sh` + `games_analyze.py`: paired games (same tiles per seat,
  seats swapped) between `static`, `pat` and `adjusted` players, with
  per-decision timing.

Do not commit generated data. A published `.bsp` belongs in the MAGPIE-DATA
repository with its provenance rows.
