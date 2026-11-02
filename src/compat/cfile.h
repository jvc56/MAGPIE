#ifndef CFILE_H
#define CFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// File operations C99 leaves to the platform, kept here so that nothing
// outside src/compat tests platform macros.

// Paths are UTF-8 everywhere (Windows included). Functions that return an
// int return 0 on success and -1 with errno set on failure.

// Opens `path` as fopen would with `mode` ("r", "w" or "a", then any of "+"
// and "b"), but never inherited by a child process -- what glibc's
// non-standard "e" mode does, which `mode` may also name. With `exclusive`
// (only for "w" modes) it creates the file and fails with EEXIST if it is
// already there -- what C11's "x" mode does. On Windows the file is opened
// sharing deletion, so an open file never stops it being replaced or removed,
// as on POSIX. Returns NULL with errno set on failure, as fopen does.
FILE *cfile_open(const char *path, const char *mode, bool exclusive);

// What cfile_access checks, as POSIX access's F_OK, R_OK and W_OK.
enum {
  CFILE_ACCESS_EXISTS = 0,
  CFILE_ACCESS_READ = 1 << 0,
  CFILE_ACCESS_WRITE = 1 << 1,
};

// Whether `path` exists and this process may do what `access_flags` asks.
bool cfile_access(const char *path, int access_flags);

// Whether `path` names a directory.
bool cfile_is_directory(const char *path);

// Whether `path` is itself a symlink (or, on Windows, a junction), whatever
// it points to.
bool cfile_is_symlink(const char *path);

// Removes the symlink `path` itself, whether it points to a file, a directory
// or nothing.
int cfile_remove_symlink(const char *path);

// Creates the directory `path` (failing with EEXIST if something is there).
int cfile_mkdir(const char *path);

// Removes the empty directory `path`. (C's remove does on POSIX, not on
// Windows.)
int cfile_rmdir(const char *path);

// Renames `from` to `to`, replacing `to` in one step if it exists, as POSIX
// rename does: C's rename leaves replacing to the platform, and Windows'
// refuses. On Windows the target may be open, by cfile_open, elsewhere.
int cfile_rename(const char *from, const char *to);

// Reads a line, newline included, into *line (grown with realloc, as POSIX
// getline does; *line may start NULL) and returns its length, or -1 at the
// end of the stream or on an error.
int64_t cfile_getline(char **line, size_t *capacity, FILE *stream);

// Replaces a file atomically: readers see the old file or the new one, never
// part of one, and a failed or abandoned write leaves the old file alone.
typedef struct CFileAtomicWrite {
  FILE *stream;
  char *destination;
  char *temporary;
} CFileAtomicWrite;

// Starts replacing `path` by opening a new temporary file beside it for
// binary writing, and returns the stream to write it through. Symlinks at
// `path` are followed, so the link survives and its target is replaced, and
// an existing file's permission bits carry over. Returns NULL with errno set
// on failure, leaving nothing behind.
FILE *cfile_atomic_write_begin(CFileAtomicWrite *atomic_write,
                               const char *path);

// As cfile_atomic_write_begin, but replaces what is at `path` itself, a
// symlink included, rather than what a link points to, as tar does when it
// unpacks over an earlier extraction. The new file has the umask's
// permission bits.
FILE *cfile_atomic_replace_begin(CFileAtomicWrite *atomic_write,
                                 const char *path);

// Closes the stream, then with `commit` puts the temporary in place of the
// target with cfile_rename, and without it removes the temporary. Returns
// false with errno set if closing or committing failed, in which case the
// temporary is removed and the target is as it was.
bool cfile_atomic_write_end(CFileAtomicWrite *atomic_write, bool commit);

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

// Seeks `stream` as fseek does, but with a 64-bit offset everywhere: fseek
// takes a long, which is 32 bits on Windows. Returns 0, or -1 with errno set.
int cfile_seek(FILE *stream, int64_t offset, int whence);

// Where `stream` is, as ftell says, but as a 64-bit offset everywhere.
// Returns -1 with errno set on failure.
int64_t cfile_tell(FILE *stream);

// The size in bytes of what `stream` reads, a file or a precached buffer, in
// *size, leaving the stream where it was. Returns false if it cannot seek.
bool cfile_stream_size(FILE *stream, int64_t *size);

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

// The full path of the existing file `path`, whatever the current directory
// later is, or NULL if it cannot be found. The caller frees.
char *cfile_absolute_path(const char *path);

// The bytes this process may still write to the filesystem holding
// `directory`, in *free_bytes (capped at INT64_MAX). Returns false if the
// filesystem cannot be asked.
bool cfile_free_bytes(const char *directory, int64_t *free_bytes);

// The size of the filesystem holding `directory`, in *total_bytes (capped at
// INT64_MAX). Returns false if the filesystem cannot be asked.
bool cfile_total_bytes(const char *directory, int64_t *total_bytes);

#endif
