#!/usr/bin/env python3
"""The phony generator/analyzer stack: models WHY humans believe non-words.

Four mechanisms, in decreasing precision (each doubles as a generator and
as a membership analyzer for belief features):
1. AFFIX morphology: regular English prefix/suffix derivation with
   orthographic rules (e-drop, y->i, consonant doubling).
2. PLURAL-CLASS confusion: wrong plural class applied to Latin/Greek/other
   borrowings (CAESURA -> CAESURES, STEREO -> STEREOES).
3. COMBINING forms: neoclassical/compound composition over real bases
   (EXO+SPERM, CIDER+LIKE, MID+HASTE).
4. EDIT-1 near-miss: misremembered spellings, one edit from a real word,
   gated by the orthographic plausibility floor.

Analyzer mode classifies a candidate; measurement mode scores the corpus
phony inventory by source.

Usage: phony_generator.py <root> measure
"""
import collections
import json
import math
import pathlib
import re
import sys

MAG = pathlib.Path("/Users/john/sources/may10-tui/MAGPIE/data/lexica")
VOWELS = set("AEIOU")
PREFIXES = ["UN", "RE", "OUT", "MIS", "PRE", "NON", "OVER", "DE", "IN",
            "DIS", "SUB", "ANTI", "UNDER", "CO", "EN", "UP", "FORE"]
COMBINING_PRE = [
    "AERO", "AGRO", "ANTI", "AUTO", "BIO", "CRYPTO", "CYBER", "ECO",
    "ELECTRO", "EXO", "GEO", "HYDRO", "HYPER", "HYPO", "INTER", "INTRA",
    "ISO", "MACRO", "MEGA", "MICRO", "MID", "MINI", "MONO", "MULTI",
    "NEO", "OMNI", "PAN", "PARA", "PHOTO", "POLY", "PROTO", "PSEUDO",
    "SEMI", "SUPER", "TELE", "THERMO", "TRANS", "TRI", "ULTRA", "BI",
    "FAN", "GUN", "SUN", "MOON", "SEA", "SNOW", "WIND", "FIRE", "WOOD",
    "STONE", "IRON", "GOLD", "SILVER", "BACK", "DOWN", "OFF", "ON"]
COMBINING_SUF = [
    "LIKE", "WISE", "WARD", "WARDS", "AGE", "DOM", "HOOD", "SHIP",
    "GENIC", "LOGY", "METER", "GRAM", "GRAPH", "PHOBE", "PHILE", "PHOBIA",
    "ITIS", "OSIS", "ISM", "IST", "ITE", "OID", "ESQUE", "CRAFT", "WORK",
    "LAND", "WEED", "WEEDS", "SACK", "SACKS", "ROOM", "SIDE", "TIME"]
# wrong-plural transformations: (strip, add) applied to a REAL word to
# produce the confused form
PLURAL_CONFUSIONS = [
    ("A", "AE"), ("A", "AS"), ("AE", "AS"), ("AE", "A"),
    ("US", "I"), ("US", "II"), ("I", "USES"), ("US", "USSES"),
    ("UM", "A"), ("UM", "UMS"), ("A", "UMS"),
    ("IS", "ES"), ("IS", "ISES"), ("ES", "IS"),
    ("EX", "ICES"), ("IX", "ICES"), ("X", "XES"),
    ("O", "OES"), ("O", "OS"), ("EAU", "EAUX"), ("EAU", "EAUS"),
    ("F", "VES"), ("FE", "VES"), ("OUSE", "ICE"),
]


def load_lexicon(name):
    return set(w.strip().upper() for w in open(MAG / f"{name}.txt")
               if w.strip())


def affix_bases(word):
    """Real-word bases that would regularly derive `word` (suffix side)."""
    w = word
    cands = []
    for suf in ("INGS", "ERS", "NESS", "MENT", "LESS", "FUL", "ISH", "LY",
                "IEST", "IER", "IES", "IED", "ILY", "ING", "EST", "ED",
                "ER", "ES", "S", "D"):
        if not w.endswith(suf) or len(w) - len(suf) < 2:
            continue
        stem = w[: len(w) - len(suf)]
        outs = [stem]
        if suf in ("ING", "ER", "EST", "ED", "INGS", "ERS"):
            outs.append(stem + "E")            # e-drop inversion
            if len(stem) >= 2 and stem[-1] == stem[-2]:
                outs.append(stem[:-1])         # doubling inversion
        if suf in ("IES", "IED", "IER", "IEST", "ILY"):
            outs.append(stem + "Y")            # y->i inversion
        if suf == "D" and not stem.endswith("E"):
            outs = []                          # bare D only after E
        cands += [(o, suf) for o in outs]
    return cands


