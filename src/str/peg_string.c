#include "peg_string.h"

#include "../compat/ctime.h"
#include "../def/peg_defs.h"
#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../impl/peg.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "move_string.h"
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One displayable outcome token: a draw rendered as the mover's drawn multiset
// followed by the bag remainder -- a sorted multiset ("DH/RS") when its
// orderings share a bucket, or "/"-segmented ("DH/R/S") when they split --
// tagged with its bucket and summed labeled-ordering weight.
typedef struct {
  char text[64];
  int bucket; // 0 = win, 1 = loss, 2 = tie
  int64_t weight;
} PegOutcomeTok;

// Per-draw family: the rows sharing the mover's drawn combo AND the
// bag-remainder multiset. The mover draws its tiles together, so the drawn
// combo is always an order-independent multiset (the leading segment); only the
// bag remainder's ordering varies across the family's rows. Tracks which
// buckets those orderings landed in, the labeled count per ordering (constant
// within a family), and the summed weight of all its rows.
typedef struct {
  char drawn[40];  // sorted mover-draw multiset (leading segment)
  char rem_ms[40]; // sorted bag-remainder multiset (family key tail)
  bool seen[3];    // [0]=win [1]=loss [2]=tie
  int64_t per_ordering;
  int64_t total_weight;
} PegOutcomeFamily;

// A growable list of short strings (sequences / segmented forms).
typedef struct {
  char **items;
  int len;
  int cap;
} PegStrList;

static void peg_strlist_push(PegStrList *list, const char *s) {
  if (list->len == list->cap) {
    list->cap = list->cap > 0 ? list->cap * 2 : 8;
    list->items =
        realloc_or_die(list->items, (size_t)list->cap * sizeof(char *));
  }
  list->items[list->len++] = string_duplicate(s);
}

static bool peg_strlist_has(const PegStrList *list, const char *s) {
  for (int item_idx = 0; item_idx < list->len; item_idx++) {
    if (strcmp(list->items[item_idx], s) == 0) {
      return true;
    }
  }
  return false;
}

static void peg_strlist_push_unique(PegStrList *list, const char *s) {
  if (!peg_strlist_has(list, s)) {
    peg_strlist_push(list, s);
  }
}

static void peg_strlist_destroy(PegStrList *list) {
  for (int item_idx = 0; item_idx < list->len; item_idx++) {
    free(list->items[item_idx]);
  }
  free(list->items);
  list->items = NULL;
  list->len = 0;
  list->cap = 0;
}

// Sort a short tile string in place (insertion sort). A no-op on the empty
// string (a pass draws nothing; a play that empties the bag leaves no
// remainder), where there is no s[0] to seed the sort from.
static void peg_sort_str(char *s) {
  if (s[0] == '\0') {
    return;
  }
  for (int char_idx = 1; s[char_idx] != '\0'; char_idx++) {
    const char key = s[char_idx];
    int prev = char_idx - 1;
    while (prev >= 0 && s[prev] > key) {
      s[prev + 1] = s[prev];
      prev--;
    }
    s[prev + 1] = key;
  }
}

// Number of distinct permutations of the sorted multiset string s.
static int64_t peg_perm_count(const char *s) {
  const int len = (int)strlen(s);
  int64_t count = 1;
  for (int factor = 2; factor <= len; factor++) {
    count *= factor;
  }
  int run = 1;
  for (int char_idx = 1; char_idx <= len; char_idx++) {
    if (char_idx < len && s[char_idx] == s[char_idx - 1]) {
      run++;
    } else {
      int64_t run_fact = 1;
      for (int factor = 2; factor <= run; factor++) {
        run_fact *= factor;
      }
      count /= run_fact;
      run = 1;
    }
  }
  return count;
}

// Distinct orderings a segmented form ("F/GH/I") represents: the product over
// its "/"-separated (sorted) segments of each segment's permutation count.
static int64_t peg_form_covered(const char *form) {
  int64_t covered = 1;
  // Zero-initialized so the analyzer sees every byte as defined: each segment
  // is null-terminated at seg_len before peg_perm_count reads it, but the
  // analyzer can't tie strlen(seg) to the written prefix and flags the read as
  // a garbage value. Behaviorally identical (bytes past the terminator are
  // never read).
  char seg[40] = {0};
  int seg_len = 0;
  for (const char *cursor = form;; cursor++) {
    if (*cursor == '/' || *cursor == '\0') {
      seg[seg_len] = '\0';
      if (seg_len > 0) {
        covered *= peg_perm_count(seg);
      }
      seg_len = 0;
      if (*cursor == '\0') {
        break;
      }
    } else if (seg_len < (int)sizeof(seg) - 1) {
      seg[seg_len++] = *cursor;
    }
  }
  return covered;
}

