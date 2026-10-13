# One English word codebook: CSW24 pilot

The dictionary-free position-link format is ready to use without any download.
A separate offline prototype trained on **one million native CSW24 games**
reduces average token length by about 25% on CSW24 and 23% on NWL23. Both use
exactly the same CSW24-trained codebook; the lexicon remains a position field.
The prototype is not enabled in the browser.

## Corpus and validation

Native static-equity selfplay produced 48,537,742 word occurrences and 172,959
distinct words. We count every newly formed maximal word of length two or more,
including crosswords, and normalize designated blanks to their letters. WMP is
enabled; WIT and RIT are disabled. Endgame and preendgame choices also use
static equity, not full solves. This is a play-frequency model, not a claim
about optimal play or human word frequencies.

Four disjoint 250,000-game seed ranges form the training corpus. Independent
10,000-game runs for each lexicon supply evaluation positions: every 100th game
at turns 8, 16, 24, and game end (one snapshot when checkpoints coincide).
That yielded 309 CSW24 and 319 NWL23 positions from 100 games per lexicon.
No evaluation counts enter the codebook. The native engine source is identical
between f63937b6 and the checkout base dab7e7f2; the latter only changes browser
memory probing and its tests. Input data hashes and measured results are in
[position-word-pilot-results.json](position-word-pilot-results.json).

Both experimental board bitstreams are encoded and decoded for every sampled
position, with overlap consistency checked. An additional synthetic board
exercises unknown words and isolated letters. Reported URL lengths include the
version-1 metadata, blank markers, rack letters, byte padding, checksum, and
base64url expansion. The prototype round-trips board letters; metadata uses
the existing format and is counted, not reimplemented. Separately, all 628
positions round-tripped through the complete production version-1 codec.

## Results

Mean token lengths, excluding the URL prefix and `?p=`:

| CSW24 codebook | Gzipped JSON bytes | CSW24 characters | NWL23 characters |
|---|---:|---:|---:|
| No codebook: packed version 1 | 0 | 117.53 | 117.03 |
| Most frequent 4,096 words | 12,297 | 116.02 | 115.33 |
| Most frequent 16,384 words | 50,957 | 111.86 | 110.66 |
| Most frequent 65,536 words | 207,355 | 97.22 | 98.19 |
| All 172,959 observed words | 604,406 | 88.49 | 90.21 |

The full-codebook hybrid's 95th percentile is 116 characters for CSW24 and 118
for NWL23. The packed fallback still fits within 135 characters with the prototype’s
two-bit mode selector, because it fits in the existing final-byte padding.
The full canonical codebook is 2,693,916 bytes before gzip. Browser parse and
lookup memory would cost more than the compressed download and has not been
measured. Raw DEFLATE of these CSW24 CGPs averages 174.36 base64url characters
even without adding a checksum.

## How the prototype works

Canonical Huffman lengths come from played-word counts. Candidate words are
maximal horizontal and vertical runs on the final board. A greedy cover chooses
the word with the greatest saving over its currently uncovered letters, then
repeats. Crossings can be covered more than once; conflicting letters are
rejected. Unknown words, uncovered parts of crosswords, and isolated letters
fall back to individual tiles. Blank identity is stored separately.

Two layouts were measured:

1. **Bitmap plus words:** retain the 225-bit occupancy map. Each word references
   a deterministic run index, followed by its Huffman code. A stop bit ends
   words; remaining letters are written in board order. Full-codebook CSW24
   average: 108.13 characters.
2. **Words with coordinates:** omit occupancy. Each word stores a type bit,
   eight-bit starting square, direction bit, and Huffman code. A literal stores
   a type bit, square, and five-bit letter. An eight-bit token count terminates
   the stream. Full-codebook CSW24 average: 88.10 characters.

The table uses the shortest of these and the packed board, plus a two-bit mode
selector. Common short words dominate raw play counts, but their placement
overhead explains why small codebooks save relatively little.

## Configurable optimization budget

`--search-ms 20` (the default) allows up to 20 milliseconds of extra cover
search per position, divided equally between the bitmap and coordinate codecs.
Use `--search-ms 0` to reproduce the original greedy results above, or a larger
budget for a more thorough search. This is a soft wall-clock budget for search
only: codebook loading, baseline construction, encoding, and validation happen
outside it. Deadline checks occur between search nodes, so one node and process
scheduling can overrun the deadline slightly. Decoding requires no search.

