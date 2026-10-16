#ifndef DATA_FETCH_H
#define DATA_FETCH_H

#include "../util/io_util.h"
#include <stdbool.h>

// `data fetch`: downloads MAGPIE's data into a data directory with nothing but
// MAGPIE itself (no bash, curl or tar, which a Windows user does not have).
// See docs/windows-plan.md, section 6.2.
//
// It fetches the same versioned tarball download_data.sh does, from
// MAGPIE-DATA, in the same chunks, checks each chunk against the size and
// SHA-256 pinned in data_fetch.c, and unpacks it. Each file is written in one
// step, so an interrupted fetch leaves no partial file. The tarball's symlinks
// are written as copies of what they point to: Windows cannot make links
// without privilege, and no loader depends on a file being a link.

// The data version this MAGPIE goes with: download_data.sh's DATA_VERSION,
// which CI checks it matches.
#define DATA_FETCH_VERSION "20260925"

// Where the chunks are published, each at <base url>/data-<version>.tgz.<aa..>.
#define DATA_FETCH_DEFAULT_BASE_URL                                            \
  "https://raw.githubusercontent.com/jvc56/MAGPIE-DATA/main/"                  \
  "versioned-tarballs"

// The data version a fetch records in a data directory, to say it is complete.
#define DATA_FETCH_VERSION_FILENAME ".data-version"

// Called with a line of progress to show the user.
typedef void (*data_fetch_report_t)(void *context, const char *message);

// Fetches data version DATA_FETCH_VERSION from `base_url` (NULL for
// DATA_FETCH_DEFAULT_BASE_URL) into `data_dir`, creating it as needed, unless
// `data_dir` already records that version. Pushes onto `error_stack` and stops
// at the first failure. `report` may be NULL.
void data_fetch(const char *base_url, const char *data_dir,
                data_fetch_report_t report, void *report_context,
                ErrorStack *error_stack);

// The second half of data_fetch, for a data tarball already on disk: unpacks
// the .tgz at `archive_path` into `data_dir`, writing its symlinks as copies.
// The archive is not checked against the pinned SHA-256s.
void data_fetch_unpack_archive(const char *archive_path, const char *data_dir,
                               ErrorStack *error_stack);

// Whether `data_dir` holds MAGPIE's data, judged by the board layout every
// game needs: what tells a first run with no data apart from a typo.
bool data_fetch_is_data_present(const char *data_dir);

#endif