// Factor a set of distinct, equal-length tile sequences (all the same multiset
// and outcome bucket) into the coarsest segmented forms: a maximal contiguous
// block whose orderings are all present collapses into one sorted multiset
// segment ("GH"), while order-significant boundaries stay "/"-separated. The
// returned forms (e.g. "F/GH/I") partition the input. Recurses on the leftmost
// boundary where the set factors as a cartesian product L x R.
static PegStrList peg_factor(const PegStrList *seqs) {
  PegStrList out = {0};
  if (seqs->items == NULL || seqs->len == 0) {
    return out;
  }
  const int width = (int)strlen(seqs->items[0]);
  if (width == 1) {
    for (int seq_idx = 0; seq_idx < seqs->len; seq_idx++) {
      peg_strlist_push_unique(&out, seqs->items[seq_idx]);
    }
    return out;
  }
  for (int split = 0; split < width - 1; split++) {
    PegStrList left = {0};
    PegStrList right = {0};
    for (int seq_idx = 0; seq_idx < seqs->len; seq_idx++) {
      char lbuf[40];
      char rbuf[40];
      memcpy(lbuf, seqs->items[seq_idx], (size_t)split + 1);
      lbuf[split + 1] = '\0';
      const int rlen = width - (split + 1);
      memcpy(rbuf, seqs->items[seq_idx] + split + 1, (size_t)rlen);
      rbuf[rlen] = '\0';
      peg_strlist_push_unique(&left, lbuf);
      peg_strlist_push_unique(&right, rbuf);
    }
    bool factors = seqs->len == left.len * right.len;
    for (int left_idx = 0; factors && left_idx < left.len; left_idx++) {
      for (int right_idx = 0; factors && right_idx < right.len; right_idx++) {
        char comb[40];
        (void)snprintf(comb, sizeof(comb), "%s%s", left.items[left_idx],
                       right.items[right_idx]);
        if (!peg_strlist_has(seqs, comb)) {
          factors = false;
        }
      }
    }
    if (factors) {
      PegStrList left_forms = peg_factor(&left);
      PegStrList right_forms = peg_factor(&right);
      for (int left_form_idx = 0; left_form_idx < left_forms.len;
           left_form_idx++) {
        for (int right_form_idx = 0; right_form_idx < right_forms.len;
             right_form_idx++) {
          char form[64];
          (void)snprintf(form, sizeof(form), "%s/%s",
                         left_forms.items[left_form_idx],
                         right_forms.items[right_form_idx]);
          peg_strlist_push(&out, form);
        }
      }
      peg_strlist_destroy(&left_forms);
      peg_strlist_destroy(&right_forms);
      peg_strlist_destroy(&left);
      peg_strlist_destroy(&right);
      return out;
    }
    peg_strlist_destroy(&left);
    peg_strlist_destroy(&right);
  }
  // Irreducible: a single permutable block when the set is all permutations of
  // one multiset; otherwise each sequence stands alone (a "/" per tile).
  char sorted0[40];
  (void)snprintf(sorted0, sizeof(sorted0), "%s", seqs->items[0]);
  peg_sort_str(sorted0);
  bool all_same_ms = true;
  for (int seq_idx = 1; all_same_ms && seq_idx < seqs->len; seq_idx++) {
    char other[40];
    (void)snprintf(other, sizeof(other), "%s", seqs->items[seq_idx]);
    peg_sort_str(other);
    if (strcmp(other, sorted0) != 0) {
      all_same_ms = false;
    }
  }
  if (all_same_ms && (int64_t)seqs->len == peg_perm_count(sorted0)) {
    peg_strlist_push(&out, sorted0);
    return out;
  }
  for (int seq_idx = 0; seq_idx < seqs->len; seq_idx++) {
    char form[64];
    int form_len = 0;
    for (int char_idx = 0; seqs->items[seq_idx][char_idx] != '\0'; char_idx++) {
      if (char_idx > 0) {
        form[form_len++] = '/';
      }
      form[form_len++] = seqs->items[seq_idx][char_idx];
    }
    form[form_len] = '\0';
    peg_strlist_push(&out, form);
  }
  return out;
}

static int peg_outcome_tok_cmp(const void *a, const void *b) {
  return strcmp(((const PegOutcomeTok *)a)->text,
                ((const PegOutcomeTok *)b)->text);
}

// 0 = win, 1 = loss, 2 = tie.
static int peg_outcome_bucket(int32_t mover_total) {
  if (mover_total > 0) {
    return 0;
  }
  if (mover_total < 0) {
    return 1;
  }
  return 2;
}

// Copy a short tile string and sort it into multiset (canonical) order.
// Single-character tiles assumed (English; blank renders as '?').
static void peg_sorted_copy(const char *src, char *dst, size_t dst_size) {
  (void)snprintf(dst, dst_size, "%s", src);
  peg_sort_str(dst);
}

// Render a draw token from the drawn-multiset prefix and a tail (a
// bag-remainder multiset or factored form), joined by "/" only when both are
// non-empty (a pass draws nothing, so the prefix is empty).
static void peg_draw_token(char *dst, size_t dst_size, const char *drawn,
                           const char *tail) {
  if (drawn[0] != '\0' && tail[0] != '\0') {
    (void)snprintf(dst, dst_size, "%s/%s", drawn, tail);
  } else {
    (void)snprintf(dst, dst_size, "%s%s", drawn, tail);
  }
}

// Append every token in `bucket` to `sb`, each preceded by a space and given an
// "xN" suffix when its labeled-ordering weight exceeds 1.
static void peg_append_outcome_toks(StringBuilder *sb,
                                    const PegOutcomeTok *toks, int n_toks,
                                    int bucket) {
  for (int tok_idx = 0; tok_idx < n_toks; tok_idx++) {
    if (toks[tok_idx].bucket != bucket) {
      continue;
    }
    if (toks[tok_idx].weight > 1) {
      string_builder_add_formatted_string(
          sb, " %sx%" PRId64, toks[tok_idx].text, toks[tok_idx].weight);
    } else {
      string_builder_add_formatted_string(sb, " %s", toks[tok_idx].text);
    }
  }
}

