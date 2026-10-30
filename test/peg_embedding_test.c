#include "peg_embedding_test.h"

#include "../src/compat/ctime.h"
#include "../src/impl/cmd_api.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// shpeg's optional argument: how many of the ranking's first rows to show,
// with the solved plays ranked below them pinned after a "..." row, or "all"
// for every play. g104331 (1 in the bag) has many solved plays below its cut
// ones.
static void test_peg_show_rows(void) {
  Magpie *mp = magpie_create(DEFAULT_TEST_DATA_PATH);
  assert(!magpie_has_error(mp));
  assert(magpie_run_sync(mp, "set -lex CSW24 -threads 4 -pegoutcomes false") ==
         MAGPIE_SUCCESS);
  assert(magpie_run_sync(
             mp, "cgp 4CYANiDS4/10PE3/8VLOGS2/10UM3/10TA3/2Q3HOTTY4/2IF1WAB7/"
                 "L2AROHA6M/E1OXEN8E/VIDIOTS7I/3N2E2LOIPEN/3G1TITmOUSE1I/"
                 "6Z7E/6E2WICE2/3LADRONE2RAJ BEFIKNR/ 322/359 0 -lex "
                 "CSW24") == MAGPIE_SUCCESS);
  assert(magpie_run_sync(mp, "peg -pegplies 1") == MAGPIE_SUCCESS);

  assert(magpie_run_sync(mp, "shpeg") == MAGPIE_SUCCESS);
  char *output = magpie_get_last_command_output(mp);
  assert(has_prefix("Status          ", output));
  assert(has_substring(output, " (finished)\n  Seed          "));
  assert(has_substring(output, "\nBest Move       14A BRIEF  25.0%"));
  assert(has_substring(output, "\n  10  "));
  assert(has_substring(output, "\n ...  "));
  assert(has_substring(output, " more, including "));
  free(output);

  assert(magpie_run_sync(mp, "shpeg 3") == MAGPIE_SUCCESS);
  output = magpie_get_last_command_output(mp);
  assert(has_substring(output, "\n   3  "));
  assert(!has_substring(output, "\n   4  "));
  assert(has_substring(output, "\n ...  "));
  free(output);

  assert(magpie_run_sync(mp, "shpeg all") == MAGPIE_SUCCESS);
  output = magpie_get_last_command_output(mp);
  assert(!has_substring(output, "\n ...  "));
  assert(!has_substring(output, " more"));
  free(output);

  assert(magpie_run_sync(mp, "shpeg 0") != MAGPIE_SUCCESS);
  char *error = magpie_get_and_clear_error(mp);
  free(error);
  assert(magpie_run_sync(mp, "shpeg some") != MAGPIE_SUCCESS);
  error = magpie_get_and_clear_error(mp);
  free(error);
  magpie_destroy(mp);
}

void test_peg_embedding(void) {
  Magpie *mp = magpie_create(DEFAULT_TEST_DATA_PATH);
  assert(!magpie_has_error(mp));
  assert(magpie_run_sync(mp, "set -lex CSW24 -threads 1 -pegtlim 0.1") ==
         MAGPIE_SUCCESS);
  assert(
      magpie_run_sync(
          mp,
          "cgp "
          "4WILI3JANE/4A3AQUAE2/2C1N3K2Y3/2RUG3E2G3/2O1L3B1REV2/2U1EL2I1EEL2/"
          "2T1SI2A1S1Y2/R1O2PWNS1T4/E1N2I4A4/AH1MED4T4/GI1OPE4E4/IT1ZO2VOUDON2/"
          "N3D10/"
          "I3E10/COIFS10 BDFHMX?/AAORTT? 420/412 0 -lex CSW24") ==
      MAGPIE_SUCCESS);
  // The command-line protocol stays text-only until an embedder opts in.
  for (int mode_idx = 0; mode_idx < 3; mode_idx++) {
    if (mode_idx > 0) {
      assert(magpie_run_sync(mp, mode_idx == 1
                                     ? "set -pegjson true"
                                     : "set -pegjson false") == MAGPIE_SUCCESS);
    }
    assert(magpie_run_sync(mp, "peg") == MAGPIE_SUCCESS);
    char *output = magpie_get_last_command_output(mp);
    assert(has_substring(output, "peginfo {") == (mode_idx == 1));
    free(output);
    assert(magpie_run_sync(mp, "shpeg") == MAGPIE_SUCCESS);
    output = magpie_get_last_command_output(mp);
    assert(has_substring(output, "peginfo {") == (mode_idx == 1));
    free(output);
  }
  assert(magpie_run_sync(mp, "set -pegjson false") == MAGPIE_SUCCESS);
  assert(magpie_run_sync(mp, "shpeg") == MAGPIE_SUCCESS);
  char *text = magpie_get_last_command_output(mp);
  assert(magpie_run_sync(mp, "set -pegjson true") == MAGPIE_SUCCESS);
  assert(magpie_run_sync(mp, "shpeg") == MAGPIE_SUCCESS);
  char *structured = magpie_get_last_command_output(mp);
  const char *table = strchr(structured, '\n');
  assert(table && strings_equal(table + 1, text));
  free(text);
  free(structured);
  magpie_destroy(mp);
  test_peg_show_rows();
}

// The g104730 position: 2 in the bag, every candidate solved at 3 plies, the
// best (11C YITES) winning 26.4%. It passes through every phase status shows:
// the greedy seed, the best seed candidate solved alone, then the rest at
// once, one per thread.
#define PEG_STATUS_OUTPUT_CGP                                                  \
  "cgp 13R1/12MA1/12OB1/11OHIA/11FACE/9Q1LI1R/2VUG1SIDECAR1A/3PALEO1ERG2D/"    \
  "4BOX2MI3I/6TENANT1ZO/10I2L1/7FOUND1O1/8UNGIRT1/2NyLONED4Y1/"                \
  "AJEE1KAT3WEST EEHISTY/ 364/387 0 -lex CSW24"

