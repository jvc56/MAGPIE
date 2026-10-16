#ifndef DATA_FETCH_H
#define DATA_FETCH_H

#include "../util/io_util.h"
#include <stdbool.h>

// `data fetch`: downloads MAGPIE's data into a data directory with nothing but
// MAGPIE itself (no bash, curl or tar, which a Windows user does not have).
//
// It fetches the same versioned tarball download_data.sh does, from
// MAGPIE-DATA, in the same chunks, checks each chunk against the size and
// SHA-256 pinned in data_fetch.c, and unpacks it into the directory. Each
// file is written in one step, so an interrupted fetch leaves no partial
// file, and the directory records that it is incomplete until the last one
// is out. The tarball's symlinks are written as copies of what they point to:
// Windows cannot make links without privilege, and no loader depends on a
// file being a link.

// The data version this MAGPIE goes with: download_data.sh's DATA_VERSION,
// which CI checks it matches.
#define DATA_FETCH_VERSION "20260925"

// Where the chunks are published, each at <base url>/data-<version>.tgz.<aa..>.
#define DATA_FETCH_DEFAULT_BASE_URL                                            \
  "https://raw.githubusercontent.com/jvc56/MAGPIE-DATA/main/"                  \
  "versioned-tarballs"

// Where a fetch records, in the data directory, the data version it unpacked
// there (or that it is still unpacking).
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

// What a data directory holds, as far as starting MAGPIE goes.
typedef enum {
  // No data: no board layout, which every game needs. What tells a first run
  // apart from a typo.
  DATA_FETCH_STATE_MISSING,
  // A fetch started unpacking here and did not finish, so the files are a
  // mix of versions, or some are missing.
  DATA_FETCH_STATE_INCOMPLETE,
  // Data fetched for another MAGPIE, of another data version.
  DATA_FETCH_STATE_OUTDATED,
  // Data put there some other way (download_data.sh, by hand), which records
  // no version.
  DATA_FETCH_STATE_UNVERSIONED,
  // Data version DATA_FETCH_VERSION, fetched.
  DATA_FETCH_STATE_CURRENT,
} data_fetch_state_t;

data_fetch_state_t data_fetch_get_state(const char *data_dir);

#endif