// Condense a candidate's per-ordering rows into one line. Each token is a draw:
// the mover's drawn tiles (always a sorted multiset, since they are drawn
// together and order-independent) as a leading segment, followed by the bag
// remainder. A play of N tiles draws N tiles, so every token begins with an
// N-tile multiset. The bag remainder is order-dependent: when all its orderings
// land in one bucket it shows as a sorted multiset ("DH/RS"); when they span
// win/loss/tie it is split into segmented forms ("DH/R/S") where contiguous
// freely-permutable blocks collapse to a sorted multiset segment and
// order-significant boundaries stay "/"-separated. Each token carries an "xN"
// labeled-ordering weight (N=1 omitted), so the win tokens' weights sum to the
// wins column and the loss tokens' to the loss column. The largest of the three
// lists is left implied (the reader infers it from the count columns); the
// smaller ones are printed, comma-separated, each with its label ("W:"/"L:"/
// "T:"). Empty lists are skipped, so a row with no tie draws just shows the
// shorter of win / loss. A tie majority is rare, so when ties are the implied
// list it is called out with a trailing ", otherwise ties". When every ordering
// shares one bucket, says "always wins/loses/ties". Caller frees.
char *peg_build_outcomes_string_rows(const PegPerScenario *rows, int n_rows) {
  if (n_rows <= 0) {
    return string_duplicate("");
  }

  // "always X" when every ordering shares a single bucket.
  bool any[3] = {false, false, false};
  for (int row_idx = 0; row_idx < n_rows; row_idx++) {
    any[peg_outcome_bucket(rows[row_idx].mover_total)] = true;
  }
  if ((int)any[0] + (int)any[1] + (int)any[2] == 1) {
    const char *all_label = "always ties";
    if (any[0]) {
      all_label = "always wins";
    } else if (any[1]) {
      all_label = "always loses";
    }
    return string_duplicate(all_label);
  }

  // Pass 1: group rows into draw families keyed by (drawn combo, bag-remainder
  // multiset). Each family records which buckets its orderings landed in, the
  // per-ordering labeled count (constant within a family), and the summed
  // weight. The drawn combo is kept sorted as the leading segment.
  PegOutcomeFamily *fams =
      malloc_or_die((size_t)n_rows * sizeof(PegOutcomeFamily));
  int n_fams = 0;
  char (*row_drawn)[40] = malloc_or_die((size_t)n_rows * sizeof(*row_drawn));
  char (*row_rem_ms)[40] = malloc_or_die((size_t)n_rows * sizeof(*row_rem_ms));
  for (int row_idx = 0; row_idx < n_rows; row_idx++) {
    peg_sorted_copy(rows[row_idx].drawn, row_drawn[row_idx],
                    sizeof(row_drawn[row_idx]));
    peg_sorted_copy(rows[row_idx].remaining, row_rem_ms[row_idx],
                    sizeof(row_rem_ms[row_idx]));
    int fam_idx = -1;
    for (int existing_idx = 0; existing_idx < n_fams; existing_idx++) {
      if (strcmp(fams[existing_idx].drawn, row_drawn[row_idx]) == 0 &&
          strcmp(fams[existing_idx].rem_ms, row_rem_ms[row_idx]) == 0) {
        fam_idx = existing_idx;
        break;
      }
    }
    if (fam_idx < 0) {
      fam_idx = n_fams++;
      (void)snprintf(fams[fam_idx].drawn, sizeof(fams[fam_idx].drawn), "%s",
                     row_drawn[row_idx]);
      (void)snprintf(fams[fam_idx].rem_ms, sizeof(fams[fam_idx].rem_ms), "%s",
                     row_rem_ms[row_idx]);
      fams[fam_idx].seen[0] = false;
      fams[fam_idx].seen[1] = false;
      fams[fam_idx].seen[2] = false;
      fams[fam_idx].per_ordering = rows[row_idx].weight;
      fams[fam_idx].total_weight = 0;
    }
    fams[fam_idx].seen[peg_outcome_bucket(rows[row_idx].mover_total)] = true;
    fams[fam_idx].total_weight += rows[row_idx].weight;
  }

  // Pass 2: emit one token per (display form, bucket). When a family's bag
  // orderings all share a bucket, the whole draw is one token (drawn multiset,
  // then the bag remainder as a multiset). When they span buckets, the bag
  // remainder is factored per bucket into segmented forms; the drawn multiset
  // stays a fixed leading segment. Tie draws get tokens too -- whether they are
  // finally shown is a display decision made below.
  PegOutcomeTok *toks =
      malloc_or_die((size_t)(n_rows + n_fams) * sizeof(PegOutcomeTok));
  int n_toks = 0;
  for (int fam_idx = 0; fam_idx < n_fams; fam_idx++) {
    const PegOutcomeFamily *fam = &fams[fam_idx];
    const int n_buckets =
        (int)fam->seen[0] + (int)fam->seen[1] + (int)fam->seen[2];
    if (n_buckets <= 1) {
      int bucket = 2;
      if (fam->seen[0]) {
        bucket = 0;
      } else if (fam->seen[1]) {
        bucket = 1;
      }
      peg_draw_token(toks[n_toks].text, sizeof(toks[n_toks].text), fam->drawn,
                     fam->rem_ms);
      toks[n_toks].bucket = bucket;
      toks[n_toks].weight = fam->total_weight;
      n_toks++;
      continue;
    }
    // Split: the bag remainder's orderings land in more than one bucket. Factor
    // them per win/loss/tie bucket, keeping the drawn multiset as a leading
    // segment.
    for (int bucket = 0; bucket <= 2; bucket++) {
      PegStrList seqs = {0};
      for (int row_idx = 0; row_idx < n_rows; row_idx++) {
        if (peg_outcome_bucket(rows[row_idx].mover_total) != bucket) {
          continue;
        }
        if (strcmp(row_drawn[row_idx], fam->drawn) != 0 ||
            strcmp(row_rem_ms[row_idx], fam->rem_ms) != 0) {
          continue;
        }
        peg_strlist_push_unique(&seqs, rows[row_idx].remaining);
      }
      if (seqs.len == 0) {
        peg_strlist_destroy(&seqs);
        continue;
      }
      PegStrList forms = peg_factor(&seqs);
      for (int form_idx = 0; form_idx < forms.len; form_idx++) {
        peg_draw_token(toks[n_toks].text, sizeof(toks[n_toks].text), fam->drawn,
                       forms.items[form_idx]);
        toks[n_toks].bucket = bucket;
        toks[n_toks].weight =
            peg_form_covered(forms.items[form_idx]) * fam->per_ordering;
        n_toks++;
      }
      peg_strlist_destroy(&forms);
      peg_strlist_destroy(&seqs);
    }
  }
  free(fams);
  free(row_drawn);
  free(row_rem_ms);

  qsort(toks, (size_t)n_toks, sizeof(PegOutcomeTok), peg_outcome_tok_cmp);
  int n_bucket_toks[3] = {0, 0, 0};
  for (int tok_idx = 0; tok_idx < n_toks; tok_idx++) {
    n_bucket_toks[toks[tok_idx].bucket]++;
  }

  // Leave the largest list implied -- the reader infers it from the count
  // columns -- and print the smaller ones, comma-separated, each with its
  // label. Empty lists are skipped, so a row with no tie draws just shows the
  // shorter of win / loss, exactly as before ties were tracked. On a size tie,
  // prefer to imply a loss, then a win, so a tie draw stays visible. A tie
  // majority is rare and surprising, so when ties are the implied list we call
  // it out explicitly with a trailing ", otherwise ties".
  static const int imply_pref[3] = {1, 0, 2}; // loss, then win, then tie
  int implied = imply_pref[0];
  for (int pref_idx = 1; pref_idx < 3; pref_idx++) {
    const int bucket = imply_pref[pref_idx];
    if (n_bucket_toks[bucket] > n_bucket_toks[implied]) {
      implied = bucket;
    }
  }

  static const char *const labels[3] = {"W:", "L:", "T:"};
  const int display_order[3] = {0, 2, 1}; // wins, ties, losses
  StringBuilder *sb = string_builder_create();
  bool first = true;
  for (int order_idx = 0; order_idx < 3; order_idx++) {
    const int bucket = display_order[order_idx];
    if (bucket == implied || n_bucket_toks[bucket] == 0) {
      continue;
    }
    if (!first) {
      string_builder_add_string(sb, ", ");
    }
    first = false;
    string_builder_add_string(sb, labels[bucket]);
    peg_append_outcome_toks(sb, toks, n_toks, bucket);
  }
  if (implied == 2) { // ties implied: rare, so name it explicitly
    string_builder_add_string(sb, ", otherwise ties");
  }
  free(toks);
  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}

