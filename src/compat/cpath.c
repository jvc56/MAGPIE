#include "cpath.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include "cwin.h"
#include <ctype.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

enum {
  // The longest executable path asked for before giving up.
  CPATH_EXECUTABLE_PATH_MAX = 1 << 16,
};

static char *cpath_copy(const char *start, size_t length) {
  char *copy = malloc(length + 1);
  if (copy) {
    memcpy(copy, start, length);
    copy[length] = '\0';
  }
  return copy;
}

// Whether the character at list[index] ends the path that starts at
// list[start].
static bool cpath_ends_item(const char *list, size_t start, size_t index) {
  const char separator = list[index];
#if defined(_WIN32)
  if (separator == ';') {
    return true;
  }
  if (separator != ':') {
    return false;
  }
  // A drive letter's colon: one letter, then ':', at the start of a path, or
  // after the \\?\ or \\.\ that lets a path past MAX_PATH ("\\?\C:\data").
  // So "d:data" is the drive's directory, never "d" and "data": a list
  // separated by ':' cannot start with a one-letter directory here.
  const size_t letter = index - 1;
  if (index == start || !isalpha((unsigned char)list[letter])) {
    return true;
  }
  if (letter == start) {
    return false;
  }
  const char *prefix = list + start;
  const bool device_prefix =
      letter == start + 4 && cpath_is_separator(prefix[0]) &&
      cpath_is_separator(prefix[1]) && (prefix[2] == '?' || prefix[2] == '.') &&
      cpath_is_separator(prefix[3]);
  return !device_prefix;
#else
  (void)start;
  return separator == ':';
#endif
}

int cpath_split_list(const char *list, char ***paths) {
  *paths = NULL;
  if (!list) {
    return 0;
  }
  // At most one path more than there are separator characters.
  int capacity = 1;
  for (const char *character = list; *character; character++) {
    capacity += *character == ':' || *character == ';';
  }
  char **items = malloc(sizeof(char *) * (size_t)capacity);
  if (!items) {
    return 0;
  }
  int count = 0;
  size_t start = 0;
  for (size_t index = 0;; index++) {
    const bool at_end = list[index] == '\0';
    if (at_end || cpath_ends_item(list, start, index)) {
      if (index > start) {
        char *item = cpath_copy(list + start, index - start);
        if (item) {
          items[count++] = item;
        }
      }
      start = index + 1;
    }
    if (at_end) {
      break;
    }
  }
  *paths = items;
  return count;
}

void cpath_list_destroy(char **paths, int count) {
  if (!paths) {
    return;
  }
  for (int path_idx = 0; path_idx < count; path_idx++) {
    free(paths[path_idx]);
  }
  free(paths);
}

char *cpath_list_first(const char *list) {
  char **paths = NULL;
  const int count = cpath_split_list(list, &paths);
  char *first = count > 0 ? cpath_copy(paths[0], strlen(paths[0]))
                          : cpath_copy("", 0);
  cpath_list_destroy(paths, count);
  return first;
}

bool cpath_is_separator(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

const char *cpath_basename(const char *path) {
  const char *basename = path;
  for (const char *character = path; *character; character++) {
    if (cpath_is_separator(*character)) {
      basename = character + 1;
    }
  }
  return basename;
}

// The directory part of an executable's path, NULL if it has none.
static char *cpath_directory_of(const char *path) {
  const char *basename = cpath_basename(path);
  if (basename == path) {
    return NULL;
  }
  size_t length = (size_t)(basename - path) - 1;
  // The root directory keeps its separator.
  if (length == 0) {
    length = 1;
  }
  return cpath_copy(path, length);
}

char *cpath_user_data_dir(void) {
#if defined(_WIN32)
  const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
  if (!local || !local[0]) {
    return NULL;
  }
  char *base = cwin_narrow(local);
  if (!base) {
    return NULL;
  }
  static const char suffix[] = "\\MAGPIE";
  const size_t size = strlen(base) + sizeof(suffix);
  char *directory = malloc(size);
  if (directory) {
    (void)snprintf(directory, size, "%s%s", base, suffix);
  }
  free(base);
  return directory;
#elif defined(__APPLE__)
  const char *home = getenv("HOME");
  if (!home || !home[0]) {
    return NULL;
  }
  static const char suffix[] = "/.magpie";
  const size_t size = strlen(home) + sizeof(suffix);
  char *directory = malloc(size);
  if (directory) {
    (void)snprintf(directory, size, "%s%s", home, suffix);
  }
  return directory;
#else
  return NULL;
#endif
}

bool cpath_fetches_to_user_data_dir(void) {
#if defined(__APPLE__)
  return true;
#else
  return false;
#endif
}

char *cpath_executable_dir(void) {
#if defined(_WIN32)
  for (DWORD capacity = MAX_PATH; capacity <= CPATH_EXECUTABLE_PATH_MAX;
       capacity *= 2) {
    wchar_t *wide = malloc(sizeof(wchar_t) * capacity);
    if (!wide) {
      return NULL;
    }
    const DWORD length = GetModuleFileNameW(NULL, wide, capacity);
    if (length == 0) {
      free(wide);
      return NULL;
    }
    if (length < capacity) {
      char *path = cwin_narrow(wide);
      char *directory = path ? cpath_directory_of(path) : NULL;
      free(path);
      free(wide);
      return directory;
    }
    free(wide);
  }
  return NULL;
#elif defined(__APPLE__)
  uint32_t size = 0;
  (void)_NSGetExecutablePath(NULL, &size);
  char *path = malloc(size + 1);
  if (!path || _NSGetExecutablePath(path, &size) != 0) {
    free(path);
    return NULL;
  }
  char *resolved = realpath(path, NULL);
  char *directory = cpath_directory_of(resolved ? resolved : path);
  free(resolved);
  free(path);
  return directory;
#elif defined(__EMSCRIPTEN__)
  return NULL;
#else
  for (size_t capacity = 256; capacity <= CPATH_EXECUTABLE_PATH_MAX;
       capacity *= 2) {
    char *path = malloc(capacity);
    if (!path) {
      return NULL;
    }
    const ssize_t length = readlink("/proc/self/exe", path, capacity);
    if (length < 0) {
      free(path);
      return NULL;
    }
    if ((size_t)length < capacity) {
      path[length] = '\0';
      char *directory = cpath_directory_of(path);
      free(path);
      return directory;
    }
    free(path);
  }
  return NULL;
#endif
}
