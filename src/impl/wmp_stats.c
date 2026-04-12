#include "wmp_stats.h" // IWYU pragma: keep

#ifdef MAGPIE_WMP_STATS

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
_Atomic uint64_t
    g_wmp_stats[WMP_STATS_NUM_CATEGORIES][WMP_STATS_MAX_BUCKETS];

static const char *const category_names[WMP_STATS_NUM_CATEGORIES] = {
    "NP_CHECK_TOTAL",
    "NP_CHECK_FOUND",
    "PT_FULL_RACK_TOTAL",
    "PT_FULL_RACK_FOUND",
    "PT_SUBRACK_WORDS_TOTAL",
    "PT_SUBRACK_WORDS_FOUND",
    "WORDS_WRITTEN_NP_CALLS",
    "WORDS_WRITTEN_NP_SUM",
    "WORDS_WRITTEN_PT_CALLS",
    "WORDS_WRITTEN_PT_SUM",
    "SHADOW_RECORD_CALLS",
    "SHADOW_FAST_PATH_TAKEN",
    "SHADOW_FAST_PATH_BYPASS_NO_RIT_ENTRY",
    "SHADOW_FAST_PATH_BYPASS_MULTI_PLAYTHROUGH",
    "SHADOW_FAST_PATH_BYPASS_NO_COVERAGE",
    "SHADOW_FAST_PATH_PRUNED",
    "SHADOW_FALLBACK_FULL_RACK",
    "SHADOW_MULTI_PT_BITVEC_PRUNED",
    "ANCHORS_EXTRACTED",
    "ANCHORS_PRUNED",
    "CHECK_PT_AND_CROSSES_TOTAL",
    "CHECK_PT_AND_CROSSES_PASSED",
};

static uint64_t load_counter(int category, int bucket) {
  return atomic_load_explicit(&g_wmp_stats[category][bucket],
                              memory_order_relaxed);
}

static uint64_t row_total(int category) {
  uint64_t total = 0;
  for (int bucket = 0; bucket < WMP_STATS_MAX_BUCKETS; bucket++) {
    total += load_counter(category, bucket);
  }
  return total;
}