static char *peg_build_outcomes_string(const PegResult *result) {
  return peg_build_outcomes_string_rows(result->per_scenario,
                                        result->n_per_scenario);
}

static void peg_append_stage_table(StringBuilder *sb,
                                   const PegStageSnapshot *history,
                                   int n_history) {
  const int64_t now_ns = ctimer_monotonic_ns();
  const int num_rows = n_history + 1;
  StringGrid *sg = string_grid_create(num_rows, 5, 2);
  string_grid_set_cell(sg, 0, 0, string_duplicate("stage"));
  string_grid_set_cell(sg, 0, 1, string_duplicate("fidelity"));
  string_grid_set_cell(sg, 0, 2, string_duplicate("cands"));
  string_grid_set_cell(sg, 0, 3, string_duplicate("best win%"));
  string_grid_set_cell(sg, 0, 4, string_duplicate("time"));

  for (int stage_idx = 0; stage_idx < n_history; stage_idx++) {
    const PegStageSnapshot *st = &history[stage_idx];
    const int row = stage_idx + 1;
    const bool is_current = (st->end_ns == 0);

    string_grid_set_cell(sg, row, 0, get_formatted_string("%d", stage_idx));

    if (st->fidelity_plies == 0) {
      string_grid_set_cell(sg, row, 1, string_duplicate("greedy"));
    } else {
      string_grid_set_cell(
          sg, row, 1, get_formatted_string("%d-ply eg", st->fidelity_plies));
    }

    if (is_current && st->field_size > 0) {
      const double pct = 100.0 * st->cands_done / st->field_size;
      string_grid_set_cell(sg, row, 2,
                           get_formatted_string("%d/%d (%.0f%%)",
                                                st->cands_done, st->field_size,
                                                pct));
    } else {
      string_grid_set_cell(
          sg, row, 2,
          get_formatted_string("%d/%d", st->cands_done, st->field_size));
    }

    if (st->best_win_pct >= 0.0) {
      string_grid_set_cell(
          sg, row, 3, get_formatted_string("%.1f%%", 100.0 * st->best_win_pct));
    } else {
      string_grid_set_cell(sg, row, 3, string_duplicate("-"));
    }

    const int64_t end_ns = (st->end_ns != 0) ? st->end_ns : now_ns;
    const double stage_secs = (double)(end_ns - st->start_ns) / 1e9;
    string_grid_set_cell(sg, row, 4, get_formatted_string("%.1fs", stage_secs));
  }

  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);
}

// Forward declarations for helpers defined later in this file.
static char *peg_fidelity_label(int fidelity);
static bool peg_moves_match(const Move *m1, const Move *m2);
static double peg_graded_history_time(const PegPollSnapshot *snap, int slot,
                                      const Move *move);

// Append `text` (an outcomes cell like "W: tok tok ...") wrapped to `avail`
// columns per line, breaking only at spaces (defensive char-break if a single
// token exceeds `avail`). The first line continues from whatever the caller
// already emitted on the current line; each continuation line is preceded by a
// newline and `indent` spaces so the column stays aligned. At most `max_lines`
// lines are emitted (0 = unlimited); if more tokens remain, the last line ends
// with " ..." (kept within `avail`) and *truncated is set true. avail >= 1.
static void peg_append_wrapped(StringBuilder *sb, const char *text, int indent,
                               int avail, int max_lines, bool *truncated) {
  if (avail < 1) {
    avail = 1;
  }
  // Tokenize on spaces (the label "W:"/"L:" is just the first token).
  int cap = 16;
  int n_tok = 0;
  const char **tok = malloc_or_die((size_t)cap * sizeof(char *));
  int *tok_len = malloc_or_die((size_t)cap * sizeof(int));
  for (const char *cursor = text; *cursor != '\0';) {
    while (*cursor == ' ') {
      cursor++;
    }
    if (*cursor == '\0') {
      break;
    }
    const char *start = cursor;
    while (*cursor != '\0' && *cursor != ' ') {
      cursor++;
    }
    if (n_tok == cap) {
      cap *= 2;
      tok = realloc_or_die(tok, (size_t)cap * sizeof(char *));
      tok_len = realloc_or_die(tok_len, (size_t)cap * sizeof(int));
    }
    tok[n_tok] = start;
    tok_len[n_tok] = (int)(cursor - start);
    n_tok++;
  }

  const int ELLIPSIS = 4; // " ..."
  int idx = 0;
  int line = 0;
  while (idx < n_tok) {
    if (line > 0) {
      string_builder_add_char(sb, '\n');
      string_builder_add_spaces(sb, indent);
    }
    const bool last_line = max_lines > 0 && line == max_lines - 1;
    int col = 0;
    while (idx < n_tok) {
      const int sep = col > 0 ? 1 : 0;
      // Defensive: a token wider than the whole cell — hard-break it.
      if (col == 0 && tok_len[idx] > avail) {
        int emitted = 0;
        while (emitted < tok_len[idx]) {
          if (emitted > 0) {
            if (max_lines > 0 && line + 1 >= max_lines) {
              if (truncated != NULL) {
                *truncated = true;
              }
              // The previous chunk already filled the column to `avail`, so
              // drop its last 3 chars and replace them with "..." to keep the
              // line within width (avail >= PEG_OUTCOMES_MIN_CELL > 3).
              const size_t cur_len = string_builder_length(sb);
              if (cur_len >= 3) {
                string_builder_truncate(sb, cur_len - 3);
              }
              string_builder_add_string(sb, "...");
              goto cleanup;
            }
            string_builder_add_char(sb, '\n');
            string_builder_add_spaces(sb, indent);
            line++;
          }
          const int chunk = (tok_len[idx] - emitted) < avail
                                ? (tok_len[idx] - emitted)
                                : avail;
          char *piece = get_formatted_string("%.*s", chunk, tok[idx] + emitted);
          string_builder_add_string(sb, piece);
          free(piece);
          emitted += chunk;
        }
        idx++;
        col = avail; // force a wrap before the next token
        continue;
      }
      // On the last allowed line keep room for " ..." when more tokens remain.
      const int reserve = (last_line && idx + 1 < n_tok) ? ELLIPSIS : 0;
      if (col > 0 && col + sep + tok_len[idx] + reserve > avail) {
        break;
      }
      if (sep) {
        string_builder_add_char(sb, ' ');
        col++;
      }
      char *piece = get_formatted_string("%.*s", tok_len[idx], tok[idx]);
      string_builder_add_string(sb, piece);
      free(piece);
      col += tok_len[idx];
      idx++;
    }
    if (last_line && idx < n_tok) {
      if (truncated != NULL) {
        *truncated = true;
      }
      string_builder_add_string(sb, " ...");
      break;
    }
    line++;
  }
cleanup:
  free(tok);
  free(tok_len);
}

