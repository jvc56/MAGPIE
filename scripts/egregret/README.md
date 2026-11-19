# Greedy endgame regret lab (not for merge)

Built from exact-dynamic-leaves fc687927 plus lab code:

- `test/eg_regret_test.c` (`magpie_test egregret:...`): static self-play games
  (CSW24, stock leaves, no pool shift, no pre-endgame table, no PAT, seeds
  91000000 + game); at each empty-bag turn the mover's top <cands> plays by
  static equity get exact 2-, 4- (and, with at most <sixmax> tiles on the two
  racks, 6-) ply endgame values. Candidate 0 is the greedy pick. Output format
  in the file's header comment. Spec:
  `first=<game>,games=<n>,threads=<t>,cands=<k>,sixmax=<tiles>,tt=<0|1>`.
  Disjoint game ranges on different machines give disjoint data.
- `scripts/egregret/analyze.py <outputs...>`: regret of the greedy pick.
- `test/leave_bag_test.c` (`leavebag:`) and a LEAVEBAG_* hook in
  `src/impl/move_gen.c`: the bag-size leave experiment (off unless both
  LEAVEBAG_NAME and LEAVEBAG_DIR are set).

Run (needs data/ with CSW24 kwg, klv2, wmp):

    make magpie_test BUILD=no_pgo_release
    nohup bin/magpie_test egregret:first=100000,games=4000,threads=8,cands=12,sixmax=10,tt=1 > m2_a.out 2> m2_a.err &

About 2,000 positions an hour on 9 M4 threads; ~2.5 positions per game.
Memory: 16 MB of transposition table per thread.
