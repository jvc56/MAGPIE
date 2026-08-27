#ifndef COMPAT_CFILE_H
#define COMPAT_CFILE_H

// File permission checks for credential files. The contribution settings file
// holds a bearer API key, so it must not be readable by other users.

#include <stdbool.h>

#include "../util/io_util.h"
#include "../util/string_util.h"

#if defined(_WIN32) || defined(__wasm__)

// Windows inherits directory ACLs and wasm has no meaningful filesystem
// ownership, so there is nothing to check or tighten on either.
static inline bool cfile_is_world_readable(const char *path) {
  (void)path;
  return false;
}

static inline void cfile_restrict_to_owner(const char *path,
                                           ErrorStack *error_stack) {
  (void)path;
  (void)error_stack;
}

#else

#include <sys/stat.h>

static inline bool cfile_is_world_readable(const char *path) {
  struct stat info;
  if (stat(path, &info) != 0) {
    return false;
  }
  return (info.st_mode & (S_IRGRP | S_IROTH)) != 0;
}

static inline void cfile_restrict_to_owner(const char *path,
                                           ErrorStack *error_stack) {
  if (chmod(path, S_IRUSR | S_IWUSR) != 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_FILE_PERMISSIONS_FAILED,
        get_formatted_string("could not restrict permissions on %s", path));
  }
}

#endif

#endif