// Find the captured per-ordering outcomes for a given move, or NULL.
static const PegCandOutcomes *peg_find_cand_outcomes(const PegResult *result,
                                                     const Move *move) {
  for (int oc_idx = 0; oc_idx < result->n_cand_outcomes; oc_idx++) {
    if (peg_moves_match(&result->cand_outcomes[oc_idx].move, move)) {
      return &result->cand_outcomes[oc_idx];
    }
  }
  return NULL;
}

// Render the final graded ranking: every play that entered a halving stage,
// grouped by the deepest endgame fidelity it reached. Deepest tier first, a
// dashed separator between tiers, and the rank continuing across them. Shows
// the whole graded list (its size is the post-greedy cutoff, e.g. 32 by default
// or the entire field under -pegtopk all), so the row buffers are sized to it.
// When snap is non-NULL, shows "total" + per-fidelity time columns (one per
// non-greedy completed stage). When snap is NULL, shows a single "time" column.
static void peg_append_graded_table(StringBuilder *sb, const PegResult *result,
                                    const Board *board,
                                    const LetterDistribution *ld,
                                    const PegPollSnapshot *snap,
                                    bool show_outcomes, int out_width,
                                    int out_max_lines, const char *trunc_note,
                                    bool *out_truncated) {
  // Per-play outcomes column (W/L draws) when requested and captured.
  const bool show_outc = show_outcomes && result->n_cand_outcomes > 0;
  // Collect distinct non-greedy fidelities from the graded result, in ascending
  // order (graded_cands is stored shallowest-first so iteration is ordered).
  int unique_fids[PEG_POLL_MAX_STAGES];
  int n_unique_fids = 0;
  if (snap != NULL) {
    for (int graded_idx = 0; graded_idx < result->n_graded; graded_idx++) {
      const int fid = result->graded_fidelity[graded_idx];
      if (fid == 0) {
        continue;
      }
      bool already = false;
      for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
        if (unique_fids[uf_idx] == fid) {
          already = true;
          break;
        }
      }
      if (!already && n_unique_fids < PEG_POLL_MAX_STAGES) {
        unique_fids[n_unique_fids++] = fid;
      }
    }
  }
  // Show time columns only when there are non-greedy fidelities.
  const bool show_time = n_unique_fids > 0;
  // History slot index for each unique fidelity (-1 = not in history, use
  // eval_seconds instead, which happens for the deepest completed stage).
  int fid_to_hist[PEG_POLL_MAX_STAGES];
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    fid_to_hist[uf_idx] = -1;
    if (snap != NULL) {
      for (int hist_slot = 0; hist_slot < snap->n_history_stages; hist_slot++) {
        if (snap->history_fidelities[hist_slot] == unique_fids[uf_idx]) {
          fid_to_hist[uf_idx] = hist_slot;
          break;
        }
      }
    }
  }

  const int total = result->n_graded;
  char **depth = malloc_or_die((size_t)total * sizeof(char *));
  char **rankc = malloc_or_die((size_t)total * sizeof(char *));
  char **movec = malloc_or_die((size_t)total * sizeof(char *));
  char **winsc = malloc_or_die((size_t)total * sizeof(char *));
  char **tiesc = malloc_or_die((size_t)total * sizeof(char *));
  char **lossc = malloc_or_die((size_t)total * sizeof(char *));
  char **winc = malloc_or_die((size_t)total * sizeof(char *));
  char **spreadc = malloc_or_die((size_t)total * sizeof(char *));
  char **totalc = malloc_or_die((size_t)total * sizeof(char *));
  char **outc =
      show_outc ? malloc_or_die((size_t)total * sizeof(char *)) : NULL;
  // timecols[fid_col][row]
  char ***timecols =
      show_time ? malloc_or_die((size_t)n_unique_fids * sizeof(char **)) : NULL;
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    timecols[uf_idx] = malloc_or_die((size_t)total * sizeof(char *));
  }
  int *rowfid = malloc_or_die((size_t)total * sizeof(int));
  int n_rows = 0;

  int64_t weighted_orderings = 0;
  int unique_orderings = 0;

  // Walk blocks deepest-to-shallowest for display order (deepest tier first).
  int block_end = total;
  while (block_end > 0) {
    const int fid = result->graded_fidelity[block_end - 1];
    int block_start = block_end - 1;
    while (block_start > 0 && result->graded_fidelity[block_start - 1] == fid) {
      block_start--;
    }
    for (int graded_idx = block_start; graded_idx < block_end; graded_idx++) {
      const PegRankedCand *cand = &result->graded_cands[graded_idx];
      if (n_rows == 0) {
        weighted_orderings = cand->weight_sum;
        unique_orderings = cand->n_scenarios;
      }
      depth[n_rows] = peg_fidelity_label(fid);
      rankc[n_rows] = get_formatted_string("%d", n_rows + 1);
      StringBuilder *move_sb = string_builder_create();
      string_builder_add_move(move_sb, board, &cand->move, ld, false);
      movec[n_rows] = string_builder_dump(move_sb, NULL);
      string_builder_destroy(move_sb);
      winsc[n_rows] = get_formatted_string("%" PRId64, cand->win_count);
      tiesc[n_rows] = get_formatted_string("%" PRId64, cand->tie_count);
      lossc[n_rows] = get_formatted_string(
          "%" PRId64, cand->weight_sum - cand->win_count - cand->tie_count);
      winc[n_rows] = get_formatted_string("%.1f", 100.0 * cand->win_pct);
      spreadc[n_rows] = get_formatted_string("%+.1f", cand->mean_spread);
      if (show_outc) {
        const PegCandOutcomes *oc = peg_find_cand_outcomes(result, &cand->move);
        outc[n_rows] =
            (oc != NULL && oc->n_rows > 0)
                ? peg_build_outcomes_string_rows(oc->rows, oc->n_rows)
                : string_duplicate("-");
      }
      rowfid[n_rows] = fid;

      if (show_time) {
        double total_secs = 0.0;
        for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
          const int col_fid = unique_fids[uf_idx];
          double col_time = -1.0;
          if (col_fid > fid) {
            // This candidate was not evaluated at this depth.
            col_time = -1.0;
          } else {
            const int hist_slot = fid_to_hist[uf_idx];
            if (hist_slot >= 0) {
              col_time = peg_graded_history_time(snap, hist_slot, &cand->move);
            }
            // Not in history → this is the deepest completed stage for this
            // tier; use eval_seconds (which equals the time at col_fid).
            if (col_time < 0.0 && col_fid == fid) {
              col_time = cand->eval_seconds;
            }
          }
          timecols[uf_idx][n_rows] =
              col_time >= 0.0 ? get_formatted_string("%.1fs", col_time)
                              : string_duplicate("-");
          if (col_time >= 0.0) {
            total_secs += col_time;
          }
        }
        totalc[n_rows] = total_secs > 0.0
                             ? get_formatted_string("%.1fs", total_secs)
                             : string_duplicate("-");
      } else {
        totalc[n_rows] = get_formatted_string("%.1fs", cand->eval_seconds);
      }
      n_rows++;
    }
    block_end = block_start;
  }

  if (trunc_note != NULL) {
    string_builder_add_formatted_string(sb, "%s\n", trunc_note);
  }
  string_builder_add_formatted_string(
      sb, "%" PRId64 " weighted bag orderings (%d unique)\n",
      weighted_orderings, unique_orderings);

  size_t wd = string_length("depth");
  size_t wr = string_length("rank");
  size_t wm = string_length("move");
  size_t ww = string_length("wins");
  size_t wti = string_length("ties");
  size_t wlo = string_length("loss");
  size_t wp = string_length("win%");
  size_t wsp = string_length("spread");
  const char *total_hdr = show_time ? "total" : "time";
  size_t wtotal = string_length(total_hdr);
  // Per-fidelity column widths, labeled by their fidelity.
  char **fid_hdrs =
      show_time ? malloc_or_die((size_t)n_unique_fids * sizeof(char *)) : NULL;
  size_t *wfid =
      show_time ? malloc_or_die((size_t)n_unique_fids * sizeof(size_t)) : NULL;
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    fid_hdrs[uf_idx] = peg_fidelity_label(unique_fids[uf_idx]);
    wfid[uf_idx] = string_length(fid_hdrs[uf_idx]);
  }
  for (int row_idx = 0; row_idx < n_rows; row_idx++) {
    wd =
        string_length(depth[row_idx]) > wd ? string_length(depth[row_idx]) : wd;
    wr =
        string_length(rankc[row_idx]) > wr ? string_length(rankc[row_idx]) : wr;
    wm =
        string_length(movec[row_idx]) > wm ? string_length(movec[row_idx]) : wm;
    ww =
        string_length(winsc[row_idx]) > ww ? string_length(winsc[row_idx]) : ww;
    wti = string_length(tiesc[row_idx]) > wti ? string_length(tiesc[row_idx])
                                              : wti;
    wlo = string_length(lossc[row_idx]) > wlo ? string_length(lossc[row_idx])
                                              : wlo;
    wp = string_length(winc[row_idx]) > wp ? string_length(winc[row_idx]) : wp;
    wsp = string_length(spreadc[row_idx]) > wsp
              ? string_length(spreadc[row_idx])
              : wsp;
    wtotal = string_length(totalc[row_idx]) > wtotal
                 ? string_length(totalc[row_idx])
                 : wtotal;
    for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
      wfid[uf_idx] = string_length(timecols[uf_idx][row_idx]) > wfid[uf_idx]
                         ? string_length(timecols[uf_idx][row_idx])
                         : wfid[uf_idx];
    }
  }

  // Total row width for the tier-separator dashes.
  // 8 fixed gaps (between 9 fixed columns) + 1 gap before total + n_unique_fids
  // gaps.
  size_t total_w = wd + wr + wm + ww + wti + wlo + wp + wsp + wtotal +
                   (size_t)(16 + 2 + 2 * n_unique_fids);
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    total_w += wfid[uf_idx];
  }

  // Column where the outcomes cell content begins (9 fixed columns + their 8
  // two-space gaps + each time column's "  <fid>" + the "  " before outcomes).
  // Wrapped continuation lines indent to here so the column stays aligned.
  size_t outc_indent =
      wd + wr + wm + ww + wti + wlo + wp + wsp + wtotal + (size_t)(16);
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    outc_indent += 2 + wfid[uf_idx];
  }
  outc_indent += 2;
  // Per-line width available for outcomes content. out_width is the whole line;
  // clamp it up so the cell always fits at least the label + one worst-case
  // token. out_width <= 0 means unbounded (no wrapping).
  int outc_avail = INT_MAX;
  if (out_width > 0) {
    int eff_width = out_width;
    const int floor_width = (int)outc_indent + PEG_OUTCOMES_MIN_CELL;
    if (eff_width < floor_width) {
      eff_width = floor_width;
    }
    outc_avail = eff_width - (int)outc_indent;
  }

  // Header.
  string_builder_add_formatted_string(
      sb, "%-*s  %*s  %-*s  %*s  %*s  %*s  %*s  %*s  %*s", (int)wd, "depth",
      (int)wr, "rank", (int)wm, "move", (int)ww, "wins", (int)wti, "ties",
      (int)wlo, "loss", (int)wp, "win%", (int)wsp, "spread", (int)wtotal,
      total_hdr);
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    string_builder_add_formatted_string(sb, "  %*s", (int)wfid[uf_idx],
                                        fid_hdrs[uf_idx]);
  }
  if (show_outc) {
    // Trailing, unaligned: outcomes strings vary in width per row, so padding
    // them to a common width would waste space; keep it as the last column.
    string_builder_add_string(sb, "  outcomes");
  }
  string_builder_add_string(sb, "\n");

  // Data rows with tier-separator lines between fidelity groups.
  for (int row_idx = 0; row_idx < n_rows; row_idx++) {
    if (row_idx > 0 && rowfid[row_idx] != rowfid[row_idx - 1]) {
      for (size_t dash_idx = 0; dash_idx < total_w; dash_idx++) {
        string_builder_add_char(sb, '-');
      }
      string_builder_add_char(sb, '\n');
    }
    string_builder_add_formatted_string(
        sb, "%-*s  %*s  %-*s  %*s  %*s  %*s  %*s  %*s  %*s", (int)wd,
        depth[row_idx], (int)wr, rankc[row_idx], (int)wm, movec[row_idx],
        (int)ww, winsc[row_idx], (int)wti, tiesc[row_idx], (int)wlo,
        lossc[row_idx], (int)wp, winc[row_idx], (int)wsp, spreadc[row_idx],
        (int)wtotal, totalc[row_idx]);
    for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
      string_builder_add_formatted_string(sb, "  %*s", (int)wfid[uf_idx],
                                          timecols[uf_idx][row_idx]);
    }
    if (show_outc) {
      string_builder_add_string(sb, "  ");
      peg_append_wrapped(sb, outc[row_idx], (int)outc_indent, outc_avail,
                         out_max_lines, out_truncated);
      free(outc[row_idx]);
    }
    string_builder_add_string(sb, "\n");
    free(depth[row_idx]);
    free(rankc[row_idx]);
    free(movec[row_idx]);
    free(winsc[row_idx]);
    free(tiesc[row_idx]);
    free(lossc[row_idx]);
    free(winc[row_idx]);
    free(spreadc[row_idx]);
    free(totalc[row_idx]);
    for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
      free(timecols[uf_idx][row_idx]);
    }
  }

  free(depth);
  free(rankc);
  free(movec);
  free(winsc);
  free(tiesc);
  free(lossc);
  free(winc);
  free(spreadc);
  free(totalc);
  free(outc);
  if (show_time) {
    for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
      free(timecols[uf_idx]);
      free(fid_hdrs[uf_idx]);
    }
  }
  free(timecols);
  free(fid_hdrs);
  free(wfid);
  free(rowfid);
}

