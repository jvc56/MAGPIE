# PAT files kept in the repository for safety

The data directory is gitignored, so these copies of the small (a few
KB) PAT weight files are tracked here. Each file's header carries its
provenance; see ../pat_champion_recipe.md for how they are built.

- pat_dls_champion_v5.pat: the CSW21 champion (v4 + opening table).
- pat_dls_champion_v4.pat, _v3.pat, _v2.pat: its predecessors.
- pat_zero_lexsigned_nofit.pat: the zero bootstrap every recipe starts
  from; pat_dls_champion_v3_runres.pat: v3 with the through-refit rows.
- CSW24_v5_s<i>_v4.pat, NWL23_v5_s<i>_v4.pat: per-seed v4-stage files
  for those lexica (before the opening table), from
  test/pat_build_champion.sh.
