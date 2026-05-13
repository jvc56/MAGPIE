# pegN: POND / DEFNNPT III/A analysis

Position: macondo "pond_pah" position 0, mover rack DEFNNPT, 4-in-bag.
Cand: `2L P(O)ND 14`, scenario `III/A` (mover draws III, bag remaining A).
This is one of macondo's 5 doc-stated III losing scenarios at endgameplies=4
(III/A, III/E, III/G, III/S, III/T → 36 of 7920 perms = 99.55% win).

## SH ladder over Noah's 8 perceived bag scenarios

Top-K Noah moves were ranked by Noah utility = `win% + alpha * mean_spread`
(alpha = 1e-4) averaged across Noah's 8 perceived bag scenarios. Each
hypothetical evaluated with greedy at d=0 and with `endgame_solve` at d=1/d=2.

| d=0 rank | d=1 rank | d=2 rank | move | d=2 util | d=2 win | d=2 spread |
|---|---|---|---|---|---|---|
| **#15** | **#1** | **#1** | `2A A(U)GUSTER 61` | **+0.250** | 25.0% | +0.1 |
| **#13** | **#2** | **#2** | `15H (TEMP)TER 12` | **+0.186** | 18.8% | -15.6 |
| #3 | #3 | #3 | `15H (TEMP)URA 12` | +0.124 | 12.5% | -10.0 |
| #4 | #10 | #26 | `15H (TEMP)T 10` | -0.003 | 0% | -25.2 |
| #6 | #12 | #15 | `N6 T(ORE) 6` | -0.002 | 0% | -24.1 |
| #2 | #15 | #8 | `J3 U(N) 2` | -0.002 | 0% | -19.5 |
| #1 | #47 | — | `6D (T)A 2` | (eliminated) | | |

The d=0 ranker correlates badly with deeper rankers: the two d=2 best moves
(A(U)GUSTER, (TEMP)TER) sit at d=0 ranks #15 and #13 — outside any
reasonable top-8 cut. A naive SH "top-8 by d=0 → top-4 by d=1 → top-1 by
d=2" picks **TEMPURA** as the winner of the d=0 top-8, but TEMPURA is only
the d=2 #3 overall.

## Realized III/A outcome (not Noah's averaged perception)

When mover plays POND and the realized scenario is III/A (not averaged),
mover's d=2 endgame outcomes against the three contenders:

| Noah's move | mover_total | outcome |
|---|---|---|
| `2A A(U)GUSTER 61` | **+1** | mover wins by 1 |
| `15H (TEMP)TER 12` | **+23** | mover wins by 23 |
| `15H (TEMP)URA 12` | **-5** | **mover loses by 5** |

TEMPURA's killer line:
```
opp 15H (TEMP)URA 12  (leave EGST)
mover 14M EFT 28
opp O12 GE(T)S 57
mover 11C (A)IN 14
opp I6 TA 17           → mover spread -5
```

A(U)GUSTER and (TEMP)TER both *gain* their high Noah utility from winning
*other* perceived scenarios (not III/A). TEMPURA is the unique kill in
III/A.

## Implications

1. Macondo's "III/A loses" label is contingent on Noah playing
   TEMPURA. Noah's d=2 *utility-maximizing* move (over his perceived
   bag) is A(U)GUSTER, which does **not** kill mover in III/A.

2. The actual loss rate depends on Noah's policy:
   - Noah plays utility-optimal (A(U)GUSTER): mover wins III/A (+1)
   - Noah plays worst-case-per-scenario: mover loses III/A (-5)

3. SH starting from a d=0 greedy top-K is unsafe for this position.
   At d=0 the actual d=2 best moves sit at ranks #15 and #13. A
   correct ranker needs at least one d=1 elimination round across
   a much larger initial pool, or a smarter d=0 prior.

## Test knobs added in this commit

- `PASSPEGN_GREEDY_NOAH_UTIL=<alpha>` — for each scenario, rank all opp
  tile-placement moves by Noah utility averaged over Noah's perceived
  bag scenarios. Prints top-N and locates target moves (TEMPURA,
  A(U)GUSTER, TEMP(T)ER, etc).
- `PASSPEGN_GREEDY_NOAH_DEPTH=<d>` — when >0, evaluate each
  hypothetical with `endgame_solve` at d plies instead of greedy.
- `PASSPEGN_GREEDY_NOAH_TOPN=<n>` — top-N to print (default 20).
- `PASSPEGN_GREEDY_OPP_MATCH='A(U)GUSTER;TEMP)URA;...'` —
  semicolon-separated substring filter on opp move text in the
  d>=1 evaluator; lets you isolate a single opp move per scenario.
- `PASSPEGN_GREEDY_DUMP_OPP=1` — dump every opp move with its greedy
  continuation (mover_total, PV, end racks).