// Append the flat candidate ranking for result->top_cands. Numeric columns are
// right-aligned; the move column is left-aligned. Columns are rank/move/win%/
// spread plus optional ones: wins and ties (integer labeled bag-ordering
// counts; losses are weight_sum - wins - ties) and time (per-candidate wall
// clock, live solves only). With show_stats, a "<P> weighted bag orderings (<U>
// unique)" line precedes the table. The live stream omits wins/ties and stats;
// the final view omits time. The caller appends the outcomes line (as in the
// graded view).
static void peg_append_flat_ranking(StringBuilder *sb, const PegResult *result,
                                    const Board *board,
                                    const LetterDistribution *ld,
                                    bool show_wins, bool show_time,
                                    bool show_stats, int min_move_width) {
  int col = 0;
  const int rank_col = col++;
  const int move_col = col++;
  const int wins_col = show_wins ? col++ : -1;
  const int ties_col = show_wins ? col++ : -1;
  const int loss_col = show_wins ? col++ : -1;
  const int winpct_col = col++;
  const int spread_col = col++;
  const int time_col = show_time ? col++ : -1;
  const int num_cols = col;
  const int num_rows = result->n_top_cands + 1;

  if (show_stats && result->n_top_cands > 0) {
    // weight_sum is the labeled ordered-draw denominator (constant across
    // plays); n_scenarios is the top play's distinct-scenario count.
    string_builder_add_formatted_string(
        sb, "%" PRId64 " weighted bag orderings (%d unique)\n",
        result->top_cands[0].weight_sum, result->top_cands[0].n_scenarios);
  }

  StringGrid *sg = string_grid_create(num_rows, num_cols, 2);
  // Right-align every numeric column; the move column stays left-aligned.
  string_grid_set_col_right_align(sg, rank_col, true);
  if (wins_col >= 0) {
    string_grid_set_col_right_align(sg, wins_col, true);
  }
  if (ties_col >= 0) {
    string_grid_set_col_right_align(sg, ties_col, true);
  }
  if (loss_col >= 0) {
    string_grid_set_col_right_align(sg, loss_col, true);
  }
  string_grid_set_col_right_align(sg, winpct_col, true);
  string_grid_set_col_right_align(sg, spread_col, true);
  if (time_col >= 0) {
    string_grid_set_col_right_align(sg, time_col, true);
  }

  string_grid_set_cell(sg, 0, rank_col, string_duplicate("rank"));
  string_grid_set_cell(sg, 0, move_col, string_duplicate("move"));
  if (wins_col >= 0) {
    string_grid_set_cell(sg, 0, wins_col, string_duplicate("wins"));
  }
  if (ties_col >= 0) {
    string_grid_set_cell(sg, 0, ties_col, string_duplicate("ties"));
  }
  if (loss_col >= 0) {
    string_grid_set_cell(sg, 0, loss_col, string_duplicate("loss"));
  }
  string_grid_set_cell(sg, 0, winpct_col, string_duplicate("win%"));
  string_grid_set_cell(sg, 0, spread_col, string_duplicate("spread"));
  if (time_col >= 0) {
    string_grid_set_cell(sg, 0, time_col, string_duplicate("time"));
  }

  for (int cand_idx = 0; cand_idx < result->n_top_cands; cand_idx++) {
    const PegRankedCand *cand = &result->top_cands[cand_idx];
    const int row = cand_idx + 1;

    string_grid_set_cell(sg, row, rank_col,
                         get_formatted_string("%d", cand_idx + 1));

    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, board, &cand->move, ld, false);
    char *move_str = string_builder_dump(move_sb, NULL);
    string_builder_destroy(move_sb);
    if (min_move_width > (int)string_length(move_str)) {
      // Left-pad to the stage's widest move so the column never grows
      // mid-solve.
      char *padded = get_formatted_string("%-*s", min_move_width, move_str);
      free(move_str);
      move_str = padded;
    }
    string_grid_set_cell(sg, row, move_col, move_str);

    if (wins_col >= 0) {
      string_grid_set_cell(sg, row, wins_col,
                           get_formatted_string("%" PRId64, cand->win_count));
    }
    if (ties_col >= 0) {
      string_grid_set_cell(sg, row, ties_col,
                           get_formatted_string("%" PRId64, cand->tie_count));
    }
    if (loss_col >= 0) {
      const int64_t loss_count =
          cand->weight_sum - cand->win_count - cand->tie_count;
      string_grid_set_cell(sg, row, loss_col,
                           get_formatted_string("%" PRId64, loss_count));
    }
    string_grid_set_cell(sg, row, winpct_col,
                         get_formatted_string("%.1f", 100.0 * cand->win_pct));
    string_grid_set_cell(sg, row, spread_col,
                         get_formatted_string("%+.1f", cand->mean_spread));

    if (time_col >= 0) {
      string_grid_set_cell(sg, row, time_col,
                           get_formatted_string("%.1fs", cand->eval_seconds));
    }
  }

  string_builder_add_string_grid(sb, sg, false);
  string_grid_destroy(sg);
}

