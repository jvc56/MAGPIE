# Softmax inference

Inference uses static score-plus-leave evaluation. Where the data has a
temperature table for the player's leaves (see [Temperatures by
bag](#temperatures-by-bag)), inference uses softmax weighting with the table's
temperature for the bag and no margin cutoff. Otherwise the default is
unchanged from earlier releases: temperature zero with a 5-point admission
margin, which keeps every rack whose best play beats the observed play by at
most 5 points and weights it by draw count ("hard margin"). A positive
temperature enables softmax weighting. PAT and neural inference experiments
have been removed.

`set -inftemp 2 -imargin 10` selects temperature 2 and admission margin 10.
`set -inftemp 8 -imargin 10000` gives T8 with effectively no cutoff, the
setting validated in [the static temperature report](static-temperature-validation-20261103.md).
Decision tests against a 4-ply simulation opponent found constant T8 slightly
worse than the hard margin (−0.046 ±0.035 win percentage points per decision
with 8 or more tiles in the bag), so constant T8 is not a default.
`-inftemp1` and `-inftemp2` override the temperature for each autoplay player;
setting `-inftemp` also sets both players' temperatures.

For each admitted candidate rack, inference computes the observed move's equity
and the best available move's equity. Its unnormalized posterior weight is

```
draw_count * exp(-max(0, best_equity - observed_equity) / temperature)
```

The margin still rejects racks whose best move beats the observed move by more
than the margin. For exchanges, the observed equity is that of the best exchange
of the observed size. Empty-bag racks retain the existing admission exception.
Opening placement adjustments and preendgame/endgame adjustments apply as in
static inference.

Letter probabilities, leave-value statistics, common-leave ordering, and
simulation sampling use posterior weights. Displayed draw counts remain exact
integers. Results show the temperature when weighting is enabled. Alias tables
are deterministic across worker counts; floating-point statistics agree within
rounding tolerance. If all weights underflow to zero, the sampler reports no
available posterior instead of sampling an invalid table.

Cutoff inference can still reject a rack as soon as a move exceeds the margin.
For admitted racks, anchor pruning uses the observed equity so it retains moves
needed to compute regret. Empty-bag weighted inference uses a full search because
its admission exception also accepts moves above the margin.

When at least four tiles are unknown, eligible positions use census play scores
to evaluate candidate racks. Empty boards include the legacy vowel placement
adjustment in each table entry before selecting its maximum; blank substitution
changes face score but preserves the represented letter's vowel penalty. This
accelerates inference without changing admission or softmax weights. See the
[performance and parity report](census-inference-performance.md).

## Temperatures by bag

The best temperature depends on how many tiles are in the bag: low (sharp)
early in the game, when a player's leave is what their play is chosen for,
and high (flat) near the end. A temperature table,
`<data path>/strategy/<name>.csv`, gives it as knots:

```
# comments start with #
4,12
10,6
17,5
25,4
38,3
53,2
75,1.5
exchange,3
```

Each `bag,temperature` row is a knot for the number of tiles in the bag before
the observed play, in ascending order (1 to 64 rows); between knots the
temperature is interpolated in log temperature, and outside them it is the
nearest knot's. The optional `exchange` row is the temperature for inferring
an exchange; without it an exchange uses the bag's temperature.

`-inftemps auto` (the default for the command line program and
`magpie_create()`) gives each player its leaves' own table,
`<leaves>_inftemp`, when the data has one. `-inftemps <name>` names a table,
and `-inftemps none` turns tables off, so inference uses `-inftemp` and
`-imargin`. `-inftemps1` and `-inftemps2` set one player's. A player with a
table ignores `-inftemp` and `-imargin` when it infers.

The tables shipped for the English lexica and FRA20 are the one above. It was
fitted on CSW24 self-play between 4-ply, 100,000-rollout simulation players, by
the likelihood of the leave really kept; separate fits on NWL23 and FRA20 games
choose the same temperatures within one grid step, and using the CSW24 table
costs them at most 0.04 nats per move, which is not significant. In a decision
test against the same player (CSW24, 400 games, each changed decision scored by
a 4-ply simulation told the opponent's kept tiles), the table against the hard
margin was −0.003 ±0.022 win percentage points per decision with 8 or more
tiles in the bag and +0.298 ±0.200 with 1 to 7 (95% intervals clustered by
game). Lexica with other tile distributions, and the 21x21 board, have no table
yet.

`inftempfit <games> [<name>]` fits a table: it plays `<games>` games with the
players as set and, at each placement that keeps tiles with tiles in the bag
and at each exchange, infers the mover's leave at temperatures from 0.5 to 32
points. Each bag band's knot is its mean bag with the temperature that gives
the leave really kept the highest probability. The table is written to
`<data path>/strategy/<leaves>_inftemp.csv` unless `<name>` is given, and its
header records each band's count and gain over the draw prior. The result does
not depend on the thread count.