def classify(word, lexset, ortho_score=None, floor=-1.35):
    """Returns the highest-precision mechanism that generates `word`,
    or None. `word` must not be in lexset."""
    # 1. affix morphology (suffix or prefix on a real base)
    for base, _suf in affix_bases(word):
        if base in lexset:
            return "affix"
    for pre in PREFIXES:
        if word.startswith(pre) and word[len(pre):] in lexset \
                and len(word) - len(pre) >= 2:
            return "affix"
    # 2. plural-class confusion
    for strip, add in PLURAL_CONFUSIONS:
        if word.endswith(add):
            base = word[: len(word) - len(add)] + strip
            if base != word and base in lexset:
                return "plural_class"
    # 3. combining forms over real bases
    for pre in COMBINING_PRE:
        rest = word[len(pre):]
        if word.startswith(pre) and len(rest) >= 3 and \
                (rest in lexset or any(b in lexset for b, _ in
                                       affix_bases(rest))):
            return "combining"
    for suf in COMBINING_SUF:
        if word.endswith(suf):
            base = word[: len(word) - len(suf)]
            if len(base) >= 3 and base in lexset:
                return "combining"
    # 4. edit-1 near-miss with plausibility floor
    if ortho_score is None or ortho_score >= floor:
        A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        for i in range(len(word)):
            if word[:i] + word[i + 1:] in lexset:
                return "edit1"
            for c in A:
                if c != word[i] and word[:i] + c + word[i + 1:] in lexset:
                    return "edit1"
        for i in range(len(word) + 1):
            for c in A:
                if word[:i] + c + word[i:] in lexset:
                    return "edit1"
        for i in range(len(word) - 1):
            sw = word[:i] + word[i + 1] + word[i] + word[i + 2:]
            if sw != word and sw in lexset:
                return "edit1"
    return None


def main():
    root = pathlib.Path(sys.argv[1])
    fams = {"TWL98": "TWL", "TWL06": "TWL", "TWL14": "TWL", "NWL18": "TWL",
            "NWL20": "TWL", "NWL23": "TWL", "CSW07": "CSW", "CSW12": "CSW",
            "CSW15": "CSW", "CSW19": "CSW", "CSW21": "CSW", "CSW24": "CSW",
            "OSWI": "CSW"}
    alias = {"TWL15": "TWL14"}
    lexsets = {n: load_lexicon(n) for n in fams if (MAG / f"{n}.txt").exists()}
    # ortho model for the edit1 floor
    import numpy as np
    d = np.load(root / "ortho_model.npz")
    tri = {k: int(c) for k, c in zip(d["keys"], d["counts"])}
    bi = {k: int(c) for k, c in zip(d["bikeys"], d["bicounts"])}

    def score(w):
        s = "^^" + w + "$"
        lp = 0.0
        for i in range(len(s) - 2):
            lp += math.log10((tri.get(s[i:i + 3], 0) + 1) /
                             (bi.get(s[i:i + 2], 0) + 28))
        return lp / (len(s) - 2)

    ng = set()
    for i, l in enumerate(open(root / "wordfreq/count_1w.txt")):
        if i >= 100000:
            break
        ng.add(l.split("\t")[0].upper())
    inv = [json.loads(l) for l in open(root / "phony_inventory_clean.jsonl")]
    inv = [r for r in inv if alias.get(r["lexicon"], r["lexicon"]) in lexsets]
    cnt = collections.Counter()
    for r in inv:
        cnt[r["word"]] += r["plays"]
    multi = {w for w, c in cnt.items() if c >= 2}
    total = sum(r["plays"] for r in inv)
    cats = collections.Counter()
    for r in inv:
        w = r["word"]
        L = alias.get(r["lexicon"], r["lexicon"])
        p = r["plays"]
        if any(w in lexsets[x] for x in lexsets if x != L) or w in multi \
                or w in ng:
            cats["lists"] += p
            continue
        mech = classify(w, lexsets[L], score(w))
        cats[mech if mech else "UNCOVERED"] += p
    print(f"total phony plays {total}")
    run = 0
    for k in ("lists", "affix", "plural_class", "combining", "edit1",
              "UNCOVERED"):
        run += cats.get(k, 0)
        print(f"  {k:>14}: +{100*cats.get(k,0)/total:>4.1f}%  "
              f"(cum {100*run/total:.1f}%)")


if __name__ == "__main__":
    main()


# ---------------------------------------------------------------------------
# Morphotactic grammar: generalizes guardrails like "-LIKE adjectives do not
# take comparative -ER". Each suffix declares input categories, an output
# category, an optional phonological gate, and terminality. Real lexicon
# words get categories by surface parse of their own morphology (with prefix
# stripping), so WIRELIKE categorizes as a non-gradable adjective and blocks
# WIRELIKER even though WIRELIKE is a valid word. Validated: blocks
# generator junk while passing 94% of real affix-class phonies.
VOWELS_Y = set("AEIOUY")
DOUBLABLE = set("BDGLMNPRT")


def syllables(word):
    n = 0
    prev = False
    for ch in word:
        v = ch in VOWELS_Y
        if v and not prev:
            n += 1
        prev = v
    return max(1, n)


def comparative_gate(stem):
    s = syllables(stem)
    return s == 1 or (s == 2 and (stem[-1:] in ("Y", "W", "R")
                                  or stem.endswith("LE")))


