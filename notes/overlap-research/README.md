# Do the teacher's racks share work? (scratch research, not for a PR)

Asked: could one pass find every sampled rack's best play at once, since the
racks overlap? Measured with `bsoverlap` (test/overlap_research_test.c) on
24 CSW24 positions, 64 racks, then timing experiments with `bsbench` (48
positions, exact teacher, 152 ms/position baseline, one M4 core).

| measure (per position) | total | distinct | repeat |
|---|---:|---:|---:|
| word-map lookups in one exact teacher run | 3.32 M | 251 k | 13x |
| word-map word-list expansions | 126 k | 9.4 k | 13x |
| plays over 64 racks, pass board | 82 k | 40 k | 2.1x |
| distinct plays over 1,024 racks, pass board | | 199 k | |
| plays over 64 racks in a candidate's lanes, per board | 39 k | 19.6 k | 2.0x |

- Pool-as-rack enumeration: the racks' play sets overlap only ~2x, and a
  pool rack would enumerate >= ~200 k plays on the pass board (~20 k in a
  candidate's lanes) versus 64 pruned best-play searches (~1.3 ms, ~0.6 ms
  in lanes). Estimated 4-25x slower. Not worth building.
- Word-map repetition: movegen's existing per-thread sub-rack cache was
  never used by the teacher (it requires leave values, which score-only
  searches with exchanges skipped never compute). Letting score-only
  searches use it (entries flagged with whether leave values are valid):
  152 -> 151.6 ms with 64 slots, 147 ms with 512 (+~2 MB per thread).
  Identical output.
- A 1M-slot (bit-rack, length) -> entry memo inside wmp_get_word_entry made
  it slower (147-150 -> 154-155 ms): the lookups are already cheap.

Conclusion: the overlap is real but does not pay; at most ~3% for 2 MB per
thread. Not pursued.
