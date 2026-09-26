# Root-only contextual leaves (`-rootleaves`)

PR #630's contextual KLV3 helped most as a **candidate selector**: when two
top-15 lists differed, KLV3's unique nominees beat KLV2's 283-217 (regret
reduction 0.0029 utility, p = 9e-5). In rollouts it cost 17% throughput and
showed no gain. This branch uses KLV3 only where it paid: to rank a
simulation's root candidates. Rollouts keep the player's KLV2, and with it
every rack-keyed cache (RIT, WMP leave maxima).

## How it works

A KLV3 leave value is the KLV2 value plus an additive contextual term:

```text
V(L, U, N, d) = KLV2(L) + sum_h L[h] * (bias[pool_bin(N), d, h]
                                        + sum_u weight[d, h, u] U[u] / N)
```

`U` is the public unseen multiset (bag plus opponent rack), `N` its size,
and `d` the number of tiles the move draws (`min(tiles played, bag)` for a
placement, the tiles exchanged for an exchange, and 0 for a pass, which gets
no term).

Move generation already scores every candidate with KLV2 (and PAT when
`patcand` is on), so adding the term to a move's equity gives its KLV3 equity
exactly. `generate_root_candidates` (`src/impl/root_candidates.c`) does this:
1. generate the player's best 1,000 moves by its own static equity;
2. compute the per-tile adjustments once for the position;
3. add each move's term;
4. keep the best `capacity(move_list)` moves, sorted.

The term moves a leave by a few points, so nothing outside the 1,000-move pool
can reach a root list of ordinary size.

`RootLeaves` (`src/ent/root_leaves.c`) reads only the KLV3 trailer and skips
the KLV2 body. The trailer must have been trained over the KLV2 the player
uses. Version-2 per-leave caps are ignored, because they only matter to a
contextual RIT and the root evaluates every candidate exactly.

## Option

`-rootleaves <name|none>`, per player `-rootleaves1` / `-rootleaves2`: load
`data/lexica/<name>.klv3` for that player's root candidates. It applies to
autoplay sim players (`get_top_simming_move`) and to PlayChooser's midgame sim.
The interactive `sim` command ranks the list `gen` produced and is unaffected.

- `none`, the default, is plain move generation. With it unset, games are
  byte-identical to the base branch (checked: fixed-iteration autoplay sim
  players, same seed).
- A table whose alphabet differs from the letter distribution is refused
  (`ERROR_STATUS_CONFIG_ROOT_LEAVES_MISMATCH`).
- **Zero-coefficient behaviour:** a zero-coefficient KLV3 gives the same
  candidate set and equities as KLV2. The root list comes back sorted rather
  than in move generation's heap order, though, so the sim's arm order and
  therefore its random sampling differ. Results are then equal in distribution,
  not byte-identical.

## Recomputed leave values at the root (design sketch, not implemented)

The same slot can take values that are not a trained model at all. At the
root, the actual unseen pool is known, so for each distinct leave among the
candidates (typically tens):

- draw `d` tiles from `U` many times (a few hundred draws, with common random
  numbers across leaves);
- for each draw, evaluate the resulting rack, e.g. by its best static equity
  next turn, or its KLV2 value plus a bingo-probability term;
- replace the leave's KLV2 value with the mean.

This is a draw-conditional leave value that no rack-keyed cache could ever
hold, and it costs only a few thousand movegen calls per decision. That fits
the root budget (seconds) and would be unaffordable in rollouts. It plugs into
`generate_root_candidates` the same way: an adjustment
`recomputed(L) - KLV2(L)` added per move, with a per-decision memo keyed by
leave. Worth doing only if the KLV3 root A/B shows candidate quality matters.

## Training a CSW24 KLV3

Use #630's recorder and trainer, both on this branch: `autoplay fj` writes the
versioned training rows (#630's recorder change is ported), and
`tools/train_klv3.py` needs numpy.

```bash
make BUILD=no_pgo_release magpie
mkdir -p obj/klv3-training-csw24 && cd obj/klv3-training-csw24
ln -s ../../data data
# Static KLV2 policy, no PAT and no RIT, so the recorded equities are pure
# KLV2: the KLV3 term is a residual over KLV2, and PAT's term is added
# separately at the root.
../../bin/magpie autoplay fj 200000 -lex CSW24 -threads 10 -wmp true \
  -seed 20260725 -pfreq 1000000 -rit false -pat none -savesettings false
python3 ../../tools/train_klv3.py --fj-glob 'autoplay_record_fj_*' \
  --letter-distribution data/letterdistributions/english.csv \
  --base-klv2 data/lexica/CSW24.klv2 --output-prefix data/lexica/CSW24_klv3_ctx
# -> data/lexica/CSW24_klv3_ctx400.klv3 (interaction-only, scale 4, #630's
#    selected model) and CSW24_klv3_ctx_report.json
```

- **Corpus size:** #630 used 50,000 games, giving 977,465 full-rack rows and
  1,558,560 projected examples. 200,000 games gives about 4× that at almost no
  extra cost.
- **Recording time:** 200 static games take 0.2 s on 2 threads, so the corpus
  takes under a minute on 10 threads.
- **Training time:** the trainer is numpy and single-threaded. Estimate about
  10-30 minutes for 12 epochs over 4-6M examples (not measured at full size).
- **Smoke test run on this branch:** 200 games and 1 epoch produced a loadable
  `.klv3` that `-rootleaves` used in autoplay.

## A/B with the position oracle

The harness on `claude/pc-candidates-oracle` (`pccands`) compares settings on
the same positions with one common-random-number oracle. To compare candidate
sources:
1. merge this branch into it;
2. add a setting dimension, e.g. `PCCANDS_ROOTLEAVES_LIST=none,CSW24_klv3_ctx400`,
   whose value is loaded once per worker as a `RootLeaves` and set on the
   PlayChooser strategy (`.root_leaves`);
3. hold K (say 15) and plies (4) fixed at the 13 s budget.

The oracle scores each chosen move independently of how it was nominated, so
the loss of KLV2+PAT against KLV3+PAT candidates is paired per position.

- **Expected effect:** #630's conditional regret reduction was about 0.3 point
  of utility on the positions where the lists differ. Differing positions are
  a minority, so the unconditional effect is smaller.
- **Sample size:** at about 0.1 point of oracle error per move, roughly
  2,000-4,000 positions should resolve a 0.05-point mean difference.
- **Why this is the efficient order:** the harness's K sweep also says whether
  wider pools help at all. If K = 45 barely beats K = 15, candidate *ordering*
  matters less than it seems.
