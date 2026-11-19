# Empty-bag eval "check + opponent's best score + split leaves" (lab handoff)

Fitted on 8,338 M4 positions (static's top 12 candidates, 4-ply labels, 5 folds):
held-out regret 2.48 vs greedy 4.33 (-1.85, SE 0.09). Weights:
`weights/check_best_split_l3.tsv` (name <tab> value; a missing name is 0).

## Value of a candidate at an empty bag

    eq = static equity as the engine computes it today
         (out play: score + 2 * opp rack face; else score - 2 * leave face - 10;
          pass: -2 * rack face - 10)
    if the move goes out:            v = eq + w[_out]
    else, after playing the move, generate ALL the opponent's moves:
      oo  = the opponent has a play using all their tiles
      obo = their best out-play score (if oo)
      obs = their best score of any play (0 if they can only pass)
      tag = "oo" if oo else "no"
      v = eq
        - obo                      (if oo; the unfitted rule)
        + w[const|tag]
        + w[obo] * obo             (if oo)
        + w[obs|no] * obs          (if not oo)
        + w[n<leave size>|tag] + w[face|tag] * (leave face sum)
        + sum over each DISTINCT 1- and 2-tile sub-multiset s of the leave
          (letters sorted, blank = '?', e.g. leave EEI -> E, I, EE, EI):
              w[s] + w[s|tag]

Pick the highest v; ties go to static order. The fit only saw static's top 12
candidates, so apply it to static's top N (try N = 4, 6, 12).

## Reference implementation of the features
`test/eg_regret_test.c`: `egs_print_replies` (plays each candidate, full
opponent movegen, prints oo/obo/obs and the leave). `egstuck:all:<file>` and
`egsolve:<file>` are described at the top of that section. Fit:
`fit_eg_combo.py`; best-first study: `bestfirst.py`.

## Cheaper versions measured offline (regret vs greedy, same positions)
- rule only (v = eq - obo), all moves, best-first: -0.82; 1.14 opponent
  rechecks per position using the bound "eq - best cached opponent out play
  the move does not touch" (one opponent movegen before the move).
- rule + split leaves without obs: -1.44.
