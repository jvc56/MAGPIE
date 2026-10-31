#include "peg_string.h"

#include "../compat/ctime.h"
#include "../def/peg_defs.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../impl/peg.h"
#include "../util/io_util.h"
#include "../util/stable_sort.h"
#include "../util/string_util.h"
#include "letter_distribution_string.h"
#include "move_string.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Outcome bucket a PEG draw lands in, keyed off the mover's final spread. These
// values index this file's per-bucket arrays (a family's seen flags, the
// per-bucket token counts, the display labels), so they must stay a dense
// 0-based range and NUMBER_OF_PEG_OUTCOME_BUCKETS must stay last.
enum {
  PEG_OUTCOME_BUCKET_WIN,
  PEG_OUTCOME_BUCKET_LOSS,
  PEG_OUTCOME_BUCKET_TIE,
  NUMBER_OF_PEG_OUTCOME_BUCKETS,
};

// The candidate in progress lists each finished draw only when it has at most
// PEG_STATUS_MAX_FINISHED_DRAW_ROWS draws; past it, its finished draws are
// totalled in one line. Summary labels are padded to PEG_SUMMARY_LABEL_WIDTH,
// and the lines under one are indented by PEG_SUMMARY_SUBLINE_INDENT. A cut
// play's bound gets up to PEG_BOUND_MAX_DECIMALS decimals to stay above zero.
enum {
  PEG_STATUS_MAX_FINISHED_DRAW_ROWS = 16,
  PEG_SUMMARY_LABEL_WIDTH = 16,
  PEG_SUMMARY_SUBLINE_INDENT = 2,
  PEG_BOUND_MAX_DECIMALS = 6,
};

