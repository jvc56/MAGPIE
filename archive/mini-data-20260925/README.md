# Data files not in MAGPIE-DATA (archived 2026-10-04)

These files are from `~/sources/magpie-data-20260925/data` on the M4 Mini, the data directory the PAT and static-ish work ran against. Each one is either missing from jvc56/MAGPIE-DATA or differs from it:

- **`strategy/*.pat`** (186 files): experimental PAT weight generations, such as the `*_efr_v3_gen_N`, `*_efr_v4` and `*_bootstrap` series. The production `<LEX>.pat` and `<LEX>_super21.pat` files are not here, because MAGPIE-DATA already has them, byte-identical.
- **`strategy/*_report.txt`** (128 files): the training reports for those generations.
- **4 CSVs** that differ from their MAGPIE-DATA versions.

Paths are relative to `data/`. This is an archive branch; not for main.