// True when two moves are the same play (type, position, tiles).
static bool peg_moves_match(const Move *m1, const Move *m2) {
  if (m1->move_type != m2->move_type || m1->dir != m2->dir ||
      m1->row_start != m2->row_start || m1->col_start != m2->col_start ||
      m1->tiles_length != m2->tiles_length ||
      m1->tiles_played != m2->tiles_played) {
    return false;
  }
  for (int tile_idx = 0; tile_idx < m1->tiles_length; tile_idx++) {
    if (m1->tiles[tile_idx] != m2->tiles[tile_idx]) {
      return false;
    }
  }
  return true;
}

// Return a heap-allocated depth label string. Caller frees.
static char *peg_fidelity_label(int fidelity) {
  return fidelity == 0 ? string_duplicate("greedy")
                       : get_formatted_string("%d-ply", fidelity);
}

// Look up a candidate's eval_seconds in a specific history slot by move
// identity. Returns -1.0 if not found.
static double peg_graded_history_time(const PegPollSnapshot *snap, int slot,
                                      const Move *move) {
  for (int hcand_idx = 0; hcand_idx < snap->history_n_cands[slot];
       hcand_idx++) {
    if (peg_moves_match(move, &snap->history_cands[slot][hcand_idx].move)) {
      return snap->history_cands[slot][hcand_idx].eval_seconds;
    }
  }
  return -1.0;
}

