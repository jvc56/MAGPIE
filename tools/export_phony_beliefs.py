#!/usr/bin/env python3
"""Export per-word phony belief priors + union believed lexicons.

For each target lexicon, build the full candidate phony pool (generator
mechanisms, as in phony_playability_runs.candidates_for), write:
  MAGPIE/data/lexica/<LEX>PHALL.txt   real lexicon + ALL candidates
  MAGPIE/data/lexica/<LEX>_phony_beliefs.csv  word,b0,xfam
where b0 = log(tier_weight * ortho_plausibility) is the rating-free
belief logit; the C side adds a global calibration offset and rating
slopes (+ for cross-family words, - otherwise).

Usage: export_phony_beliefs.py <corpus_root> <LEX> [<LEX> ...]
"""
import importlib.util
import json
import math
import pathlib
import random
import sys

import numpy as np

HERE = pathlib.Path(__file__).parent
spec = importlib.util.spec_from_file_location("pg", HERE / "phony_generator.py")
pg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pg)
spec2 = importlib.util.spec_from_file_location(
    "ppr", HERE / "phony_playability_runs.py")
ppr = importlib.util.module_from_spec(spec2)
spec2.loader.exec_module(ppr)

MAGPIE = pathlib.Path("/Users/john/sources/may10-tui/MAGPIE")


def main():
    root = pathlib.Path(sys.argv[1])
    lexica = sys.argv[2:]
    score = ppr.ortho_scorer(root)
    fam_ph = {}
    import collections
    fam_counts = collections.defaultdict(collections.Counter)
    for line in open(root / "phony_inventory_clean.jsonl"):
        r = json.loads(line)
        fam = "CSW" if r["lexicon"].startswith(("CSW", "OSW")) else "TWL"
        fam_counts[fam][r["word"]] += r["plays"]
    corpus_ph = {f: {w for w, c in cnt.items() if c >= 2}
                 for f, cnt in fam_counts.items()}
    lexsets = {}
    for lx in set(lexica) | {"CSW24", "NWL23"}:
        p = MAGPIE / f"data/lexica/{lx}.txt"
        if p.exists():
            lexsets[lx] = set(w.strip().upper() for w in open(p) if w.strip())
    for lx in lexica:
        fam = "CSW" if lx.startswith(("CSW", "OSW")) else "TWL"
        pool_lex = "CSW24" if fam == "TWL" else "NWL23"
        other = lexsets.get(pool_lex, set()) - lexsets[lx]
        rng = random.Random(1234 + hash(lx) % 997)
        cands = ppr.candidates_for(lx, lexsets[lx], other,
                                   corpus_ph[fam], rng, score)
        xfam_words = set()
        rows = []
        plays = fam_counts[fam]
        for word, tier, weight in cands:
            if weight <= 0:
                continue
            # Corpus play counts widen the head: TE-class phonies are
            # believed by nearly everyone (shared belief is what lets
            # them stick), while the generated tail stays idiosyncratic.
            b0 = math.log(weight) + 0.35 * math.log(1 + plays.get(word, 0))
            if tier == "xfamily":
                xfam_words.add(word)
            rows.append((word, b0, 1 if tier == "xfamily" else 0))
        aug = sorted(lexsets[lx] | {w for w, _, _ in rows})
        with open(MAGPIE / f"data/lexica/{lx}PHALL.txt", "w") as f:
            f.write("\n".join(aug) + "\n")
        klv_src = MAGPIE / f"data/lexica/{lx}.klv2"
        if klv_src.exists():
            import shutil
            shutil.copy(klv_src, MAGPIE / f"data/lexica/{lx}PHALL.klv2")
        with open(MAGPIE / f"data/lexica/{lx}_phony_beliefs.csv", "w") as f:
            for word, b0, xfam in sorted(rows):
                f.write(f"{word},{b0:.3f},{xfam}\n")
        print(f"{lx}: {len(rows)} candidate phonies "
              f"({len(xfam_words)} cross-family), union {len(aug)}",
              flush=True)
        import subprocess
        subprocess.run([str(MAGPIE / "bin/magpie"), "convert", "text2kwg",
                        f"{lx}PHALL"], capture_output=True, cwd=str(MAGPIE),
                       timeout=600)


if __name__ == "__main__":
    main()