A deterministic depth-first branch-and-bound search starts with the greedy
encoding as its incumbent. Each branch either includes or excludes a candidate
word, so horizontal-only, vertical-only, and mixed-orientation covers compete
on actual bit cost including placement and literals. Overlapping squares earn
literal savings only once. An optimistic bound sums the remaining positive
marginal savings while ignoring overlaps; branches that cannot improve the
incumbent are pruned. On timeout, the smallest complete cover found is returned,
never a partial search state or something larger than the greedy baseline.

Mean full-codebook hybrid token lengths on the same held-out positions:

| Extra search budget per position | CSW24 | NWL23 |
|---|---:|---:|
| 0 ms (greedy) | 88.49 | 90.21 |
| 20 ms | 88.33 | 89.97 |
| 200 ms | 88.32 | 89.97 |

At 200 ms every sampled layout finished searching: 618/618 CSW24 and 638/638
NWL23. Actual extra search averaged about 2.5 and 3.0 milliseconds per position
on this machine, because completed searches stop early. The greedy cover was
already close to optimal on this sample, but the search removes the assumption
that the locally best word is globally best. These are optima **within the
current candidate set and layouts** (maximal board words present in the
codebook), not across all possible compression schemes or dictionary subwords.
Timing and timeout completion counts depend on the machine.

Validation includes exhaustive enumeration of small synthetic cover problems,
a known greedy counterexample, deterministic timeout/invalid-budget checks,
and actual mixed-axis/literal bitstream round trips. Run:

```sh
python3 -m unittest discover -s wasmentry/qa -p test_word_codec_pilot.py -v
```

## Next decision

The full codebook buys about 29 characters per link at a 604 KB gzip download.
The 65,536-word model is a possible compromise, saving about 19–20 characters
for 207 KB. Keep this separate from the initial no-download format until that
tradeoff is wanted. A shipped model must have an immutable version/fingerprint
so links never change meaning when frequencies are retrained. It must retain
literal fallback and strictly bound decoding, positions, overlaps, and word
lengths; the research decoder is not hardened for untrusted URLs.

Further experiments could optimize dictionary entries for net bits saved,
train frequencies on selected covers rather than all played words, encode
coordinate deltas, or extend the candidate set to dictionary subwords.
Larger and more diverse held-out games are needed before choosing a final
codec. The single CSW24 model already works for NWL23 without assuming that
every NWL23 word appeared in training.

## Reproduce

From a checkout with native lexicon/leave/WMP data installed:

```sh
make -f wasmentry/qa/word-frequency-pilot.mk word-frequency-pilot BUILD=no_pgo_release
mkdir -p /tmp/magpie-word-corpus
for shard in 0 1 2 3; do
  ./bin/word-frequency-pilot "$((shard * 250000))" 250000 \
    "/tmp/magpie-word-corpus/train-$shard.tsv" \
    "/tmp/magpie-word-corpus/train-$shard.cgp" CSW24
done
./bin/word-frequency-pilot 1000000 10000 /tmp/magpie-word-corpus/heldout.tsv /tmp/magpie-word-corpus/heldout.cgp CSW24
./bin/word-frequency-pilot 1010000 10000 /tmp/magpie-word-corpus/nwl-heldout.tsv /tmp/magpie-word-corpus/nwl-heldout.cgp NWL23
python3 wasmentry/qa/word-codec-pilot.py \
  --counts /tmp/magpie-word-corpus/train-*.tsv \
  --positions /tmp/magpie-word-corpus/heldout.cgp \
  --search-ms 0 \
  --output /tmp/magpie-word-corpus/csw-results.json
```

Repeat with `nwl-heldout.cgp`; add `--limit 4096`, `16384`, or `65536` for the
smaller models. Shards may run concurrently if resources allow. The aggregate
training counts, sorted alphabetically as `WORD<TAB>COUNT<LF>`, have SHA-256
`73bab4fa5ba8ffba6ce6b6d3381f1329e6f6e5368eb7a00b54539430171699d2`.
