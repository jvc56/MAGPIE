#!/usr/bin/env python3
"""Export per-lexicon word-feature tables for the MAGPIE nerfed player.

For every word in each lexicon: playability (log10, from the 1M-game
autoplay campaign), literacy (log10 per-million, max of SUBTLEX-US and
Google web unigrams), and an absent-from-language flag.

Writes <MAGPIE>/data/lexica/<LEX>_wordfeats.csv rows: WORD,logplay,loglit,absent

Usage: export_wordfeats.py [lex ...]
"""
import math
import pathlib
import sys

MAGPIE = pathlib.Path("/Users/john/sources/may10-tui/MAGPIE")


def _stem_candidates(w):
    """Productive-inflection stem candidates for a word (mirrors the C
    nerfed_player_backfill_inflections logic exactly): a stem plus
    -ING/-ED/-S/-ES/-IES/-ER/-EST/-LY, handling e-drop and consonant
    doubling. Mutually exclusive by the word's ending, same else-if order."""
    n = len(w)
    if n >= 5 and w.endswith("ING"):
        c = [w[:-3], w[:-3] + "E"]              # WALK / BAKE
        if w[-4] == w[-5]:
            c.append(w[:-4])                    # STOP (undouble)
        return c
    if n >= 4 and w.endswith("ED"):
        c = [w[:-2], w[:-1]]                    # WALK / BAKE (drop D)
        if n >= 5 and w[-3] == w[-4]:
            c.append(w[:-3])                    # STOP
        return c
    if n >= 5 and w.endswith("EST"):
        c = [w[:-3], w[:-2]]                    # TALL / NICE
        if w[-4] == w[-5]:
            c.append(w[:-4])                    # BIG
        return c
    if n >= 4 and w.endswith("ER"):
        c = [w[:-2], w[:-1]]                    # WALK / NICE
        if n >= 5 and w[-3] == w[-4]:
            c.append(w[:-3])                    # BIG
        return c
    if n >= 4 and w.endswith("LY"):
        c = [w[:-2]]                            # QUICK
        if w[-3] == "I":
            c.append(w[:-3] + "Y")             # HAPPY (ILY -> Y)
        return c
    if n >= 3 and w.endswith("S"):
        c = [w[:-1]]                            # CAT
        if n >= 4 and w[-2] == "E":
            c.append(w[:-2])                    # BOX
            if w[-3] == "I":
                c.append(w[:-3] + "Y")         # TRY (IES -> Y)
        return c
    return []


def backfill_inflections(feats):
    """A productive inflection of a more recognizable common stem should be
    as VISIBLE as its stem (a literate player reads CONSIDERING off
    CONSIDER + -ING on sight), but self-play frequency floors the long
    form. Rewrites each inflection's logplay/loglit from its best valid
    stem. feats: {WORD: [logplay, loglit, absent]}, modified in place.
    Reads originals into a snapshot so stem chains are order-independent."""
    orig = {w: (f[0], f[1]) for w, f in feats.items()}
    for w, f in feats.items():
        best_lp, best_lit = f[0], f[1]
        for stem in _stem_candidates(w):
            s = orig.get(stem)
            if s is not None and s[0] > best_lp:
                best_lp = s[0]
                best_lit = max(f[1], s[1])
        f[0] = best_lp
        f[1] = best_lit
HERE = pathlib.Path(__file__).parent.parent
DEFAULT_LEXICA = ["TWL98", "TWL06", "TWL14", "NWL18", "NWL20", "NWL23",
                  "CSW07", "CSW12", "CSW15", "CSW19", "CSW21", "CSW24",
                  "OSWI"]


def main():
    lexica = sys.argv[1:] or DEFAULT_LEXICA
    sub = {}
    with open(HERE / "wordfreq/SUBTLEXus74286wordstextversion.txt") as fh:
        next(fh)
        for line in fh:
            parts = line.split("\t")
            sub[parts[0].upper()] = float(parts[5])
    ng = {}
    with open(HERE / "wordfreq/count_1w.txt") as fh:
        for line in fh:
            w, c = line.split("\t")
            ng[w.upper()] = int(c) / 1e6

    for lex in lexica:
        words = [w.strip().upper() for w in
                 open(MAGPIE / f"data/lexica/{lex}.txt") if w.strip()]
        play = {}
        ppath = MAGPIE / f"{lex}_playability.csv"
        if ppath.exists():
            for line in open(ppath):
                c, w = line.strip().split(",")
                play[w] = float(c)
        feats = {}
        for w in words:
            lit = max(sub.get(w, 0.0), ng.get(w, 0.0))
            logplay = math.log10(1 + play.get(w, 0.0) * 1000) - 2.0
            loglit = math.log10(1 + lit) - 1.0
            feats[w] = [logplay, loglit, 1 if lit == 0 else 0]
        # Visibility of a productive inflection tracks its stem's, not its
        # own (floored) self-play frequency.
        backfill_inflections(feats)
        out = MAGPIE / f"data/lexica/{lex}_wordfeats.csv"
        with open(out, "w") as fh:
            for w in sorted(feats):
                lp, lit, absent = feats[w]
                fh.write(f"{w},{lp:.4f},{lit:.4f},{absent}\n")
        print(f"{lex}: {len(words)} words -> {out.name}")


if __name__ == "__main__":
    main()
