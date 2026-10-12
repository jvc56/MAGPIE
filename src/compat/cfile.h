#ifndef CFILE_H
#define CFILE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// File operations C99 leaves to the platform, kept here so that nothing
// outside src/compat tests platform macros.

// Opens `path` as fopen would with `mode` ("r", "w" or "a", optionally with
// "b"; no "+"), but never inherited by a child process -- what glibc's
// non-standard "e" mode does. With `exclusive` (only for "w" modes) it creates
// the file and fails with EEXIST if it is already there -- what C11's "x" mode
// does. Returns NULL with errno set on failure, as fopen does.
FILE *cfile_open(const char *path, const char *mode, bool exclusive);

// An advisory lock on a lock file, released when it is closed or the process
// dies: cfile_lock_open opens (creating) the file and returns its handle, or
// -1 with errno set; cfile_lock_try takes the lock without waiting and says
// whether it got it, with *busy set when another process holds it rather than
// the filesystem refusing locks; cfile_lock_close releases and closes.
int cfile_lock_open(const char *path);
bool cfile_lock_try(int lock, bool *busy);
void cfile_lock_close(int lock);

// The local time of `time`, thread safely. Returns false if it cannot be
// represented.
bool cfile_localtime(time_t time, struct tm *local);

// Whether `stream` is open on a regular file: a directory, say, opens on
// some platforms and then reads as garbage.
bool cfile_is_regular_file(FILE *stream);

// This process's id.
long cfile_process_id(void);

// What identifies a file's contents short of reading them: its size, inode
// and modification and status-change times, to the nanosecond where the
// filesystem records it (0 where it does not). Returns false if `path`
// cannot be stat'ed.
typedef struct CFileIdentity {
  int64_t size;
  uint64_t inode;
  int64_t mtime_seconds;
  long mtime_nanoseconds;
  int64_t ctime_seconds;
  long ctime_nanoseconds;
} CFileIdentity;

bool cfile_identity(const char *path, CFileIdentity *identity);

#endif