// One displayable outcome token: a draw rendered as the mover's drawn multiset
// followed by the bag remainder -- a sorted multiset ("DH/RS") when its
// orderings share a bucket, or "/"-segmented ("DH/R/S") when they split --
// tagged with its bucket and summed labeled-ordering weight.
typedef struct {
  char text[64];
  int bucket; // a PEG_OUTCOME_BUCKET_* value
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
  bool seen[NUMBER_OF_PEG_OUTCOME_BUCKETS];
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

// Classify a draw by the mover's final spread.
static int peg_outcome_bucket(int32_t mover_total) {
  if (mover_total > 0) {
    return PEG_OUTCOME_BUCKET_WIN;
  }
  if (mover_total < 0) {
    return PEG_OUTCOME_BUCKET_LOSS;
  }
  return PEG_OUTCOME_BUCKET_TIE;
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
// shorter of win / loss. The implied list is named with a trailing ", otherwise
// wins/loses/ties" only when the cell would otherwise be ambiguous: ties are
// the implied majority (rare), or the tie list is the only list shown (its
// win/loss counterpart was empty, so "T: E" alone cannot say which of win/loss
// is implied). When every ordering shares one bucket, says "always
// wins/loses/ties". Caller frees.
char *peg_build_outcomes_string_rows(const PegPerScenario *rows, int n_rows) {
  if (n_rows <= 0) {
    return string_duplicate("");
  }

  // "always X" when every ordering shares a single bucket.
  bool any[NUMBER_OF_PEG_OUTCOME_BUCKETS] = {false};
  for (int row_idx = 0; row_idx < n_rows; row_idx++) {
    any[peg_outcome_bucket(rows[row_idx].mover_total)] = true;
  }
  int n_any_buckets = 0;
  for (int bucket = 0; bucket < NUMBER_OF_PEG_OUTCOME_BUCKETS; bucket++) {
    n_any_buckets += (int)any[bucket];
  }
  if (n_any_buckets == 1) {
    const char *all_label = "always ties";
    if (any[PEG_OUTCOME_BUCKET_WIN]) {
      all_label = "always wins";
    } else if (any[PEG_OUTCOME_BUCKET_LOSS]) {
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
      for (int bucket = 0; bucket < NUMBER_OF_PEG_OUTCOME_BUCKETS; bucket++) {
        fams[fam_idx].seen[bucket] = false;
      }
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
    int n_buckets = 0;
    for (int bucket = 0; bucket < NUMBER_OF_PEG_OUTCOME_BUCKETS; bucket++) {
      n_buckets += (int)fam->seen[bucket];
    }
    if (n_buckets <= 1) {
      int bucket = PEG_OUTCOME_BUCKET_TIE;
      if (fam->seen[PEG_OUTCOME_BUCKET_WIN]) {
        bucket = PEG_OUTCOME_BUCKET_WIN;
      } else if (fam->seen[PEG_OUTCOME_BUCKET_LOSS]) {
        bucket = PEG_OUTCOME_BUCKET_LOSS;
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
    for (int bucket = 0; bucket < NUMBER_OF_PEG_OUTCOME_BUCKETS; bucket++) {
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

  stable_sort(toks, (size_t)n_toks, sizeof(PegOutcomeTok), peg_outcome_tok_cmp);
  int n_bucket_toks[NUMBER_OF_PEG_OUTCOME_BUCKETS] = {0};
  for (int tok_idx = 0; tok_idx < n_toks; tok_idx++) {
    n_bucket_toks[toks[tok_idx].bucket]++;
  }

  // Leave the largest list implied -- the reader infers it from the count
  // columns -- and print the smaller ones, comma-separated, each with its
  // label. Empty lists are skipped, so a row with no tie draws just shows the
  // shorter of win / loss, exactly as before ties were tracked. On a size tie,
  // prefer to imply a loss, then a win, so a tie draw stays visible.
  static const int imply_pref[NUMBER_OF_PEG_OUTCOME_BUCKETS] = {
      PEG_OUTCOME_BUCKET_LOSS,
      PEG_OUTCOME_BUCKET_WIN,
      PEG_OUTCOME_BUCKET_TIE,
  };
  int implied = imply_pref[0];
  for (int pref_idx = 1; pref_idx < NUMBER_OF_PEG_OUTCOME_BUCKETS; pref_idx++) {
    const int bucket = imply_pref[pref_idx];
    if (n_bucket_toks[bucket] > n_bucket_toks[implied]) {
      implied = bucket;
    }
  }

  static const char *const labels[NUMBER_OF_PEG_OUTCOME_BUCKETS] = {
      [PEG_OUTCOME_BUCKET_WIN] = "W:",
      [PEG_OUTCOME_BUCKET_LOSS] = "L:",
      [PEG_OUTCOME_BUCKET_TIE] = "T:",
  };
  static const char *const otherwise_word[NUMBER_OF_PEG_OUTCOME_BUCKETS] = {
      [PEG_OUTCOME_BUCKET_WIN] = "wins",
      [PEG_OUTCOME_BUCKET_LOSS] = "loses",
      [PEG_OUTCOME_BUCKET_TIE] = "ties",
  };
  const int display_order[NUMBER_OF_PEG_OUTCOME_BUCKETS] = {
      PEG_OUTCOME_BUCKET_WIN,
      PEG_OUTCOME_BUCKET_TIE,
      PEG_OUTCOME_BUCKET_LOSS,
  };
  StringBuilder *sb = string_builder_create();
  int n_shown = 0;
  int last_shown = -1;
  for (int order_idx = 0; order_idx < NUMBER_OF_PEG_OUTCOME_BUCKETS;
       order_idx++) {
    const int bucket = display_order[order_idx];
    if (bucket == implied || n_bucket_toks[bucket] == 0) {
      continue;
    }
    if (n_shown > 0) {
      string_builder_add_string(sb, ", ");
    }
    string_builder_add_string(sb, labels[bucket]);
    peg_append_outcome_toks(sb, toks, n_toks, bucket);
    last_shown = bucket;
    n_shown++;
  }
  // Name the implied list when the cell alone would not pin it down: ties are
  // the implied majority (rare and surprising), or the only list shown is the
  // tie list -- its win/loss counterpart was empty, so "T: E" alone cannot say
  // whether the implied majority is wins or losses. Two labeled lists already
  // imply the third, and a no-tie row implies its win/loss counterpart, so
  // those stay label-free.
  if (implied == PEG_OUTCOME_BUCKET_TIE ||
      (n_shown == 1 && last_shown == PEG_OUTCOME_BUCKET_TIE)) {
    string_builder_add_formatted_string(sb, ", otherwise %s",
                                        otherwise_word[implied]);
  }
  free(toks);
  char *out = string_builder_dump_and_destroy(sb, NULL);
  return out;
}

static char *peg_build_outcomes_string(const PegResult *result) {
  return peg_build_outcomes_string_rows(result->per_scenario,
                                        result->n_per_scenario);
}

// A stage's fidelity as the stage table names it: greedy, eg to end, or an
// N-ply eg.
// Forward declarations for helpers defined later in this file.
static char *peg_fidelity_label(int fidelity);
static bool peg_moves_match(const Move *m1, const Move *m2);
static double peg_graded_history_time(const PegPollSnapshot *snap, int slot,
                                      const Move *move);

// Render borrowed token arrays; the tokenizer retains ownership of them.
static void peg_append_wrapped_tokens(StringBuilder *sb, const char *const *tok,
                                      const int *tok_len, int n_tok, int indent,
                                      int avail, int max_lines,
                                      bool *truncated) {
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
              return;
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
}

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

  peg_append_wrapped_tokens(sb, tok, tok_len, n_tok, indent, avail, max_lines,
                            truncated);
  free(tok);
  free(tok_len);
}

// Per-line width available for an outcomes cell that starts at column
// `indent`. out_width is the whole line; it is clamped up so the cell always
// fits at least the label and one worst-case token. out_width <= 0 means
// unbounded (no wrapping).
static int peg_outcomes_avail(size_t indent, int out_width) {
  if (out_width <= 0) {
    return INT_MAX;
  }
  int eff_width = out_width;
  const int floor_width = (int)indent + PEG_OUTCOMES_MIN_CELL;
  if (eff_width < floor_width) {
    eff_width = floor_width;
  }
  return eff_width - (int)indent;
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

// Capitalizes the first letter of `text` in place, and returns it.
static char *peg_capitalize(char *text) {
  if (text[0] >= 'a' && text[0] <= 'z') {
    text[0] = (char)(text[0] - 'a' + 'A');
  }
  return text;
}

// A cut play's bound, the most its win% could reach: "<=25.0", or with more
// decimals where one would round a bound above zero down to "<=0.0", which
// reads as a play that lost every ordering. Caller frees.
static char *peg_bound_text(double win_pct) {
  const double pct = 100.0 * win_pct;
  int decimals = 1;
  while (decimals < PEG_BOUND_MAX_DECIMALS && pct > 0.0 &&
         pct < 0.5 * pow(10.0, -decimals)) {
    decimals++;
  }
  return get_formatted_string("<=%.*f", decimals, pct);
}

// A play's win% for a table: the most a cut play could reach, "-" for one a
// time limit left unscored, or the win%. Caller frees.
static char *peg_win_pct_text(bool cut, double win_pct) {
  if (cut) {
    return peg_bound_text(win_pct);
  }
  if (win_pct < 0.0) {
    return string_duplicate("-");
  }
  return get_formatted_string("%.1f", 100.0 * win_pct);
}

// True when any of the first n candidates has a known mean spread.
static bool peg_any_spread_known(const PegRankedCand *cands, int n) {
  for (int cand_idx = 0; cand_idx < n; cand_idx++) {
    if (!cands[cand_idx].spread_unknown) {
      return true;
    }
  }
  return false;
}

// One graded-table line's fixed cells, each padded to its width: depth (when
// shown), rank, move, wins, ties, losses, win%, spread (when shown), and the
// total time.
static void peg_append_graded_cells(StringBuilder *sb, bool show_depth,
                                    bool show_spread, int wd, const char *depth,
                                    int wr, const char *rank, int wm,
                                    const char *move, int ww, const char *wins,
                                    int wti, const char *ties, int wlo,
                                    const char *loss, int wp, const char *win,
                                    int wsp, const char *spread, int wtotal,
                                    const char *total) {
  if (show_depth) {
    string_builder_add_formatted_string(sb, "%-*s  ", wd, depth);
  }
  string_builder_add_formatted_string(sb, "%*s  %-*s  %*s  %*s  %*s  %*s", wr,
                                      rank, wm, move, ww, wins, wti, ties, wlo,
                                      loss, wp, win);
  if (show_spread) {
    string_builder_add_formatted_string(sb, "  %*s", wsp, spread);
  }
  string_builder_add_formatted_string(sb, "  %*s", wtotal, total);
}

// Render the final graded ranking: every play that entered a halving stage,
// grouped by the deepest endgame fidelity it reached. Deepest tier first, a
// dashed separator between tiers, and the rank continuing across them. Shows
// the first max_rows rows of the graded list (0 = all of it; its size is the
// post-greedy cutoff, e.g. 32 by default or the entire field under -pegtopk
// all), then how many more there are. The row buffers are sized to the list.
// When snap is non-NULL, shows "total" + per-fidelity time columns (one per
// non-greedy completed stage). When snap is NULL, shows a single "time" column.
static void peg_append_graded_table(StringBuilder *sb, const PegResult *result,
                                    const Board *board,
                                    const LetterDistribution *ld,
                                    const PegPollSnapshot *snap, int max_rows,
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
  // The depth column only when the plays reached different depths, and the
  // spread column only when some play has one.
  bool show_depth = false;
  for (int graded_idx = 1; graded_idx < total; graded_idx++) {
    if (result->graded_fidelity[graded_idx] != result->graded_fidelity[0]) {
      show_depth = true;
    }
  }
  const bool show_spread = peg_any_spread_known(result->graded_cands, total);

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
      depth[n_rows] = peg_fidelity_label(fid);
      rankc[n_rows] = get_formatted_string("%d", n_rows + 1);
      StringBuilder *move_sb = string_builder_create();
      string_builder_add_move(move_sb, board, &cand->move, ld, false);
      movec[n_rows] = string_builder_dump_and_destroy(move_sb, NULL);
      // A cut candidate (PegArgs.early_cutoff) has no counts, and its win%
      // is the most it could have reached.
      winsc[n_rows] = cand->cut
                          ? string_duplicate("-")
                          : get_formatted_string("%" PRId64, cand->win_count);
      tiesc[n_rows] = cand->cut
                          ? string_duplicate("-")
                          : get_formatted_string("%" PRId64, cand->tie_count);
      lossc[n_rows] =
          cand->cut ? string_duplicate("-")
                    : get_formatted_string("%" PRId64, cand->weight_sum -
                                                           cand->win_count -
                                                           cand->tie_count);
      winc[n_rows] = peg_win_pct_text(cand->cut, cand->win_pct);
      spreadc[n_rows] = cand->spread_unknown
                            ? string_duplicate("-")
                            : get_formatted_string("%+.1f", cand->mean_spread);
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

  size_t wd = show_depth ? string_length("Depth") : 0;
  size_t wr = string_length("Rank");
  size_t wm = string_length("Move");
  size_t ww = string_length("W");
  size_t wti = string_length("T");
  size_t wlo = string_length("L");
  size_t wp = string_length("Win%");
  size_t wsp = show_spread ? string_length("Spread") : 0;
  const char *total_hdr = show_time ? "Total" : "Time";
  size_t wtotal = string_length(total_hdr);
  // Per-fidelity column widths, labeled by their fidelity.
  char **fid_hdrs =
      show_time ? malloc_or_die((size_t)n_unique_fids * sizeof(char *)) : NULL;
  size_t *wfid =
      show_time ? malloc_or_die((size_t)n_unique_fids * sizeof(size_t)) : NULL;
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    fid_hdrs[uf_idx] = peg_capitalize(peg_fidelity_label(unique_fids[uf_idx]));
    wfid[uf_idx] = string_length(fid_hdrs[uf_idx]);
  }
  for (int row_idx = 0; row_idx < n_rows; row_idx++) {
    if (show_depth) {
      wd = string_length(depth[row_idx]) > wd ? string_length(depth[row_idx])
                                              : wd;
    }
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
    if (show_spread) {
      wsp = string_length(spreadc[row_idx]) > wsp
                ? string_length(spreadc[row_idx])
                : wsp;
    }
    wtotal = string_length(totalc[row_idx]) > wtotal
                 ? string_length(totalc[row_idx])
                 : wtotal;
    for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
      wfid[uf_idx] = string_length(timecols[uf_idx][row_idx]) > wfid[uf_idx]
                         ? string_length(timecols[uf_idx][row_idx])
                         : wfid[uf_idx];
    }
  }

  // Every column but the outcomes is its width plus a two-space gap before
  // the next. The optional depth and spread columns take none when hidden.
  size_t row_w = wr + wm + ww + wti + wlo + wp + wtotal + 12;
  if (show_depth) {
    row_w += wd + 2;
  }
  if (show_spread) {
    row_w += wsp + 2;
  }
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    row_w += 2 + wfid[uf_idx];
  }
  // Total row width for the tier-separator dashes.
  const size_t total_w = row_w;
  // Column where the outcomes cell content begins, after the "  " that
  // precedes it. Wrapped continuation lines indent to here so the column stays
  // aligned.
  const size_t outc_indent = row_w + 2;
  const int outc_avail = peg_outcomes_avail(outc_indent, out_width);

  // Header.
  peg_append_graded_cells(sb, show_depth, show_spread, (int)wd, "Depth",
                          (int)wr, "Rank", (int)wm, "Move", (int)ww, "W",
                          (int)wti, "T", (int)wlo, "L", (int)wp, "Win%",
                          (int)wsp, "Spread", (int)wtotal, total_hdr);
  for (int uf_idx = 0; uf_idx < n_unique_fids; uf_idx++) {
    string_builder_add_formatted_string(sb, "  %*s", (int)wfid[uf_idx],
                                        fid_hdrs[uf_idx]);
  }
  if (show_outc) {
    // Trailing, unaligned: outcomes strings vary in width per row, so padding
    // them to a common width would waste space; keep it as the last column.
    string_builder_add_string(sb, "  Outcomes");
  }
  string_builder_add_string(sb, "\n");

  // Data rows with tier-separator lines between fidelity groups.
  const int n_shown = max_rows > 0 && n_rows > max_rows ? max_rows : n_rows;
  for (int row_idx = 0; row_idx < n_rows; row_idx++) {
    if (row_idx >= n_shown) {
      if (show_outc) {
        free(outc[row_idx]);
      }
    } else if (row_idx > 0 && rowfid[row_idx] != rowfid[row_idx - 1]) {
      for (size_t dash_idx = 0; dash_idx < total_w; dash_idx++) {
        string_builder_add_char(sb, '-');
      }
      string_builder_add_char(sb, '\n');
    }
    if (row_idx < n_shown) {
      peg_append_graded_cells(sb, show_depth, show_spread, (int)wd,
                              depth[row_idx], (int)wr, rankc[row_idx], (int)wm,
                              movec[row_idx], (int)ww, winsc[row_idx], (int)wti,
                              tiesc[row_idx], (int)wlo, lossc[row_idx], (int)wp,
                              winc[row_idx], (int)wsp, spreadc[row_idx],
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
    }
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

  if (n_rows > n_shown) {
    string_builder_add_formatted_string(sb, "      (%d more)\n",
                                        n_rows - n_shown);
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

// Append the flat candidate ranking of result->top_cands, ranked by `ranks`
// (NULL = 1, 2, 3, ...), with a "..." row wherever the ranks skip. Numeric
// columns are right-aligned; the move column is left-aligned. The columns are
// rank, move, the orderings won, tied and lost (losses are weight_sum - wins -
// ties), win%, spread when any shown play has one, and with show_time each
// play's wall clock. With show_outcomes and captured outcomes
// (result->cand_outcomes), a trailing outcomes column shows each play's draws,
// wrapped to out_width and out_max_lines as in the graded view.
static void peg_append_flat_ranking(StringBuilder *sb, const PegResult *result,
                                    const Board *board,
                                    const LetterDistribution *ld,
                                    const int *ranks, bool show_time,
                                    bool show_outcomes, int out_width,
                                    int out_max_lines, bool *out_truncated) {
  const int n_shown = result->n_top_cands;
  const bool show_spread = peg_any_spread_known(result->top_cands, n_shown);
  int col = 0;
  const int rank_col = col++;
  const int move_col = col++;
  const int wins_col = col++;
  const int ties_col = col++;
  const int loss_col = col++;
  const int winpct_col = col++;
  const int spread_col = show_spread ? col++ : -1;
  const int time_col = show_time ? col++ : -1;
  const int num_cols = col;
  // Each grid row's candidate, or -1 for a "..." row where the ranks skip.
  int *row_cand = malloc_or_die((size_t)(2 * n_shown + 1) * sizeof(int));
  int num_rows = 1;
  row_cand[0] = -1;
  for (int cand_idx = 0; cand_idx < n_shown; cand_idx++) {
    const int rank = ranks != NULL ? ranks[cand_idx] : cand_idx + 1;
    int prev_rank = 0;
    if (cand_idx > 0) {
      prev_rank = ranks != NULL ? ranks[cand_idx - 1] : cand_idx;
    }
    if (rank > prev_rank + 1 && cand_idx > 0) {
      row_cand[num_rows++] = -1;
    }
    row_cand[num_rows++] = cand_idx;
  }

  StringGrid *sg = string_grid_create(num_rows, num_cols, 2);
  // Right-align every numeric column; the move column stays left-aligned.
  for (int col_idx = 0; col_idx < num_cols; col_idx++) {
    string_grid_set_col_right_align(sg, col_idx, col_idx != move_col);
  }

  string_grid_set_cell(sg, 0, rank_col, string_duplicate("Rank"));
  string_grid_set_cell(sg, 0, move_col, string_duplicate("Move"));
  string_grid_set_cell(sg, 0, wins_col, string_duplicate("W"));
  string_grid_set_cell(sg, 0, ties_col, string_duplicate("T"));
  string_grid_set_cell(sg, 0, loss_col, string_duplicate("L"));
  string_grid_set_cell(sg, 0, winpct_col, string_duplicate("Win%"));
  if (spread_col >= 0) {
    string_grid_set_cell(sg, 0, spread_col, string_duplicate("Spread"));
  }
  if (time_col >= 0) {
    string_grid_set_cell(sg, 0, time_col, string_duplicate("Time"));
  }

  for (int row = 1; row < num_rows; row++) {
    const int cand_idx = row_cand[row];
    if (cand_idx < 0) {
      string_grid_set_cell(sg, row, rank_col, string_duplicate("..."));
      for (int col_idx = 1; col_idx < num_cols; col_idx++) {
        string_grid_set_cell(sg, row, col_idx, string_duplicate(""));
      }
      continue;
    }
    const PegRankedCand *cand = &result->top_cands[cand_idx];
    string_grid_set_cell(sg, row, rank_col,
                         get_formatted_string("%d", ranks != NULL
                                                        ? ranks[cand_idx]
                                                        : cand_idx + 1));

    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, board, &cand->move, ld, false);
    string_grid_set_cell(sg, row, move_col,
                         string_builder_dump_and_destroy(move_sb, NULL));

    // A cut candidate has no counts, and its win% is the most it could have
    // reached.
    const int64_t loss_count =
        cand->weight_sum - cand->win_count - cand->tie_count;
    string_grid_set_cell(
        sg, row, wins_col,
        cand->cut ? string_duplicate("-")
                  : get_formatted_string("%" PRId64, cand->win_count));
    string_grid_set_cell(
        sg, row, ties_col,
        cand->cut ? string_duplicate("-")
                  : get_formatted_string("%" PRId64, cand->tie_count));
    string_grid_set_cell(sg, row, loss_col,
                         cand->cut
                             ? string_duplicate("-")
                             : get_formatted_string("%" PRId64, loss_count));
    string_grid_set_cell(sg, row, winpct_col,
                         peg_win_pct_text(cand->cut, cand->win_pct));
    if (spread_col >= 0) {
      string_grid_set_cell(
          sg, row, spread_col,
          cand->spread_unknown
              ? string_duplicate("-")
              : get_formatted_string("%+.1f", cand->mean_spread));
    }
    if (time_col >= 0) {
      string_grid_set_cell(sg, row, time_col,
                           get_formatted_string("%.1fs", cand->eval_seconds));
    }
  }

  if (!show_outcomes || result->n_cand_outcomes == 0) {
    string_builder_add_string_grid(sb, sg, false);
    string_grid_destroy(sg);
    free(row_cand);
    return;
  }
  // Every grid line has the same width and ends in the column padding, so the
  // outcomes cell starts right after it, and wrapped lines indent to there.
  StringBuilder *grid_sb = string_builder_create();
  string_builder_add_string_grid(grid_sb, sg, false);
  string_grid_destroy(sg);
  const char *line = string_builder_peek(grid_sb);
  for (int row = 0; row < num_rows; row++) {
    const char *line_end = strchr(line, '\n');
    char *row_str = get_substring(line, 0, (int)(line_end - line));
    string_builder_add_string(sb, row_str);
    if (row == 0) {
      string_builder_add_string(sb, "Outcomes");
    } else if (row_cand[row] >= 0) {
      const PegCandOutcomes *oc = peg_find_cand_outcomes(
          result, &result->top_cands[row_cand[row]].move);
      char *cell = (oc != NULL && oc->n_rows > 0)
                       ? peg_build_outcomes_string_rows(oc->rows, oc->n_rows)
                       : string_duplicate("-");
      const size_t indent = visual_string_length(row_str);
      peg_append_wrapped(sb, cell, (int)indent,
                         peg_outcomes_avail(indent, out_width), out_max_lines,
                         out_truncated);
      free(cell);
    }
    free(row_str);
    string_builder_add_string(sb, "\n");
    line = line_end + 1;
  }
  string_builder_destroy(grid_sb);
  free(row_cand);
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
  if (fidelity == 0) {
    return string_duplicate("greedy");
  }
  // An exhaustive stage searches each endgame to the end of the game.
  if (fidelity >= PEG_EXHAUSTIVE_PLIES) {
    return string_duplicate("end");
  }
  return get_formatted_string("%d-ply", fidelity);
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

// A draw's status: running, stopped, or for a done one won, lost or tied when
// every one of its orderings went that way, else its orderings won, tied and
// lost.
static char *peg_draw_status_text(const PegDrawStatus *draw) {
  if (draw->state == PEG_DRAW_RUNNING) {
    return string_duplicate("running");
  }
  if (draw->state != PEG_DRAW_DONE || draw->weight_sum <= 0) {
    return string_duplicate("stopped");
  }
  const int64_t losses = draw->weight_sum - draw->win_count - draw->tie_count;
  if (draw->win_count == draw->weight_sum) {
    return string_duplicate("won");
  }
  if (losses == draw->weight_sum) {
    return string_duplicate("lost");
  }
  if (draw->tie_count == draw->weight_sum) {
    return string_duplicate("tied");
  }
  return get_formatted_string("%" PRId64 "W %" PRId64 "T %" PRId64 "L",
                              draw->win_count, draw->tie_count, losses);
}

// A draw as the outcomes write it: the mover's tiles, then the bag it leaves
// ("drawn/bag"), with "-" when the mover draws nothing. Caller frees.
static char *peg_draw_text(const PegDrawStatus *draw,
                           const LetterDistribution *ld) {
  StringBuilder *drawn_sb = string_builder_create();
  for (int tile_idx = 0; tile_idx < draw->n_drawn; tile_idx++) {
    string_builder_add_user_visible_letter(drawn_sb, ld, draw->drawn[tile_idx]);
  }
  if (draw->n_drawn == 0) {
    string_builder_add_char(drawn_sb, '-');
  }
  if (draw->n_bag > 0) {
    string_builder_add_char(drawn_sb, '/');
  }
  for (int tile_idx = 0; tile_idx < draw->n_bag; tile_idx++) {
    string_builder_add_user_visible_letter(drawn_sb, ld, draw->bag[tile_idx]);
  }
  return string_builder_dump_and_destroy(drawn_sb, NULL);
}

// Fills cells first_col and first_col + 1 of `row` with the endgame a running
// draw's thread is searching: its depth, and the root moves searched of all at
// that depth. With no endgame to show, the first cell says why, and the other
// is "-":
// - "nested": the draw leaves tiles in the bag, so its thread works through
//   the opponent's replies and later draws rather than one endgame;
// - `idle` (when not NULL): the row has no draw running, such as a candidate
//   between its draws;
// - "-": otherwise, as when one thread solves without endgame helpers.
static void peg_set_endgame_cells(StringGrid *sg, int row, int first_col,
                                  const PegDrawStatus *draw, const char *idle) {
  const bool running = draw->state == PEG_DRAW_RUNNING;
  if (running && draw->in_endgame) {
    string_grid_set_cell(sg, row, first_col,
                         get_formatted_string("%d", draw->depth));
    string_grid_set_cell(sg, row, first_col + 1,
                         get_formatted_string("%d/%d", draw->root_moves_done,
                                              draw->root_moves_total));
    return;
  }
  const char *reason = "-";
  if (running && (draw->n_bag > 0 || draw->n_drawn == 0)) {
    reason = "nested";
  } else if (!running && idle != NULL) {
    reason = idle;
  }
  string_grid_set_cell(sg, row, first_col, string_duplicate(reason));
  string_grid_set_cell(sg, row, first_col + 1, string_duplicate("-"));
}

// The candidate in progress, when candidates are solved one at a time: which
// it is, then a row for each running draw, with the endgame it is searching,
// and for each finished one when there are few; past
// PEG_STATUS_MAX_FINISHED_DRAW_ROWS draws, the finished ones are totalled
// instead.
static void peg_append_live_candidate(StringBuilder *sb,
                                      const PegPollSnapshot *snap,
                                      const Game *game, int64_t now_ns) {
  if (snap->eval_start_ns == 0 || snap->currently_evaluating_move_idx < 0 ||
      snap->n_stage_history == 0) {
    return;
  }
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  StringBuilder *move_sb = string_builder_create();
  string_builder_add_move(move_sb, board, &snap->evaluating_move, ld, false);
  string_builder_add_formatted_string(sb, "Currently working on %s\n",
                                      string_builder_peek(move_sb));
  string_builder_destroy(move_sb);
  const int n_draws = snap->draws_queued + snap->draws_running +
                      snap->draws_done + snap->draws_stopped;
  if (n_draws == 0) {
    return;
  }
  const bool list_finished = n_draws <= PEG_STATUS_MAX_FINISHED_DRAW_ROWS;
  int n_rows = 0;
  for (int row_idx = 0; row_idx < snap->n_draw_rows; row_idx++) {
    if (list_finished || snap->draw_rows[row_idx].state == PEG_DRAW_RUNNING) {
      n_rows++;
    }
  }
  if (n_rows > 0) {
    StringGrid *sg = string_grid_create(n_rows + 1, 6, 2);
    const char *headers[] = {"Draw",   "Status", "Time",
                             "Spread", "Depth",  "Moves"};
    for (int col_idx = 0; col_idx < 6; col_idx++) {
      string_grid_set_cell(sg, 0, col_idx, string_duplicate(headers[col_idx]));
    }
    int grid_row = 1;
    for (int row_idx = 0; row_idx < snap->n_draw_rows; row_idx++) {
      const PegDrawStatus *draw = &snap->draw_rows[row_idx];
      const bool running = draw->state == PEG_DRAW_RUNNING;
      if (!list_finished && !running) {
        continue;
      }
      string_grid_set_cell(sg, grid_row, 0, peg_draw_text(draw, ld));
      string_grid_set_cell(sg, grid_row, 1, peg_draw_status_text(draw));
      const int64_t end_ns = running ? now_ns : draw->end_ns;
      string_grid_set_cell(
          sg, grid_row, 2,
          get_formatted_string("%.1fs",
                               (double)(end_ns - draw->start_ns) / 1e9));
      string_grid_set_cell(
          sg, grid_row, 3,
          draw->state == PEG_DRAW_DONE && !draw->spread_unknown
              ? get_formatted_string("%+.1f", draw->mean_spread)
              : string_duplicate("-"));
      peg_set_endgame_cells(sg, grid_row, 4, draw, /*idle=*/NULL);
      grid_row++;
    }
    string_builder_add_string_grid(sb, sg, false);
    string_grid_destroy(sg);
  }
  if (!list_finished && snap->draws_done > 0) {
    string_builder_add_formatted_string(
        sb,
        "%d finished draws: orderings won %" PRId64 ", tied %" PRId64
        ", lost %" PRId64 "\n",
        snap->draws_done, snap->draws_done_wins, snap->draws_done_ties,
        snap->draws_done_weight - snap->draws_done_wins -
            snap->draws_done_ties);
  }
}

// The candidates solved at once, one per thread: how many, then a row for
// each, longest running first: how many of its draws it has solved of all it
// has, how long it has run, and the draw it is solving, with the endgame its
// thread is searching.
static void peg_append_live_cands(StringBuilder *sb,
                                  const PegPollSnapshot *snap, const Game *game,
                                  int64_t now_ns) {
  string_builder_add_formatted_string(
      sb, "Currently working on %d candidate%s:\n", snap->cands_running,
      snap->cands_running == 1 ? "" : "s");
  if (snap->n_cand_rows > 0) {
    const Board *board = game_get_board(game);
    const LetterDistribution *ld = game_get_ld(game);
    StringGrid *sg = string_grid_create(snap->n_cand_rows + 1, 6, 2);
    const char *headers[] = {"Move", "Draws", "Time", "Draw", "Depth", "Moves"};
    for (int col_idx = 0; col_idx < 6; col_idx++) {
      string_grid_set_cell(sg, 0, col_idx, string_duplicate(headers[col_idx]));
    }
    for (int row_idx = 0; row_idx < snap->n_cand_rows; row_idx++) {
      const PegCandStatus *cand = &snap->cand_rows[row_idx];
      const int grid_row = row_idx + 1;
      StringBuilder *move_sb = string_builder_create();
      string_builder_add_move(move_sb, board, &cand->move, ld, false);
      string_grid_set_cell(sg, grid_row, 0,
                           string_builder_dump_and_destroy(move_sb, NULL));
      string_grid_set_cell(sg, grid_row, 1,
                           cand->draws_total > 0
                               ? get_formatted_string("%d/%d", cand->draws_done,
                                                      cand->draws_total)
                               : string_duplicate("-"));
      string_grid_set_cell(
          sg, grid_row, 2,
          get_formatted_string("%.1fs",
                               (double)(now_ns - cand->start_ns) / 1e9));
      string_grid_set_cell(sg, grid_row, 3,
                           cand->draw.state == PEG_DRAW_RUNNING
                               ? peg_draw_text(&cand->draw, ld)
                               : string_duplicate("-"));
      peg_set_endgame_cells(sg, grid_row, 4, &cand->draw,
                            cand->draws_done == 0 ? NULL : "between");
    }
    string_builder_add_string_grid(sb, sg, false);
    string_grid_destroy(sg);
  }
}

// ----- summary ---------------------------------------------------------------

// One summary line: its label, padded, then `value`, which it frees.
static void peg_append_summary_line(StringBuilder *sb, const char *label,
                                    char *value) {
  string_builder_add_formatted_string(sb, "%-*s%s\n", PEG_SUMMARY_LABEL_WIDTH,
                                      label, value);
  free(value);
}

// One indented line under a summary line, its value in the summary's value
// column. Frees `value`.
static void peg_append_summary_subline(StringBuilder *sb, const char *label,
                                       char *value) {
  string_builder_add_formatted_string(
      sb, "%*s%-*s%s\n", PEG_SUMMARY_SUBLINE_INDENT, "",
      PEG_SUMMARY_LABEL_WIDTH - PEG_SUMMARY_SUBLINE_INDENT, label, value);
  free(value);
}

// A stage's candidates finished of all it has. Caller frees.
static char *peg_progress_text(const PegStageSnapshot *stage) {
  const int pct =
      stage->field_size > 0 ? 100 * stage->cands_done / stage->field_size : 0;
  return get_formatted_string("%d of %d (%d%%)", stage->cands_done,
                              stage->field_size, pct);
}

// A line for each stage, under the status line: how long it took, or has
// taken by now_ns, and whether it has finished or is running. With more than
// one endgame stage, each gives its candidates too.
static void peg_append_stage_lines(StringBuilder *sb,
                                   const PegStageSnapshot *history,
                                   int n_history, int64_t now_ns) {
  for (int stage_idx = 0; stage_idx < n_history; stage_idx++) {
    const PegStageSnapshot *stage = &history[stage_idx];
    char *label = peg_capitalize(
        stage->fidelity_plies == 0 ? string_duplicate("greedy")
                                   : peg_fidelity_label(stage->fidelity_plies));
    const bool finished = stage->end_ns != 0;
    const int64_t end_ns = finished ? stage->end_ns : now_ns;
    const double secs = (double)(end_ns - stage->start_ns) / 1e9;
    const char *state = finished ? "finished" : "running";
    peg_append_summary_subline(
        sb, label,
        n_history > 2 ? get_formatted_string("%.1fs (%s), %d candidates", secs,
                                             state, stage->field_size)
                      : get_formatted_string("%.1fs (%s)", secs, state));
    free(label);
  }
}

// A play and its win%: the greedy seed's estimate, the most a cut play could
// reach, or the orderings it won, tied and lost. Caller frees.
static char *peg_best_text(const PegRankedCand *cand, const Board *board,
                           const LetterDistribution *ld, bool estimate) {
  StringBuilder *text_sb = string_builder_create();
  string_builder_add_move(text_sb, board, &cand->move, ld, false);
  if (!cand->cut && cand->win_pct < 0.0) {
    string_builder_add_string(text_sb, "  not scored");
  } else if (estimate) {
    string_builder_add_formatted_string(text_sb, "  %.1f%% (greedy)",
                                        100.0 * cand->win_pct);
  } else if (cand->cut) {
    char *bound = peg_bound_text(cand->win_pct);
    string_builder_add_formatted_string(text_sb, "  %s%%", bound);
    free(bound);
  } else {
    string_builder_add_formatted_string(
        text_sb, "  %.1f%%  (%" PRId64 " W, %" PRId64 " T, %" PRId64 " L)",
        100.0 * cand->win_pct, cand->win_count, cand->tie_count,
        cand->weight_sum - cand->win_count - cand->tie_count);
  }
  return string_builder_dump_and_destroy(text_sb, NULL);
}

// A count and its share of `total`: "57 (28.5%)". Caller frees.
static char *peg_count_text(int count, int total) {
  return get_formatted_string("%d (%.1f%%)", count,
                              total > 0 ? 100.0 * count / total : 0.0);
}

// The lines under "Progress": the candidates solved (finished) and cut, and
// those left (labelled `left_label`) when there are any, each with its share
// of `total`.
static void peg_append_progress_lines(StringBuilder *sb, int n_solved,
                                      int n_cut, int n_left,
                                      const char *left_label, int total) {
  peg_append_summary_subline(sb, "Finished", peg_count_text(n_solved, total));
  peg_append_summary_subline(sb, "Cut", peg_count_text(n_cut, total));
  if (n_left > 0) {
    peg_append_summary_subline(sb, left_label, peg_count_text(n_left, total));
  }
}

// The bag orderings every play is scored over, and the distinct draws among
// them. Caller frees.
static char *peg_draws_text(const PegRankedCand *cand) {
  return get_formatted_string("%" PRId64 " orderings (%d unique draws)",
                              cand->weight_sum, cand->n_scenarios);
}

// The running solve's summary: how long it has run, each stage's time and
// state, the current stage's progress with its finished, cut and pending
// candidates, the orderings, and the best move so far (the greedy estimate
// until an endgame stage finishes a candidate).
static void peg_append_live_summary(StringBuilder *sb,
                                    const PegPollSnapshot *snap,
                                    const Game *game, int64_t now_ns,
                                    const PegRankedCand *entries, int n_entries,
                                    bool from_baseline) {
  const int64_t start_ns =
      snap->n_stage_history > 0 ? snap->stage_history[0].start_ns : now_ns;
  peg_append_summary_line(
      sb, "Status",
      get_formatted_string("%.1fs (running)",
                           (double)(now_ns - start_ns) / 1e9));
  if (snap->n_stage_history == 0) {
    return;
  }
  peg_append_stage_lines(sb, snap->stage_history, snap->n_stage_history,
                         now_ns);
  const PegStageSnapshot *stage =
      &snap->stage_history[snap->n_stage_history - 1];
  const int fidelity = stage->fidelity_plies;
  peg_append_summary_line(sb, "Progress", peg_progress_text(stage));
  if (fidelity > 0) {
    peg_append_progress_lines(
        sb, stage->cands_done - stage->cands_cut, stage->cands_cut,
        stage->field_size - stage->cands_done, "Pending", stage->field_size);
  }
  const bool estimate = from_baseline || fidelity == 0;
  if (n_entries > 0) {
    peg_append_summary_line(sb, "Draws", peg_draws_text(&entries[0]));
    peg_append_summary_line(sb, "Best Move",
                            peg_best_text(&entries[0], game_get_board(game),
                                          game_get_ld(game), estimate));
  }
}

// The finished (or stopped) solve's summary: how it ended and when, each
// stage's time, the progress of its last completed stage with how many of the
// ranked plays were solved and cut, the orderings, and the best move.
static void peg_append_result_summary(StringBuilder *sb,
                                      const PegResult *result,
                                      const Game *game) {
  const double secs = ctimer_elapsed_seconds(&result->timer);
  const int last = result->last_completed_stage;
  char *status = NULL;
  if (last < 0) {
    status = get_formatted_string("%.1fs (running)", secs);
  } else if (result->last_stage_partial) {
    status =
        get_formatted_string("%.1fs (stopped during stage %d)", secs, last);
  } else if (result->can_resume) {
    status = get_formatted_string("%.1fs (stopped after stage %d)", secs, last);
  } else {
    status = get_formatted_string("%.1fs (finished)", secs);
  }
  peg_append_summary_line(sb, "Status", status);
  peg_append_stage_lines(sb, result->stage_history, result->n_stage_history,
                         ctimer_monotonic_ns());
  const PegStageSnapshot *stage = last >= 0 && last < result->n_stage_history
                                      ? &result->stage_history[last]
                                      : NULL;
  const bool estimate = stage != NULL && stage->fidelity_plies == 0;
  if (stage != NULL) {
    peg_append_summary_line(sb, "Progress", peg_progress_text(stage));
    if (!estimate) {
      int n_cut = 0;
      for (int cand_idx = 0; cand_idx < result->n_top_cands; cand_idx++) {
        if (result->top_cands[cand_idx].cut) {
          n_cut++;
        }
      }
      peg_append_progress_lines(sb, result->n_top_cands - n_cut, n_cut,
                                stage->field_size - stage->cands_done,
                                "Unsolved", stage->field_size);
    }
  }
  if (result->n_top_cands > 0) {
    peg_append_summary_line(sb, "Draws", peg_draws_text(&result->top_cands[0]));
    peg_append_summary_line(sb, "Best Move",
                            peg_best_text(&result->top_cands[0],
                                          game_get_board(game),
                                          game_get_ld(game), estimate));
  }
}

// How many more plays there are than the ranking lists, and how many of them
// are solved (n_more_solved < 0 when the rows are greedy estimates, neither
// solved nor cut).
static void peg_append_more_line(StringBuilder *sb, int n_more,
                                 int n_more_solved) {
  if (n_more <= 0) {
    return;
  }
  if (n_more_solved < 0) {
    string_builder_add_formatted_string(sb, "      (%d more)\n", n_more);
  } else if (n_more_solved > 0) {
    string_builder_add_formatted_string(
        sb, "      (%d more, including %d solved)\n", n_more, n_more_solved);
  } else {
    string_builder_add_formatted_string(sb, "      (%d more, all cut)\n",
                                        n_more);
  }
}

// The finished ranking, as status shows the live one: the first max_rows rows
// of result->top_cands (0 = every play), then up to
// PEG_DISPLAY_MAX_PINNED_ROWS solved plays ranked below them, at their ranks,
// and how many more plays there are. Greedy estimates (estimate) are neither
// solved nor cut, so none are pinned.
static void peg_append_result_ranking(
    StringBuilder *sb, const PegResult *result, const Board *board,
    const LetterDistribution *ld, int max_rows, bool estimate, bool show_time,
    bool show_outcomes, int out_width, int out_max_lines, bool *out_truncated) {
  const int n_cands = result->n_top_cands;
  const int n_top = max_rows > 0 && n_cands > max_rows ? max_rows : n_cands;
  const int max_listed = n_top + PEG_DISPLAY_MAX_PINNED_ROWS;
  PegRankedCand *ranking =
      malloc_or_die((size_t)max_listed * sizeof(PegRankedCand));
  int *ranks = malloc_or_die((size_t)max_listed * sizeof(int));
  int n_rows = 0;
  int n_solved = 0;
  int n_listed_solved = 0;
  for (int cand_idx = 0; cand_idx < n_cands; cand_idx++) {
    const PegRankedCand *cand = &result->top_cands[cand_idx];
    if (!cand->cut) {
      n_solved++;
    }
    const bool listed =
        cand_idx < n_top || (!estimate && !cand->cut && n_rows < max_listed);
    if (!listed) {
      continue;
    }
    ranking[n_rows] = *cand;
    ranks[n_rows] = cand_idx + 1;
    n_rows++;
    if (!cand->cut) {
      n_listed_solved++;
    }
  }
  PegResult listed = {0};
  listed.top_cands = ranking;
  listed.n_top_cands = n_rows;
  // The listed result only borrows the outcomes.
  listed.cand_outcomes = result->cand_outcomes;
  listed.n_cand_outcomes = result->n_cand_outcomes;
  peg_append_flat_ranking(sb, &listed, board, ld, ranks, show_time,
                          show_outcomes, out_width, out_max_lines,
                          out_truncated);
  free(ranking);
  free(ranks);
  peg_append_more_line(sb, n_cands - n_rows,
                       estimate ? -1 : n_solved - n_listed_solved);
}

// The live ranking: its first PEG_DISPLAY_ROWS rows, then up to
// PEG_DISPLAY_MAX_PINNED_ROWS solved plays ranked below them, at their ranks,
// and how many more plays have finished. The seed's rows are greedy
// estimates, and are shown until the endgame stage finishes a candidate
// (from_baseline). With show_outcomes, the rows have an outcomes column from
// the finished candidates' `outcomes`, which come from the poll.
static void
peg_append_live_ranking(StringBuilder *sb, const PegPollSnapshot *snap,
                        const Game *game, const PegRankedCand *entries,
                        int n_entries, bool from_baseline, bool show_outcomes,
                        PegCandOutcomes *outcomes, int n_outcomes,
                        int out_width, int out_max_lines, bool *out_truncated) {
  const PegStageSnapshot *stage =
      snap->n_stage_history > 0
          ? &snap->stage_history[snap->n_stage_history - 1]
          : NULL;
  const bool estimate =
      from_baseline || (stage != NULL && stage->fidelity_plies == 0);
  if (estimate) {
    string_builder_add_formatted_string(
        sb, "Greedy ranking%s\n",
        from_baseline ? ", until a candidate finishes" : "");
  }
  const int n_top = n_entries < PEG_DISPLAY_ROWS ? n_entries : PEG_DISPLAY_ROWS;
  PegRankedCand ranking[PEG_DISPLAY_ROWS + PEG_DISPLAY_MAX_PINNED_ROWS];
  int ranks[PEG_DISPLAY_ROWS + PEG_DISPLAY_MAX_PINNED_ROWS];
  int n_rows = 0;
  int n_listed_solved = 0;
  for (int entry_idx = 0; entry_idx < n_top; entry_idx++) {
    ranking[n_rows] = entries[entry_idx];
    ranks[n_rows] = entry_idx + 1;
    n_rows++;
    if (!entries[entry_idx].cut) {
      n_listed_solved++;
    }
  }
  // The solved plays below the top rows, which the top entries can miss.
  if (!estimate) {
    for (int solved_idx = 0; solved_idx < snap->n_solved_rows &&
                             n_rows < n_top + PEG_DISPLAY_MAX_PINNED_ROWS;
         solved_idx++) {
      if (snap->solved_ranks[solved_idx] <= n_top) {
        continue;
      }
      ranking[n_rows] = snap->solved_rows[solved_idx];
      ranks[n_rows] = snap->solved_ranks[solved_idx];
      n_rows++;
      n_listed_solved++;
    }
  }
  PegResult live = {0};
  live.top_cands = ranking;
  live.n_top_cands = n_rows;
  live.last_completed_stage = snap->stage;
  // The live result only borrows the outcomes; the caller frees them.
  live.cand_outcomes = outcomes;
  live.n_cand_outcomes = n_outcomes;
  peg_append_flat_ranking(sb, &live, game_get_board(game), game_get_ld(game),
                          ranks, /*show_time=*/false, show_outcomes, out_width,
                          out_max_lines, out_truncated);
  // The stage counts every finished play, listed or not.
  int n_finished = n_entries;
  int n_solved = -1;
  if (from_baseline) {
    if (snap->n_stage_history >= 2) {
      n_finished = snap->stage_history[snap->n_stage_history - 2].cands_done;
    }
  } else if (stage != NULL) {
    n_finished = stage->cands_done;
    if (stage->fidelity_plies > 0) {
      n_solved = stage->cands_done - stage->cands_cut;
    }
  }
  peg_append_more_line(sb, n_finished - n_rows,
                       n_solved < 0 ? -1 : n_solved - n_listed_solved);
}

// The live view of a running solve, as status shows it at `now_ns`: the
// summary, the candidate or candidates being solved, and the best ranking so
// far -- the current stage's, or the previous stage's while the current one
// has finished no candidate.
static void
peg_append_live_status(StringBuilder *sb, const PegPollSnapshot *snap,
                       const Game *game, int64_t now_ns, bool show_outcomes,
                       PegCandOutcomes *outcomes, int n_outcomes, int out_width,
                       int out_max_lines, bool *out_truncated) {
  const PegRankedCand *live_entries = snap->entries;
  int n_live_entries = snap->n_entries;
  const bool from_baseline =
      n_live_entries == 0 && snap->n_baseline_entries > 0;
  if (from_baseline) {
    live_entries = snap->baseline_entries;
    n_live_entries = snap->n_baseline_entries;
  }
  peg_append_live_summary(sb, snap, game, now_ns, live_entries, n_live_entries,
                          from_baseline);
  // A blank line before each section that follows.
  StringBuilder *solving_sb = string_builder_create();
  if (snap->cands_running > 0) {
    peg_append_live_cands(solving_sb, snap, game, now_ns);
  } else {
    peg_append_live_candidate(solving_sb, snap, game, now_ns);
  }
  if (string_builder_length(solving_sb) > 0) {
    string_builder_add_formatted_string(sb, "\n%s",
                                        string_builder_peek(solving_sb));
  }
  string_builder_destroy(solving_sb);
  if (n_live_entries == 0) {
    return;
  }
  string_builder_add_string(sb, "\n");
  peg_append_live_ranking(sb, snap, game, live_entries, n_live_entries,
                          from_baseline, show_outcomes, outcomes, n_outcomes,
                          out_width, out_max_lines, out_truncated);
}

char *peg_snapshot_get_status_string(const PegPollSnapshot *snapshot,
                                     const Game *game, int64_t now_ns,
                                     PegCandOutcomes *outcomes,
                                     int n_outcomes) {
  StringBuilder *sb = string_builder_create();
  peg_append_live_status(sb, snapshot, game, now_ns, outcomes != NULL, outcomes,
                         n_outcomes, /*out_width=*/0,
                         /*out_max_lines=*/0, NULL);
  return string_builder_dump_and_destroy(sb, NULL);
}

char *peg_result_get_string(const PegResult *result, const Game *game,
                            bool show_outcomes, PegPoll *poll, int max_rows,
                            int out_width, int out_max_lines,
                            const char *trunc_note, bool *out_truncated) {
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
    // Each finished candidate's per-draw outcomes, fed live via the poll.
    PegCandOutcomes *live_oc = NULL;
    int live_n_oc = 0;
    if (show_outcomes) {
      peg_poll_copy_outcomes(poll, &live_oc, &live_n_oc);
    }
    peg_append_live_status(sb, &snap, game, ctimer_monotonic_ns(),
                           show_outcomes, live_oc, live_n_oc, out_width,
                           out_max_lines, out_truncated);
    peg_cand_outcomes_destroy_array(live_oc, live_n_oc);
    char *out = string_builder_dump_and_destroy(sb, NULL);
    return out;
  }

  if (result->last_completed_stage < 0 && result->n_stage_history == 0) {
    string_builder_add_string(sb, "no PEG results.\n");
    char *out = string_builder_dump_and_destroy(sb, NULL);
    return out;
  }

  peg_append_result_summary(sb, result, game);
  if (result->n_top_cands == 0) {
    char *out = string_builder_dump_and_destroy(sb, NULL);
    return out;
  }
  string_builder_add_string(sb, "\n");

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  // Final (graded) view: group the plays that reached each stage, deepest
  // first. With a single tier (a complete solve), the plays are ranked as
  // status ranks them: the top rows, then the solved plays below them. The
  // flat table below is used for a result that never finished a halving stage
  // (e.g. the time limit hit during greedy); in that case there are no tiers,
  // so just show the post-greedy ranking.
  bool one_tier = true;
  for (int graded_idx = 1; graded_idx < result->n_graded; graded_idx++) {
    if (result->graded_fidelity[graded_idx] != result->graded_fidelity[0]) {
      one_tier = false;
    }
  }
  if (result->n_graded > 0 && !one_tier) {
    // The graded table carries a per-play outcomes column itself (when
    // show_outcomes), so no separate "outcomes (best)" line is needed here.
    peg_append_graded_table(sb, result, board, ld, have_snap ? &snap : NULL,
                            max_rows, show_outcomes, out_width, out_max_lines,
                            trunc_note, out_truncated);
    char *out = string_builder_dump_and_destroy(sb, NULL);
    return out;
  }
  if (trunc_note != NULL) {
    string_builder_add_formatted_string(sb, "%s\n", trunc_note);
  }
  if (result->n_graded > 0) {
    PegResult graded = {0};
    graded.top_cands = result->graded_cands;
    graded.n_top_cands = result->n_graded;
    graded.cand_outcomes = result->cand_outcomes;
    graded.n_cand_outcomes = result->n_cand_outcomes;
    peg_append_result_ranking(sb, &graded, board, ld, max_rows,
                              /*estimate=*/false,
                              /*show_time=*/true, show_outcomes, out_width,
                              out_max_lines, out_truncated);
    char *out = string_builder_dump_and_destroy(sb, NULL);
    return out;
  }

  const int last = result->last_completed_stage;
  const bool estimate = last < 0 || last >= result->n_stage_history ||
                        result->stage_history[last].fidelity_plies == 0;
  peg_append_result_ranking(sb, result, board, ld, max_rows, estimate,
                            /*show_time=*/false, /*show_outcomes=*/false,
                            out_width, out_max_lines, out_truncated);
  if (show_outcomes && result->n_per_scenario > 0) {
    char *outcomes = peg_build_outcomes_string(result);
    string_builder_add_formatted_string(sb, "\noutcomes (best): %s\n",
                                        outcomes);
    free(outcomes);
  }

  char *out = string_builder_dump_and_destroy(sb, NULL);
  return out;
}

static void peg_json_move(StringBuilder *sb, const Move *move,
                          const Game *game) {
  StringBuilder *text = string_builder_create();
  string_builder_add_move(text, game_get_board(game), move, game_get_ld(game),
                          false);
  string_builder_add_json_string(sb, string_builder_peek(text));
  string_builder_destroy(text);
}

// %f may use a multi-byte decimal separator in the embedding process locale.
// Fixed-point formatting has no grouping; copy digits/sign and normalize the
// sole separator without changing the process-global locale.
static void peg_json_decimal(StringBuilder *sb, double value, int precision) {
  char *formatted = get_formatted_string("%.*f", precision, value);
  bool separator_written = false;
  for (const char *cursor = formatted; *cursor; cursor++) {
    if ((*cursor >= '0' && *cursor <= '9') || *cursor == '-') {
      string_builder_add_char(sb, *cursor);
    } else if (!separator_written) {
      string_builder_add_char(sb, '.');
      separator_written = true;
    }
  }
  free(formatted);
}

static void peg_json_candidates(StringBuilder *sb, const PegRankedCand *rows,
                                int count, int depth, const Game *game) {
  string_builder_add_string(sb, "[");
  for (int entry_idx = 0; entry_idx < count; entry_idx++) {
    const PegRankedCand *row = &rows[entry_idx];
    string_builder_add_string(sb, entry_idx > 0 ? ",{\"move\":" : "{\"move\":");
    peg_json_move(sb, &row->move, game);
    string_builder_add_formatted_string(
        sb, ",\"score\":%d", equity_to_int(move_get_score(&row->move)));
    string_builder_add_string(sb, ",\"win\":");
    if (row->weight_sum > 0 && isfinite(row->win_pct) && row->win_pct >= 0) {
      peg_json_decimal(sb, 100.0 * row->win_pct, 6);
    } else {
      string_builder_add_string(sb, "null");
    }
    string_builder_add_string(sb, ",\"spread\":");
    if (row->spread_unknown) {
      string_builder_add_string(sb, "null");
    } else {
      peg_json_decimal(sb, isfinite(row->mean_spread) ? row->mean_spread : 0.0,
                       6);
    }
    string_builder_add_formatted_string(
        sb,
        ",\"wins\":%" PRId64 ",\"ties\":%" PRId64 ",\"losses\":%" PRId64
        ",\"depth\":%d,\"cut\":%s}",
        row->win_count, row->tie_count,
        row->weight_sum - row->win_count - row->tie_count, depth,
        row->cut ? "true" : "false");
  }
  string_builder_add_string(sb, "]");
}

char *peg_snapshot_get_json(const PegPollSnapshot *snapshot, const Game *game) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb,
      "{\"stage\":%d,\"depth\":%d,\"done\":%s,\"fieldSize\":%d,\"stages\":[",
      snapshot->stage, snapshot->fidelity_plies,
      snapshot->done ? "true" : "false", snapshot->field_size);
  const int64_t now = ctimer_monotonic_ns();
  for (int entry_idx = 0; entry_idx < snapshot->n_stage_history; entry_idx++) {
    const PegStageSnapshot *stage = &snapshot->stage_history[entry_idx];
    string_builder_add_formatted_string(
        sb,
        "%s{\"depth\":%d,\"completed\":%d,\"total\":%d,\"closed\":%s,"
        "\"seconds\":",
        entry_idx > 0 ? "," : "", stage->fidelity_plies, stage->cands_done,
        stage->field_size, stage->end_ns ? "true" : "false");
    peg_json_decimal(
        sb,
        stage->start_ns > 0
            ? fmax(0.0, (double)((stage->end_ns ? stage->end_ns : now) -
                                 stage->start_ns) /
                            1e9)
            : 0.0,
        3);
    string_builder_add_char(sb, '}');
  }
  string_builder_add_formatted_string(sb, "],\"canResume\":%s,\"moves\":[",
                                      snapshot->can_resume ? "true" : "false");
  for (int entry_idx = 0; entry_idx < snapshot->n_stage_moves; entry_idx++) {
    if (entry_idx > 0) {
      string_builder_add_string(sb, ",");
    }
    peg_json_move(sb, &snapshot->stage_moves[entry_idx], game);
  }
  string_builder_add_string(sb, "],\"evaluating\":");
  const int active = snapshot->currently_evaluating_move_idx;
  if (!snapshot->done && active >= 0 && active < snapshot->n_stage_moves) {
    peg_json_move(sb, &snapshot->stage_moves[active], game);
  } else {
    string_builder_add_string(sb, "null");
  }
  string_builder_add_string(sb, ",\"entries\":");
  peg_json_candidates(sb, snapshot->entries, snapshot->n_entries,
                      snapshot->fidelity_plies, game);
  string_builder_add_string(sb, ",\"baseline\":");
  peg_json_candidates(sb, snapshot->baseline_entries,
                      snapshot->n_baseline_entries, snapshot->baseline_fidelity,
                      game);
  string_builder_add_string(sb, "}");
  char *json = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return json;
}

char *peg_poll_get_json(PegPoll *poll, const Game *game) {
  if (!poll) {
    return string_duplicate("null");
  }
  PegPollSnapshot snapshot = {0};
  peg_poll_read(poll, &snapshot);
  return peg_snapshot_get_json(&snapshot, game);
}

char *peg_poll_outcomes_json(PegPoll *poll, const Board *board,
                             const LetterDistribution *ld,
                             const char *move_text) {
  PegCandOutcomes *outcomes = NULL;
  int count = 0;
  peg_poll_copy_outcomes(poll, &outcomes, &count);
  StringBuilder *json = string_builder_create();
  string_builder_add_string(json, "null");
  for (int index = 0; index < count; index++) {
    const PegCandOutcomes *one = &outcomes[index];
    StringBuilder *name = string_builder_create();
    string_builder_add_move(name, board, &one->move, ld, false);
    const bool matches = strings_equal(string_builder_peek(name), move_text);
    string_builder_destroy(name);
    if (!matches) {
      continue;
    }
    string_builder_clear(json);
    string_builder_add_formatted_string(json, "{\"depth\":%d,\"rows\":[",
                                        one->fidelity);
    for (int row_idx = 0; row_idx < one->n_rows; row_idx++) {
      const PegPerScenario *row = &one->rows[row_idx];
      string_builder_add_string(json, row_idx ? ",{\"drawn\":" : "{\"drawn\":");
      string_builder_add_json_string(json, row->drawn);
      string_builder_add_string(json, ",\"remaining\":");
      string_builder_add_json_string(json, row->remaining);
      string_builder_add_formatted_string(
          json, ",\"weight\":%" PRId64 ",\"margin\":%d,\"pv\":", row->weight,
          row->mover_total);
      string_builder_add_json_string(json, row->pv ? row->pv : "");
      string_builder_add_char(json, '}');
    }
    string_builder_add_string(json, "]}");
    break;
  }
  peg_cand_outcomes_destroy_array(outcomes, count);
  return string_builder_dump_and_destroy(json, NULL);
}
