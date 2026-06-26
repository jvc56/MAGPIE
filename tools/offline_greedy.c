// Offline value-per-byte dictionary subset selection from a playability cache.
//
// Reads a word list (<prefix>.words, id = 0-based line) and one or more binary
// position dumps (<prefix>.<n>) produced by `playability ... -pbdump`. Runs the
// greedy: each batch recomputes, for the current selected set S, every
// candidate word's marginal bonus over S (re-derived from the cached positions,
// no autoplay), divides by its prefix-sharing node cost, and adds the top
// batch. The seed (all 2- and 3-letter words) is always available.
//
// Emits the selection order (seed words, then chosen 4+ words in greedy order)
// to the output file, one word per line, so any byte budget can be sliced by
// building the acdawg of a prefix of the order.
//
// Usage: offline_greedy <words> <batch> <out_order> <dump1> [dump2 ...]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p) { fprintf(stderr, "OOM %zu\n", n); exit(1); }
  return p;
}

static void *xrealloc(void *p, size_t n) {
  p = realloc(p, n);
  if (!p) { fprintf(stderr, "OOM %zu\n", n); exit(1); }
  return p;
}

// --- word list ---
static char **g_word;   // word[i] = string for id i
static int *g_len;      // length of word i
static int g_nwords;

static void load_words(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  int cap = 1 << 18;
  g_word = xmalloc((size_t)cap * sizeof(char *));
  g_len = xmalloc((size_t)cap * sizeof(int));
  char line[128];
  g_nwords = 0;
  while (fgets(line, sizeof(line), f)) {
    int L = (int)strlen(line);
    while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r')) L--;
    line[L] = '\0';
    if (g_nwords == cap) {
      cap *= 2;
      g_word = xrealloc(g_word, (size_t)cap * sizeof(char *));
      g_len = xrealloc(g_len, (size_t)cap * sizeof(int));
    }
    g_word[g_nwords] = xmalloc((size_t)L + 1);
    memcpy(g_word[g_nwords], line, (size_t)L + 1);
    g_len[g_nwords] = L;
    g_nwords++;
  }
  fclose(f);
}

// --- cached positions (flat) ---
typedef struct { int32_t baseline; uint32_t mstart; uint16_t mcount; } Pos;
typedef struct { int32_t equity; uint32_t wstart; uint8_t wcount; } Mv;
static Pos *g_pos; static long g_npos, g_pos_cap;
static Mv *g_mv; static long g_nmv, g_mv_cap;
static uint32_t *g_mvw; static long g_nmvw, g_mvw_cap; // 4+ word ids only

static void load_dump(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  int32_t baseline; uint16_t nmoves;
  while (fread(&baseline, sizeof(int32_t), 1, f) == 1) {
    if (fread(&nmoves, sizeof(uint16_t), 1, f) != 1) break;
    if (g_npos == g_pos_cap) {
      g_pos_cap = g_pos_cap ? g_pos_cap * 2 : (1 << 20);
      g_pos = xrealloc(g_pos, (size_t)g_pos_cap * sizeof(Pos));
    }
    Pos *p = &g_pos[g_npos];
    p->baseline = baseline;
    p->mstart = (uint32_t)g_nmv;
    p->mcount = nmoves;
    for (int m = 0; m < nmoves; m++) {
      int32_t equity; uint8_t nwords;
      if (fread(&equity, sizeof(int32_t), 1, f) != 1) { fclose(f); return; }
      if (fread(&nwords, sizeof(uint8_t), 1, f) != 1) { fclose(f); return; }
      uint32_t ids[16];
      if (nwords > 16) { fprintf(stderr, "nwords too big\n"); exit(1); }
      if (fread(ids, sizeof(uint32_t), nwords, f) != nwords) { fclose(f); return; }
      // keep only 4+ letter ids (2-3 letter words are always in the seed)
      uint8_t kept = 0;
      uint32_t keep[16];
      for (int k = 0; k < nwords; k++) {
        if (g_len[ids[k]] >= 4) keep[kept++] = ids[k];
      }
      if (g_nmv == g_mv_cap) {
        g_mv_cap = g_mv_cap ? g_mv_cap * 2 : (1 << 21);
        g_mv = xrealloc(g_mv, (size_t)g_mv_cap * sizeof(Mv));
      }
      Mv *mv = &g_mv[g_nmv];
      mv->equity = equity;
      mv->wstart = (uint32_t)g_nmvw;
      mv->wcount = kept;
      while (g_nmvw + kept > g_mvw_cap) {
        g_mvw_cap = g_mvw_cap ? g_mvw_cap * 2 : (1 << 22);
        g_mvw = xrealloc(g_mvw, (size_t)g_mvw_cap * sizeof(uint32_t));
      }
      memcpy(&g_mvw[g_nmvw], keep, (size_t)kept * sizeof(uint32_t));
      g_nmvw += kept;
      g_nmv++;
    }
    g_npos++;
  }
  fclose(f);
}

// --- prefix-sharing node-cost trie (A-Z) ---
static int (*g_trie)[26]; static int g_trie_nodes, g_trie_cap;
static int trie_new_node(void) {
  if (g_trie_nodes == g_trie_cap) {
    g_trie_cap = g_trie_cap ? g_trie_cap * 2 : (1 << 20);
    g_trie = xrealloc(g_trie, (size_t)g_trie_cap * sizeof(*g_trie));
  }
  memset(g_trie[g_trie_nodes], 0, sizeof(g_trie[0])); // 0 = no child
  return g_trie_nodes++;
}

