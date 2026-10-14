#ifndef DATA_FETCH_H
#define DATA_FETCH_H

#include "../util/io_util.h"
#include <stdbool.h>

// `data fetch`: downloads MAGPIE's data files, one at a time, into a data
// directory, with nothing but MAGPIE itself (no bash, curl or tar, which a
// Windows user does not have). See docs/windows-plan.md, section 6.2.
//
// What to fetch is a JSON manifest, published by MAGPIE-DATA beside the files
// (scripts/data_manifest.py writes one):
//
//   {"files": [
//     {"path": "lexica/CSW24.kwg", "url": "https://...", "bytes": 4813452,
//      "sha256": "<64 hex digits>"},
//     {"path": "lexica/CSW24_super21.klv2",
//      "copy_of": "lexica/CSW21_super21.klv2"}]}
//
// Paths are relative to the data directory and '/'-separated, and may not
// leave it. A "copy_of" entry stands for what is a symlink in the data
// tarballs: the file is written as a copy of the one it names, which must be
// listed too, so no loader depends on a link (Windows cannot make one without
// privilege). A file already present with the listed SHA-256 is kept; any
// other is downloaded, checked against its size and SHA-256, and written in
// one step, so an interrupted fetch never leaves a partial file.

// The manifest of the data set this MAGPIE goes with.
#define DATA_FETCH_DEFAULT_MANIFEST_URL                                        \
  "https://raw.githubusercontent.com/jvc56/MAGPIE-DATA/main/manifests/"        \
  "data-20260925.json"

// Called with a line of progress to show the user.
typedef void (*data_fetch_report_t)(void *context, const char *message);

// Fetches what the manifest at `manifest_url` lists into `data_dir`,
// creating it and its subdirectories as needed. Pushes onto `error_stack` and
// stops at the first failure. `report` may be NULL.
void data_fetch(const char *manifest_url, const char *data_dir,
                data_fetch_report_t report, void *report_context,
                ErrorStack *error_stack);

// Whether `data_dir` holds MAGPIE's data, judged by the board layouts every
// game needs: what tells a first run with no data apart from a typo.
bool data_fetch_is_data_present(const char *data_dir);

#endif
