#include "cdir.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include "cwin.h"
#include <wchar.h>
#include <windows.h>
#else
#include <dirent.h>
#endif

struct CDir {
#if defined(_WIN32)
  HANDLE find;
  WIN32_FIND_DATAW entry;
  // FindFirstFileW returns the first entry with the handle, so it is
  // waiting to be read before FindNextFileW is called.
  bool entry_pending;
  char *name;
#else
  DIR *dir;
#endif
};

static bool cdir_is_dot_entry(const char *name) {
  return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

CDir *cdir_open(const char *path) {
  if (!path) {
    errno = EINVAL;
    return NULL;
  }
  CDir *dir = calloc(1, sizeof(CDir));
  if (!dir) {
    errno = ENOMEM;
    return NULL;
  }
#if defined(_WIN32)
  const size_t length = strlen(path);
  char *pattern = malloc(length + 3);
  if (!pattern) {
    free(dir);
    errno = ENOMEM;
    return NULL;
  }
  memcpy(pattern, path, length);
  size_t pattern_length = length;
  if (length > 0 && path[length - 1] != '/' && path[length - 1] != '\\') {
    pattern[pattern_length++] = '\\';
  }
  pattern[pattern_length++] = '*';
  pattern[pattern_length] = '\0';
  wchar_t *wide = cwin_wide(pattern);
  free(pattern);
  if (!wide) {
    free(dir);
    return NULL;
  }
  dir->find = FindFirstFileW(wide, &dir->entry);
  free(wide);
  if (dir->find == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    free(dir);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      errno = ENOENT;
    } else if (error == ERROR_DIRECTORY) {
      errno = ENOTDIR;
    } else {
      errno = EACCES;
    }
    return NULL;
  }
  dir->entry_pending = true;
#else
  dir->dir = opendir(path);
  if (!dir->dir) {
    const int error_number = errno;
    free(dir);
    errno = error_number;
    return NULL;
  }
#endif
  return dir;
}

const char *cdir_next(CDir *dir) {
  if (!dir) {
    return NULL;
  }
#if defined(_WIN32)
  while (true) {
    if (!dir->entry_pending && !FindNextFileW(dir->find, &dir->entry)) {
      return NULL;
    }
    dir->entry_pending = false;
    free(dir->name);
    dir->name = cwin_narrow(dir->entry.cFileName);
    if (dir->name && !cdir_is_dot_entry(dir->name)) {
      return dir->name;
    }
  }
#else
  const struct dirent *entry;
  while ((entry = readdir(dir->dir)) != NULL) {
    if (!cdir_is_dot_entry(entry->d_name)) {
      return entry->d_name;
    }
  }
  return NULL;
#endif
}

void cdir_close(CDir *dir) {
  if (!dir) {
    return;
  }
#if defined(_WIN32)
  (void)FindClose(dir->find);
  free(dir->name);
#else
  (void)closedir(dir->dir);
#endif
  free(dir);
}
