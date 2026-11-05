#!/usr/bin/env python3
"""Build per-word study features for Letterbox's Word Walls filters and sorts.

Writes letterbox/csw24_features.tsv with one row per CSW24 word:

    WORD  FIRST_CSW  IN_NWL  LEMMA_PER_MILLION  SURPRISE

FIRST_CSW          first CSW edition containing the word (2007 means CSW07 or
                   earlier)
IN_NWL             1 if the word is also valid in NWL23
LEMMA_PER_MILLION  English text frequency of the word's lemma family: each
                   form's frequency is the max of SUBTLEX-US and Google web
                   unigrams (per million), summed over every CSW24 form that
                   shares a lemma. A word with several possible lemmas takes
                   the most frequent family. A form can only inherit up to
                   INHERIT_RATIO times its own frequency (floored at
                   INHERIT_FLOOR), so homographs of common words that are
                   themselves rare (JUSTING, THEEING, WELLING) don't borrow
                   the frequency of JUST, THE or WELL.
SURPRISE           how unexpected the word's letter ordering is, in bits:
                   letter-trigram surprisal (with ^^ start and $ end padding,
                   interpolated with bigrams and unigrams, trained on CSW24)
                   minus the unigram surprisal of its letters. Subtracting the
                   unigram term means rare letters alone (Q, Z, ...) don't make
                   a word "surprising"; only their arrangement does.

Usage (lemminflect provides lemmas; nothing else is needed beyond stdlib):
    uv run --python 3.12 --with lemminflect letterbox/tools/build_word_features.py \\
        <data/lexica dir> <annotated-corpus/wordfreq dir>
"""
import collections
import math
import pathlib
import sys

from lemminflect import getAllLemmas

CSW_EDITIONS = [(2007, "CSW07"), (2012, "CSW12"), (2015, "CSW15"),
                (2019, "CSW19"), (2021, "CSW21"), (2024, "CSW24")]
NWL = "NWL23"

# Interpolation weights for trigram, bigram and unigram letter models
LAMBDAS = (0.75, 0.2, 0.05)
START = "^"

# Limit on how much frequency a form can inherit from its lemma family
INHERIT_RATIO = 20
INHERIT_FLOOR = 0.01  # per million
END = "$"


def read_lexicon(lexica, name):
    with open(lexica / f"{name}.txt") as fh:
        return {line.strip().upper() for line in fh if line.strip()}


def read_frequencies(wordfreq):
    """Per-million frequency of each form: max of SUBTLEX-US and Google web."""
    freq = collections.defaultdict(float)
    with open(wordfreq / "SUBTLEXus74286wordstextversion.txt") as fh:
        next(fh)
        for line in fh:
            parts = line.split("\t")
            word = parts[0].upper()
            freq[word] = max(freq[word], float(parts[5]))
    with open(wordfreq / "count_1w.txt") as fh:
        for line in fh:
            word, count = line.split("\t")
            word = word.upper()
            # count_1w comes from ~1T tokens, so count / 1e6 is per million
            freq[word] = max(freq[word], int(count) / 1e6)
    return freq


def suffix_lemmas(word, lexicon):
    """Fallback for words lemminflect doesn't know: strip a regular inflection,
    accepting the stem only if it's itself a valid word."""
    candidates = []
    if word.endswith("IES"):
        candidates.append(word[:-3] + "Y")
    if word.endswith("ES"):
        candidates.append(word[:-2])
    if word.endswith("S") and not word.endswith("SS"):
        candidates.append(word[:-1])
    if word.endswith("ED"):
        candidates += [word[:-2], word[:-1]]
        if len(word) >= 5 and word[-3] == word[-4]:
            candidates.append(word[:-3])
    if word.endswith("ING") and len(word) >= 6:
        candidates += [word[:-3], word[:-3] + "E"]
        if word[-4] == word[-5]:
            candidates.append(word[:-4])
    return [c for c in candidates if len(c) >= 2 and c in lexicon]


def lemmas_for(word, lexicon):
    found = getAllLemmas(word.lower())
    lemmas = {lemma.upper() for forms in found.values() for lemma in forms}
    if not lemmas:
        lemmas = set(suffix_lemmas(word, lexicon))
    lemmas.add(word)  # Every word is at least in its own family
    return lemmas


def train_letter_model(words):
    tri = collections.Counter()
    tri_ctx = collections.Counter()
    bi = collections.Counter()
    bi_ctx = collections.Counter()
    uni = collections.Counter()
    for w in words:
        s = START + START + w + END
        for i in range(2, len(s)):
            tri[s[i - 2:i + 1]] += 1
            tri_ctx[s[i - 2:i]] += 1
            bi[s[i - 1:i + 1]] += 1
            bi_ctx[s[i - 1]] += 1
            uni[s[i]] += 1
    total = sum(uni.values())
    symbols = len(uni)

    def prob(context, c):
        p_uni = (uni[c] + 1) / (total + symbols)
        p_bi = bi[context[-1] + c] / bi_ctx[context[-1]] if bi_ctx[context[-1]] else p_uni
        p_tri = tri[context + c] / tri_ctx[context] if tri_ctx[context] else p_bi
        return LAMBDAS[0] * p_tri + LAMBDAS[1] * p_bi + LAMBDAS[2] * p_uni, p_uni

    def surprise(w):
        s = START + START + w + END
        bits = 0.0
        for i in range(2, len(s)):
            p, p_uni = prob(s[i - 2:i], s[i])
            bits -= math.log2(p)
            if s[i] != END:
                bits += math.log2(p_uni)
        return bits

    return surprise


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    lexica = pathlib.Path(sys.argv[1])
    wordfreq = pathlib.Path(sys.argv[2])
    out = pathlib.Path(__file__).resolve().parent.parent / "csw24_features.tsv"

    csw = read_lexicon(lexica, "CSW24")
    first_edition = {}
    for year, name in CSW_EDITIONS:
        for w in read_lexicon(lexica, name):
            first_edition.setdefault(w, year)
    nwl = read_lexicon(lexica, NWL)
    freq = read_frequencies(wordfreq)

    word_lemmas = {w: lemmas_for(w, csw) for w in csw}
    family_freq = collections.defaultdict(float)
    for w, lemmas in word_lemmas.items():
        for lemma in lemmas:
            family_freq[lemma] += freq.get(w, 0.0)

    surprise = train_letter_model(csw)

    with open(out, "w") as fh:
        for w in sorted(csw):
            own = freq.get(w, 0.0)
            family = max(family_freq[lemma] for lemma in word_lemmas[w])
            lemma_freq = max(own, min(family, max(own, INHERIT_FLOOR) * INHERIT_RATIO))
            fh.write(f"{w}\t{first_edition.get(w, 2024)}\t{int(w in nwl)}\t"
                     f"{lemma_freq:.4g}\t{surprise(w):.2f}\n")
    print(f"{len(csw)} words -> {out}")


if __name__ == "__main__":
    main()