char *peg_result_get_string(const PegResult *result, const Game *game,
                            bool show_outcomes, PegPoll *poll, int out_width,
                            int out_max_lines, const char *trunc_note,
                            bool *out_truncated) {
  StringBuilder *sb = string_builder_create();

  // Read poll snapshot once. Used for both the live path and (when done) to
  // supply per-stage timing history to the completed-result display. Zeroed so
  // it is never read uninitialized when poll == NULL (have_snap gates real
  // use).
  PegPollSnapshot snap = {0};
  bool have_snap = false;
  if (poll != NULL) {
    peg_poll_read(poll, &snap);
    have_snap = true;
  }

  // Live path: poll is set and the solve is still running.
  if (have_snap && !snap.done) {
    const int64_t now_ns = ctimer_monotonic_ns();
    const int64_t start_ns =
        snap.n_stage_history > 0 ? snap.stage_history[0].start_ns : now_ns;
    const double total_secs = (double)(now_ns - start_ns) / 1e9;
    string_builder_add_formatted_string(sb, "PEG (running): %.1fs\n",
                                        total_secs);
    if (snap.n_stage_history > 0) {
      peg_append_stage_table(sb, snap.stage_history, snap.n_stage_history);
      string_builder_add_string(sb, "\n");
    }
    // Use the current stage's entries, or fall back to the previous stage's
    // ranking (saved at each stage boundary) while a new stage has not yet
    // finished a candidate, so a poll always shows the best available ranking.
    PegRankedCand *live_entries = snap.entries;
    int n_live_entries = snap.n_entries;
    if (n_live_entries == 0 && snap.n_baseline_entries > 0) {
      live_entries = snap.baseline_entries;
      n_live_entries = snap.n_baseline_entries;
    }
    if (n_live_entries > 0) {
      const Board *board = game_get_board(game);
      const LetterDistribution *ld = game_get_ld(game);
      // Render the same flat table as the final printout on the live snapshot,
      // so a `sta` poll matches the final view (with partial data) rather than
      // a separate live layout.
      PegResult live = {0};
      live.top_cands = live_entries;
      live.n_top_cands = n_live_entries;
      live.last_completed_stage = snap.stage;
      peg_append_flat_ranking(sb, &live, board, ld, /*show_wins=*/true,
                              /*show_time=*/false, /*show_stats=*/true,
                              /*min_move_width=*/0);
      // Per-multiset W/L/T for the best play, fed live via the poll outcomes.
      if (show_outcomes) {
        PegCandOutcomes *live_oc = NULL;
        int live_n_oc = 0;
        peg_poll_copy_outcomes(poll, &live_oc, &live_n_oc);
        live.cand_outcomes = live_oc;
        live.n_cand_outcomes = live_n_oc;
        const PegCandOutcomes *best =
            peg_find_cand_outcomes(&live, &live_entries[0].move);
        if (best != NULL && best->n_rows > 0) {
          char *oc = peg_build_outcomes_string_rows(best->rows, best->n_rows);
          string_builder_add_formatted_string(sb, "\noutcomes (best): %s\n",
                                              oc);
          free(oc);
        }
        peg_cand_outcomes_destroy_array(live_oc, live_n_oc);
      }
    }
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  if (result->last_completed_stage < 0 && result->n_stage_history == 0) {
    string_builder_add_string(sb, "no PEG results.\n");
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  if (result->last_completed_stage < 0) {
    const int64_t now_ns = ctimer_monotonic_ns();
    const double total_secs =
        (double)(now_ns - result->stage_history[0].start_ns) / 1e9;
    string_builder_add_formatted_string(sb, "PEG (running): %.1fs\n",
                                        total_secs);
  } else if (result->last_stage_partial) {
    string_builder_add_formatted_string(
        sb, "PEG (stage %d partial): %d candidates, %.2fs\n",
        result->last_completed_stage, result->n_top_cands,
        ctimer_elapsed_seconds(&result->timer));
  } else {
    string_builder_add_formatted_string(
        sb, "PEG (last completed stage %d): %d candidates, %.2fs\n",
        result->last_completed_stage, result->n_top_cands,
        ctimer_elapsed_seconds(&result->timer));
  }

  if (result->n_stage_history > 0) {
    peg_append_stage_table(sb, result->stage_history, result->n_stage_history);
    string_builder_add_string(sb, "\n");
  }

  if (result->n_top_cands == 0) {
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Final (graded) view: group the plays that reached each stage, deepest
  // first. The flat table below is used for the live/in-progress view and for a
  // result that never finished a halving stage (e.g. the time limit hit during
  // greedy); in that case there are no tiers, so just show the post-greedy
  // ranking.
  if (result->n_graded > 0) {
    // The graded table carries a per-play outcomes column itself (when
    // show_outcomes), so no separate "outcomes (best)" line is needed here.
    peg_append_graded_table(sb, result, board, ld, have_snap ? &snap : NULL,
                            show_outcomes, out_width, out_max_lines, trunc_note,
                            out_truncated);
    char *out = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    return out;
  }

  peg_append_flat_ranking(sb, result, board, ld, /*show_wins=*/true,
                          /*show_time=*/false, /*show_stats=*/true,
                          /*min_move_width=*/0);
  if (show_outcomes && result->n_per_scenario > 0) {
    char *outcomes = peg_build_outcomes_string(result);
    string_builder_add_formatted_string(sb, "\noutcomes (best): %s\n",
                                        outcomes);
    free(outcomes);
  }

  char *out = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return out;
}
