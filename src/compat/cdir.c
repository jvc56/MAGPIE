#include "cdir.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
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

#if defined(_WIN32)
static wchar_t *cdir_wide(const char *text) {
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  wchar_t *wide = length > 0 ? malloc(sizeof(wchar_t) * (size_t)length) : NULL;
  if (wide) {
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                              length);
  }
  return wide;
}

static char *cdir_narrow(const wchar_t *wide) {
  const int length =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  char *narrow = length > 0 ? malloc((size_t)length) : NULL;
  if (narrow) {
    (void)WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, length, NULL,
                              NULL);
  }
  return narrow;
}
#endif

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
  wchar_t *wide = cdir_wide(pattern);
  free(pattern);
  if (!wide) {
    free(dir);
    errno = EINVAL;
    return NULL;
  }
  dir->find = FindFirstFileW(wide, &dir->entry);
  free(wide);
  if (dir->find == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    free(dir);
    errno = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? ENOENT
            : error == ERROR_DIRECTORY ? ENOTDIR
                                       : EACCES;
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
    dir->name = cdir_narrow(dir->entry.cFileName);
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
