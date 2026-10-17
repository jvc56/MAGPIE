#ifndef CDIR_H
#define CDIR_H

// Listing the entries of a directory, which POSIX does with opendir and
// Windows with FindFirstFile.

typedef struct CDir CDir;

// Opens the directory `path` (UTF-8) for listing. Returns NULL with errno set
// on failure.
CDir *cdir_open(const char *path);

// The name of the next entry (UTF-8), skipping "." and "..", or NULL when
// there are no more. Entries come in no particular order. The name is valid
// until the next call.
const char *cdir_next(CDir *dir);

void cdir_close(CDir *dir);

#endif
