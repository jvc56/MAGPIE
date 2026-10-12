#ifndef ATOMIC_FILE_H
#define ATOMIC_FILE_H

#include "../util/io_util.h"
#include "../util/string_util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

// The caller owns both returned paths, including on failure. Resolve existing
// symlinks before selecting a sibling temporary file so rename preserves them.
static inline int atomic_file_create(const char *filename, char **destination,
                                     char **temporary) {
  *destination = NULL;
  *temporary = NULL;
  struct stat metadata;
  *destination = string_duplicate(filename);
  int found;
  for (int links = 0;; links++) {
    found = lstat(*destination, &metadata);
    if (found != 0) {
      if (errno != ENOENT) {
        return -1;
      }
      break;
    }
    if (!S_ISLNK(metadata.st_mode)) {
      break;
    }
    if (links == 40) {
      errno = ELOOP;
      return -1;
    }
    char target[PATH_MAX + 1];
    const ssize_t length = readlink(*destination, target, PATH_MAX);
    if (length < 0) {
      return -1;
    }
    if (length == PATH_MAX) {
      errno = ENAMETOOLONG;
      return -1;
    }
    target[length] = '\0';
    const char *slash = strrchr(*destination, '/');
    char *resolved = target[0] == '/' || !slash
                         ? string_duplicate(target)
                         : get_formatted_string("%.*s/%s",
                                                (int)(slash - *destination),
                                                *destination, target);
    free(*destination);
    *destination = resolved;
  }
  *temporary = get_formatted_string("%s.tmp.XXXXXX", *destination);
  int descriptor = mkstemp(*temporary);
  if (descriptor < 0) {
    return -1;
  }
  if (found == 0) {
    if (fchmod(descriptor, metadata.st_mode & 0777) != 0) {
      const int saved_errno = errno;
      (void)close(descriptor);
      (void)unlink(*temporary);
      errno = saved_errno;
      return -1;
    }
  } else {
    // Let open apply the current umask without temporarily changing this
    // process-wide setting while other threads might be creating files.
    (void)close(descriptor);
    if (unlink(*temporary) != 0) {
      return -1;
    }
    descriptor = open(*temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    // O_EXCL rejects an intervening creator; never unlink their file.
  }
  return descriptor;
}

#endif