GRAMMAR = {
    "LIKE": ({"N", "ANY"}, "ADJN", None, False),
    "ISH": ({"N", "ADJ", "ADJG", "ANY"}, "ADJN", None, False),
    "Y": ({"N", "ANY"}, "ADJG", None, False),
    "FUL": ({"N", "ANY"}, "ADJ", None, False),
    "LESS": ({"N", "ANY"}, "ADJ", None, False),
    "OID": ({"N", "ANY"}, "ADJ", None, False),
    "CMP": ({"ADJG", "ANY"}, "ADJ", comparative_gate, True),
    "EST": ({"ADJG", "ANY"}, "ADJ", comparative_gate, True),
    "LY": ({"ADJ", "ADJG", "ADJN", "ANY"}, "ADV", None, True),
    "NESS": ({"ADJ", "ADJG", "ADJN", "ANY"}, "N", None, False),
    "ISM": ({"N", "ADJ", "ANY"}, "N", None, False),
    "IST": ({"N", "ADJ", "ANY"}, "N", None, False),
    "AGE": ({"N", "V", "ANY"}, "N", None, False),
    "ITIS": ({"N", "ANY"}, "N", None, False),
    "AGT": ({"V", "N", "ANY"}, "N", None, False),
    "ED": ({"V", "N", "ANY"}, "PART", None, True),
    "ING": ({"V", "N", "ANY"}, "GER", None, True),
    "INGS": ({"V", "N", "ANY"}, "PL", None, True),
    "S": ({"N", "V", "ADJG", "ANY"}, "PL", None, True),
    "ES": ({"N", "V", "ANY"}, "PL", None, True),
    "ERS": ({"V", "N", "ANY"}, "PL", None, True),
}
GRAMMAR_SUFFIXES = [
    ("INGS", "INGS"), ("NESS", "NESS"), ("LESS", "LESS"), ("LIKE", "LIKE"),
    ("ITIS", "ITIS"), ("IEST", "EST"), ("FUL", "FUL"), ("ISH", "ISH"),
    ("ISM", "ISM"), ("IST", "IST"), ("AGE", "AGE"), ("OID", "OID"),
    ("ERS", "ERS"), ("IER", "CMP"), ("ILY", "LY"), ("ING", "ING"),
    ("EST", "EST"), ("ED", "ED"), ("LY", "LY"), ("ES", "ES"),
    ("ER", "CMP,AGT"), ("Y", "Y"), ("S", "S"), ("D", "ED"),
]


def grammar_stem_variants(word, suf):
    stem = word[: len(word) - len(suf)]
    outs = [(stem, False)]
    if suf in ("ING", "ED", "INGS", "ERS", "EST", "ER"):
        outs.append((stem + "E", False))
        if len(stem) >= 2 and stem[-1] == stem[-2]:
            outs.append((stem[:-1], stem[-1] not in DOUBLABLE))
    if suf in ("IER", "IEST", "ILY", "IES"):
        outs = [(stem + "Y", False)]
    if suf == "LY":
        outs.append((stem + "E", False))   # -LE -> -LY (possible/possibly)
    if suf == "D":
        outs = [(stem, False)] if stem.endswith("E") else [(stem + "E", False)]
    return outs


def word_category(word, lexset, depth=0):
    """Category of a real word by surface parse (prefixes stripped)."""
    if depth > 2:
        return "ANY"
    for pre in PREFIXES:
        if word.startswith(pre) and word[len(pre):] in lexset \
                and len(word) - len(pre) >= 2:
            return word_category(word[len(pre):], lexset, depth + 1)
    for suf, keys in GRAMMAR_SUFFIXES:
        if not word.endswith(suf) or len(word) - len(suf) < 2:
            continue
        for key in keys.split(","):
            _needs, makes, gate, _term = GRAMMAR[key]
            for stem, bad in grammar_stem_variants(word, suf):
                if bad or (gate and not gate(stem)):
                    continue
                if stem in lexset:
                    return makes
    return "ANY"


def legal_derivation(word, lexset, depth=0):
    """Morphotactically legal affix chain deriving word, or None."""
    if depth > 2:
        return None
    for pre in PREFIXES:
        rest = word[len(pre):]
        if word.startswith(pre) and len(rest) >= 2 and rest in lexset:
            return ["PRE:" + pre]
    for suf, keys in GRAMMAR_SUFFIXES:
        if not word.endswith(suf) or len(word) - len(suf) < 2:
            continue
        for key in keys.split(","):
            needs, _makes, gate, _term = GRAMMAR[key]
            for stem, bad in grammar_stem_variants(word, suf):
                if bad or (gate and not gate(stem)):
                    continue
                if stem in lexset:
                    cat = word_category(stem, lexset)
                    if cat in ("ADV", "PART", "GER", "PL"):
                        continue
                    if cat in needs or ("ANY" in needs and cat == "ANY"):
                        return [key]
                inner = legal_derivation(stem, lexset, depth + 1)
                if inner and not inner[-1].startswith("PRE:"):
                    ikey = inner[-1]
                    _n2, imakes, _g2, iterm = GRAMMAR[ikey]
                    if not iterm and imakes in needs:
                        return inner + [key]
    return None
