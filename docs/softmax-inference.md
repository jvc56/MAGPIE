# Softmax inference

Inference uses static score-plus-leave evaluation. The default is unchanged
from earlier releases: temperature zero with a 5-point admission margin, which
keeps every rack whose best play beats the observed play by at most 5 points
and weights it by draw count ("hard margin"). A positive temperature enables
softmax weighting. PAT and neural inference experiments have been removed.

`set -inftemp 2 -imargin 10` selects temperature 2 and admission margin 10.
`set -inftemp 8 -imargin 10000` gives T8 with effectively no cutoff, the
setting validated in [the static temperature report](static-temperature-validation-20261103.md).
Decision tests against a 4-ply simulation opponent found constant T8 slightly
worse than the hard margin (−0.046 ±0.035 win percentage points per decision
with 8 or more tiles in the bag), so the hard margin stays the default.
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