void wmp_stats_print(void) {
  uint64_t grand_total_wmp_walks = 0;
  grand_total_wmp_walks += row_total(WMP_STATS_NP_CHECK_TOTAL);
  grand_total_wmp_walks += row_total(WMP_STATS_PT_FULL_RACK_TOTAL);
  grand_total_wmp_walks += row_total(WMP_STATS_PT_SUBRACK_WORDS_TOTAL);

  fprintf(stderr, "\n=== MAGPIE WMP stats ===\n");
  fprintf(
      stderr,
      "Total wmp_get_word_entry calls across all movegen sites: %llu\n",
      (unsigned long long)grand_total_wmp_walks);
  fprintf(stderr, "  NP_CHECK               (gen_load):   %llu\n",
          (unsigned long long)row_total(WMP_STATS_NP_CHECK_TOTAL));
  fprintf(stderr, "  PT_FULL_RACK           (shadow):     %llu\n",
          (unsigned long long)row_total(WMP_STATS_PT_FULL_RACK_TOTAL));
  fprintf(stderr, "  PT_SUBRACK_WORDS       (record):     %llu\n",
          (unsigned long long)row_total(WMP_STATS_PT_SUBRACK_WORDS_TOTAL));
  fprintf(stderr, "\n");

  fprintf(stderr, "Shadow fast-path decisions (playthrough branch):\n");
  const uint64_t shadow_total = row_total(WMP_STATS_SHADOW_RECORD_CALLS);
  const uint64_t fp_taken = row_total(WMP_STATS_SHADOW_FAST_PATH_TAKEN);
  const uint64_t fp_pruned = row_total(WMP_STATS_SHADOW_FAST_PATH_PRUNED);
  const uint64_t fp_no_entry =
      row_total(WMP_STATS_SHADOW_FAST_PATH_BYPASS_NO_RIT_ENTRY);
  const uint64_t fp_multi =
      row_total(WMP_STATS_SHADOW_FAST_PATH_BYPASS_MULTI_PLAYTHROUGH);
  const uint64_t fp_no_cov =
      row_total(WMP_STATS_SHADOW_FAST_PATH_BYPASS_NO_COVERAGE);
  const uint64_t fallback_full_rack =
      row_total(WMP_STATS_SHADOW_FALLBACK_FULL_RACK);
  const uint64_t multi_pt_bitvec_pruned =
      row_total(WMP_STATS_SHADOW_MULTI_PT_BITVEC_PRUNED);
  fprintf(stderr, "  total shadow_record calls:          %llu\n",
          (unsigned long long)shadow_total);
  fprintf(stderr,
          "  fast_path_taken:                    %llu (of which %llu pruned)\n",
          (unsigned long long)fp_taken, (unsigned long long)fp_pruned);
  fprintf(stderr, "  bypass_no_rit_entry:                %llu\n",
          (unsigned long long)fp_no_entry);
  fprintf(stderr, "  bypass_multi_playthrough:           %llu\n",
          (unsigned long long)fp_multi);
  fprintf(stderr, "  bypass_no_coverage:                 %llu\n",
          (unsigned long long)fp_no_cov);
  fprintf(stderr, "  fallback_full_rack_wmp_call:        %llu\n",
          (unsigned long long)fallback_full_rack);
  fprintf(stderr, "  multi_pt_tp7_bitvec_pruned:         %llu\n",
          (unsigned long long)multi_pt_bitvec_pruned);
  fprintf(stderr, "\n");

  fprintf(stderr, "Per-category, per-bucket counts (row totals on right):\n");
  fprintf(stderr,
          "%-42s", "category\\bucket");
  for (int bucket = 0; bucket < WMP_STATS_MAX_BUCKETS; bucket++) {
    bool any = false;
    for (int cat = 0; cat < WMP_STATS_NUM_CATEGORIES; cat++) {
      if (load_counter(cat, bucket) != 0) {
        any = true;
        break;
      }
    }
    if (any) {
      fprintf(stderr, " %10d", bucket);
    }
  }
  fprintf(stderr, " %15s\n", "TOTAL");

  for (int cat = 0; cat < WMP_STATS_NUM_CATEGORIES; cat++) {
    uint64_t total = row_total(cat);
    if (total == 0) {
      continue;
    }
    fprintf(stderr, "%-42s", category_names[cat]);
    for (int bucket = 0; bucket < WMP_STATS_MAX_BUCKETS; bucket++) {
      bool any = false;
      for (int cat2 = 0; cat2 < WMP_STATS_NUM_CATEGORIES; cat2++) {
        if (load_counter(cat2, bucket) != 0) {
          any = true;
          break;
        }
      }
      if (!any) {
        continue;
      }
      const uint64_t val = load_counter(cat, bucket);
      if (val == 0) {
        fprintf(stderr, " %10s", ".");
      } else {
        fprintf(stderr, " %10llu", (unsigned long long)val);
      }
    }
    fprintf(stderr, " %15llu\n", (unsigned long long)total);
  }
  fprintf(stderr, "\n");

  fprintf(stderr, "Hit rates (FOUND / TOTAL):\n");
  const uint64_t np_t = row_total(WMP_STATS_NP_CHECK_TOTAL);
  const uint64_t np_f = row_total(WMP_STATS_NP_CHECK_FOUND);
  if (np_t > 0) {
    fprintf(stderr, "  NP_CHECK:       %llu/%llu = %.1f%%\n",
            (unsigned long long)np_f, (unsigned long long)np_t,
            100.0 * (double)np_f / (double)np_t);
  }
  const uint64_t ptfr_t = row_total(WMP_STATS_PT_FULL_RACK_TOTAL);
  const uint64_t ptfr_f = row_total(WMP_STATS_PT_FULL_RACK_FOUND);
  if (ptfr_t > 0) {
    fprintf(stderr, "  PT_FULL_RACK:   %llu/%llu = %.1f%%\n",
            (unsigned long long)ptfr_f, (unsigned long long)ptfr_t,
            100.0 * (double)ptfr_f / (double)ptfr_t);
  }
  const uint64_t ptsw_t = row_total(WMP_STATS_PT_SUBRACK_WORDS_TOTAL);
  const uint64_t ptsw_f = row_total(WMP_STATS_PT_SUBRACK_WORDS_FOUND);
  if (ptsw_t > 0) {
    fprintf(stderr, "  PT_SUBRACK:     %llu/%llu = %.1f%%\n",
            (unsigned long long)ptsw_f, (unsigned long long)ptsw_t,
            100.0 * (double)ptsw_f / (double)ptsw_t);
  }
  fprintf(stderr, "\n");

  const uint64_t anchors_extracted = row_total(WMP_STATS_ANCHORS_EXTRACTED);
  const uint64_t anchors_pruned = row_total(WMP_STATS_ANCHORS_PRUNED);
  fprintf(stderr, "Anchor heap:\n");
  fprintf(stderr, "  extracted:                          %llu\n",
          (unsigned long long)anchors_extracted);
  fprintf(stderr, "  pruned (better_play_has_been_found): %llu\n",
          (unsigned long long)anchors_pruned);
  if (anchors_extracted > 0) {
    fprintf(stderr, "  processed:                          %llu (%.1f%% of extracted)\n",
            (unsigned long long)(anchors_extracted - anchors_pruned),
            100.0 * (double)(anchors_extracted - anchors_pruned) /
                (double)anchors_extracted);
  }
  fprintf(stderr, "\n");

  const uint64_t crosses_total =
      row_total(WMP_STATS_CHECK_PT_AND_CROSSES_TOTAL);
  const uint64_t crosses_passed =
      row_total(WMP_STATS_CHECK_PT_AND_CROSSES_PASSED);
  fprintf(stderr, "check_playthrough_and_crosses:\n");
  fprintf(stderr, "  total:                              %llu\n",
          (unsigned long long)crosses_total);
  fprintf(stderr, "  passed:                             %llu\n",
          (unsigned long long)crosses_passed);
  if (crosses_total > 0) {
    fprintf(stderr, "  pass rate:                          %.1f%%\n",
            100.0 * (double)crosses_passed / (double)crosses_total);
  }
  fprintf(stderr, "\n");

  fprintf(stderr, "Words materialized (write_words_to_buffer):\n");
  const uint64_t np_calls = row_total(WMP_STATS_WORDS_WRITTEN_NP_CALLS);
  const uint64_t np_sum = row_total(WMP_STATS_WORDS_WRITTEN_NP_SUM);
  const uint64_t pt_calls = row_total(WMP_STATS_WORDS_WRITTEN_PT_CALLS);
  const uint64_t pt_sum = row_total(WMP_STATS_WORDS_WRITTEN_PT_SUM);
  if (np_calls > 0) {
    fprintf(stderr,
            "  NP: %llu calls, %llu words (avg %.2f words/call)\n",
            (unsigned long long)np_calls, (unsigned long long)np_sum,
            (double)np_sum / (double)np_calls);
  }
  if (pt_calls > 0) {
    fprintf(stderr,
            "  PT: %llu calls, %llu words (avg %.2f words/call)\n",
            (unsigned long long)pt_calls, (unsigned long long)pt_sum,
            (double)pt_sum / (double)pt_calls);
  }
  fprintf(stderr, "=== end MAGPIE WMP stats ===\n");
  fflush(stderr);
}

void wmp_stats_init(void) {
  memset(g_wmp_stats, 0, sizeof(g_wmp_stats));
  atexit(wmp_stats_print);
}

#endif