enum {
  PEG_STATUS_DECILES = 10,
  PEG_STATUS_PHASE_MAX = 128,
};

static const double peg_status_poll_seconds = 0.02;

// Where a running status is: its phase, which names the running stage and
// how many candidates are worked on at once (falling as the last ones
// finish), the tenth of the stage's candidates finished, and whether the
// candidate solved alone lists its draws yet. Snapshots are written when it
// changes.
typedef struct PegStatusMilestone {
  char phase[PEG_STATUS_PHASE_MAX];
  int decile;
  bool lists_draws;
} PegStatusMilestone;

// The rest of the summary line labelled `label`, up to its end, into value.
static void peg_status_summary_value(const char *status, const char *label,
                                     char *value, size_t value_size) {
  value[0] = '\0';
  const char *line = strstr(status, label);
  if (line == NULL) {
    return;
  }
  line += strlen(label);
  while (*line == ' ') {
    line++;
  }
  size_t length = 0;
  while (line[length] != '\0' && line[length] != '\n' &&
         length + 1 < value_size) {
    length++;
  }
  memcpy(value, line, length);
  value[length] = '\0';
}

// Whether `status` is a running solve's: its first line is the status line,
// ending "(running)".
static bool peg_status_is_running(const char *status) {
  const char *first_end = strchr(status, '\n');
  const char *running = strstr(status, " (running)\n");
  return has_prefix("Status          ", status) && first_end != NULL &&
         running != NULL && running < first_end;
}

static PegStatusMilestone peg_status_milestone(const char *status) {
  PegStatusMilestone milestone;
  // The running stage's line under the status line, as
  // "  3-ply         1.2s (running)".
  char stage_label[PEG_STATUS_PHASE_MAX] = "";
  const char *stage_lines = strchr(status, '\n');
  const char *running =
      stage_lines != NULL ? strstr(stage_lines, " (running)") : NULL;
  if (running != NULL) {
    const char *line = running;
    while (line > status && line[-1] != '\n') {
      line--;
    }
    (void)sscanf(line, "%127s", stage_label);
  }
  int n_at_once = 0;
  const char *working = strstr(status, "\nCurrently working on ");
  if (working != NULL) {
    (void)sscanf(working, "\nCurrently working on %d", &n_at_once);
  }
  (void)snprintf(milestone.phase, sizeof(milestone.phase), "%s %d %d",
                 stage_label, n_at_once,
                 has_substring(status, "\nSolving ") ? 1 : 0);
  char progress[PEG_STATUS_PHASE_MAX];
  peg_status_summary_value(status, "\nProgress ", progress, sizeof(progress));
  int cands_done = 0;
  int field_size = 0;
  milestone.decile = PEG_STATUS_DECILES;
  milestone.lists_draws = has_substring(status, "\nDraw ");
  if (sscanf(progress, "%d of %d", &cands_done, &field_size) == 2 &&
      field_size > 0) {
    milestone.decile = cands_done * PEG_STATUS_DECILES / field_size;
  }
  return milestone;
}

static bool peg_status_milestones_equal(const PegStatusMilestone *milestone,
                                        const PegStatusMilestone *other) {
  return strings_equal(milestone->phase, other->phase) &&
         milestone->decile == other->decile &&
         milestone->lists_draws == other->lists_draws;
}

// Writes peg_output.txt: what status shows at each stage of a running PEG,
// then what shpeg shows when it is done.
void test_peg_status_output(void) {
  Magpie *mp = magpie_create(DEFAULT_TEST_DATA_PATH);
  assert(!magpie_has_error(mp));
  assert(magpie_run_sync(mp, "set -lex CSW24 -threads 8") == MAGPIE_SUCCESS);
  assert(magpie_run_sync(mp, PEG_STATUS_OUTPUT_CGP) == MAGPIE_SUCCESS);
  FILE *output_file = fopen("peg_output.txt", "w");
  assert(output_file != NULL);
  const int64_t start_ns = ctimer_monotonic_ns();
  assert(magpie_run_async_human_readable(mp, "peg") == MAGPIE_SUCCESS);
  PegStatusMilestone last_milestone = {
      .phase = "", .decile = -1, .lists_draws = false};
  int num_snapshots = 0;
  while (magpie_get_thread_status(mp) != MAGPIE_THREAD_STATUS_FINISHED) {
    char *status = magpie_get_last_command_status_message(mp);
    if (peg_status_is_running(status)) {
      const PegStatusMilestone milestone = peg_status_milestone(status);
      if (!peg_status_milestones_equal(&milestone, &last_milestone)) {
        last_milestone = milestone;
        num_snapshots++;
        (void)fprintf(output_file, "==== status at %.1fs ====\n%s\n",
                      (double)(ctimer_monotonic_ns() - start_ns) / 1e9, status);
      }
    }
    free(status);
    ctime_nap(peg_status_poll_seconds);
  }
  assert(magpie_await(mp) == MAGPIE_SUCCESS);
  assert(num_snapshots > 0);
  assert(magpie_run_sync_human_readable(mp, "shpeg") == MAGPIE_SUCCESS);
  char *shpeg = magpie_get_last_command_output(mp);
  assert(has_substring(shpeg, "11C YITES"));
  (void)fprintf(output_file, "==== shpeg at %.1fs ====\n%s",
                (double)(ctimer_monotonic_ns() - start_ns) / 1e9, shpeg);
  free(shpeg);
  assert(fclose(output_file) == 0);
  magpie_destroy(mp);
}
