# Wordlist subset selection for small lexica

Greedily selects the highest-value subset of a lexicon that fits a target
dictionary size, for small/embedded targets (retro engines, 2^17-edge formats,
etc.). It maximizes *playing strength per byte* rather than capping by word
length: it front-loads the cheap, high-value words (Q-without-U plays, bingo
stems that share existing prefixes) and starves the low-value mid-length words.

## Pipeline

1. **Dump positions** (one autoplay pass; the expensive step, done once):

   ```
   magpie
   set -lex CSW24 -k1 CSW24 -k2 CSW24 -threads N -gp false
   playability <num_games> -smallkwg CSW24_2to3 -pbdump cache/pb
   ```

   This writes `cache/pb.<n>` (per worker) and `cache/pb.words`. Each position
   record holds the seed baseline equity and the moves above it with their
   formed-word ids. `-smallkwg` is the seed set whose baseline the bonus is
   measured against (e.g. all 2- and 3-letter words).

2. **Run the greedy** (offline, seconds; no re-simulation as the set grows):

   ```
   cc -O2 -o offline_greedy tools/offline_greedy.c
   ./offline_greedy cache/pb.words <batch> order.txt cache/pb.*[0-9]
   ```

   `order.txt` is the selection order: the seed words, then the chosen 4+ letter
   words in greedy (value-per-byte) order. The greedy recomputes each candidate's
   marginal bonus over the current set from the cache (so the value signal can use
   millions of games), divides by its prefix-sharing node cost, and stops when no
   remaining word improves play.

3. **Materialize at a byte budget**: take a prefix of `order.txt`, **sort it**
   (the DAWG builder needs sorted input), and build the dictionary:

   ```
   sort <(head -N order.txt) > subset.txt   # placed under data/lexica/
   magpie ; set -lex CSW24 ; convert text2acdawg subset
   ```

   Binary-search `N` for the acdawg size you want.

The bonus metric and the `-pbdump` cache come from `src/impl/word_playability.c`.
