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

// Where a user's own copy of MAGPIE's files goes when there is nowhere else:
// %LOCALAPPDATA%\MAGPIE on Windows and ~/.magpie on macOS. NULL elsewhere,
// where data lives beside the build. The caller frees it.
//
// macOS's has no space in it, unlike ~/Library/Application Support: a data
// path is one token in MAGPIE's commands and in a saved settings file.
char *cpath_user_data_dir(void);

// Whether data that is nowhere to be found is fetched into the user's own
// directory rather than the working directory. On macOS it is: started from
// Finder, from a terminal, or from the PATH, MAGPIE's working directory is
// wherever the shell is, the home folder for a double-click. Started from
// Explorer on Windows, it is the executable's own folder, and a shortcut
// names one.
bool cpath_fetches_to_user_data_dir(void);

// The directory holding the running executable, with no trailing separator,
// or NULL if the platform cannot say. The caller frees it.
char *cpath_executable_dir(void);

#endif
