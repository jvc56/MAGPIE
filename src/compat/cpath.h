#ifndef CPATH_H
#define CPATH_H

#include <stdbool.h>

// Paths and lists of paths, which the platforms spell differently.
//
// A list of paths (-path, the data path) separates its paths with ':' on
// POSIX and with ';' on Windows, where ':' separates them too unless it
// follows a drive letter, so C:\magpie\data:data is two paths there and a list
// written for POSIX still works.

// Splits `list` into its paths, dropping empty ones. Returns how many there
// are and sets *paths to an array of them, which the caller frees with
// cpath_list_destroy.
int cpath_split_list(const char *list, char ***paths);
void cpath_list_destroy(char **paths, int count);

// The first path in `list` ("" if it has none), which the caller frees.
char *cpath_list_first(const char *list);

// Whether `c` separates the directories in a path: '/' everywhere, and '\'
// as well on Windows.
bool cpath_is_separator(char c);

// The part of `path` after its last separator: its file name.
const char *cpath_basename(const char *path);

// The directory holding the running executable, with no trailing separator,
// or NULL if the platform cannot say. The caller frees it.
char *cpath_executable_dir(void);

#endif