static void trie_add(const char *w, int L) {
  int node = 0;
  for (int i = 0; i < L; i++) {
    int c = w[i] - 'A';
    if (c < 0 || c >= 26) return;
    if (!g_trie[node][c]) g_trie[node][c] = trie_new_node();
    node = g_trie[node][c];
  }
}

static int trie_cost(const char *w, int L) { // new nodes = L - shared prefix
  int node = 0, shared = 0;
  for (int i = 0; i < L; i++) {
    int c = w[i] - 'A';
    if (c < 0 || c >= 26 || !g_trie[node][c]) break;
    node = g_trie[node][c]; shared++;
  }
  return L - shared;
}

typedef struct { double ratio; int w; } Cand;
static int cand_cmp(const void *a, const void *b) {
  double ra = ((const Cand *)a)->ratio, rb = ((const Cand *)b)->ratio;
  return ra < rb ? 1 : (ra > rb ? -1 : 0); // descending by ratio
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s <words> <batch> <out_order> <dump...>\n", argv[0]);
    return 1;
  }
  const char *words_path = argv[1];
  int batch = atoi(argv[2]);
  const char *out_path = argv[3];
  load_words(words_path);
  fprintf(stderr, "words: %d\n", g_nwords);
  for (int a = 4; a < argc; a++) { load_dump(argv[a]); }
  fprintf(stderr, "positions: %ld, moves: %ld, move-words(4+): %ld\n",
          g_npos, g_nmv, g_nmvw);

  // in_S over 4+ words; seed (len<=3) is implicitly always available.
  char *in_S = calloc((size_t)g_nwords, 1);
  int64_t *bonus = xmalloc((size_t)g_nwords * sizeof(int64_t));
  uint32_t *stamp = calloc((size_t)g_nwords, sizeof(uint32_t)); // per-pos dedup
  if (!in_S || !stamp) { fprintf(stderr, "OOM\n"); return 1; }
  uint32_t gen = 0;

  // trie seeded with all 2-3 letter words (their prefixes are free)
  trie_new_node(); // root = 0
  for (int i = 0; i < g_nwords; i++) if (g_len[i] <= 3) trie_add(g_word[i], g_len[i]);

  // output order: seed words first
  FILE *out = fopen(out_path, "w");
  if (!out) { fprintf(stderr, "cannot open %s\n", out_path); return 1; }
  for (int i = 0; i < g_nwords; i++) if (g_len[i] <= 3) fprintf(out, "%s\n", g_word[i]);

  long selected = 0;
  for (;;) {
    memset(bonus, 0, (size_t)g_nwords * sizeof(int64_t));
    // recompute bonus over current S
    for (long pi = 0; pi < g_npos; pi++) {
      Pos *p = &g_pos[pi];
      int64_t baseline = p->baseline;
      gen++;
      // collect missing==1 candidates until first all-in-S move
      uint32_t cw[64]; int32_t ceq[64]; int nc = 0;
      for (int m = 0; m < p->mcount; m++) {
        Mv *mv = &g_mv[p->mstart + m];
        int missing = 0; uint32_t mw = 0;
        for (int k = 0; k < mv->wcount; k++) {
          uint32_t w = g_mvw[mv->wstart + k];
          if (!in_S[w]) { missing++; mw = w; if (missing >= 2) break; }
        }
        if (missing == 0) { if (mv->equity > baseline) baseline = mv->equity; break; }
        if (missing == 1 && nc < 64) { cw[nc] = mw; ceq[nc] = mv->equity; nc++; }
      }
      for (int k = 0; k < nc; k++) {
        uint32_t w = cw[k];
        if (stamp[w] == gen) continue; // first (highest) per word per position
        stamp[w] = gen;
        if (ceq[k] > baseline) bonus[w] += (int64_t)ceq[k] - baseline;
      }
    }
    // Rank all positive-bonus 4+ candidates by bonus/cost (cost from the trie
    // as it stands at the start of the batch), take the top `batch`.
    static Cand *cand = NULL; static int cand_cap = 0;
    int ncand = 0;
    for (int w = 0; w < g_nwords; w++) {
      if (g_len[w] < 4 || in_S[w] || bonus[w] <= 0) continue;
      int cost = trie_cost(g_word[w], g_len[w]); if (cost < 1) cost = 1;
      if (ncand == cand_cap) {
        cand_cap = cand_cap ? cand_cap * 2 : (1 << 16);
        cand = xrealloc(cand, (size_t)cand_cap * sizeof(Cand));
      }
      cand[ncand].ratio = (double)bonus[w] / cost;
      cand[ncand].w = w;
      ncand++;
    }
    if (ncand == 0) break; // converged: no value-adding words remain
    qsort(cand, (size_t)ncand, sizeof(Cand), cand_cmp);
    int take = ncand < batch ? ncand : batch;
    for (int i = 0; i < take; i++) {
      int w = cand[i].w;
      in_S[w] = 1;
      trie_add(g_word[w], g_len[w]);
      fprintf(out, "%s\n", g_word[w]);
      selected++;
    }
    fprintf(stderr, "selected %ld (+%d, %d candidates) ...\n", selected, take,
            ncand);
  }
  fclose(out);
  fprintf(stderr, "DONE selected %ld 4+ words\n", selected);
  return 0;
}
