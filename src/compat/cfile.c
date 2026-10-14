#include "cfile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/file.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

FILE *cfile_open(const char *path, const char *mode, bool exclusive) {
  int flags = 0;
  switch (mode[0]) {
  case 'r':
    flags = O_RDONLY;
    break;
  case 'w':
    flags = O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC);
    break;
  case 'a':
    flags = O_WRONLY | O_CREAT | O_APPEND;
    break;
  default:
    errno = EINVAL;
    return NULL;
  }
  if ((exclusive && mode[0] != 'w') || strchr(mode, '+') != NULL) {
    errno = EINVAL;
    return NULL;
  }
#if defined(_WIN32)
  flags |= _O_NOINHERIT;
  if (strchr(mode, 'b') != NULL) {
    flags |= _O_BINARY;
  }
  const int descriptor = _open(path, flags, _S_IREAD | _S_IWRITE);
  if (descriptor < 0) {
    return NULL;
  }
  FILE *stream = _fdopen(descriptor, mode);
  if (!stream) {
    const int error_number = errno;
    (void)_close(descriptor);
    errno = error_number;
  }
#else
  const int descriptor = open(path, flags | O_CLOEXEC, 0666);
  if (descriptor < 0) {
    return NULL;
  }
  FILE *stream = fdopen(descriptor, mode);
  if (!stream) {
    const int error_number = errno;
    (void)close(descriptor);
    errno = error_number;
  }
#endif
  return stream;
}

int cfile_lock_open(const char *path) {
#if defined(_WIN32)
  return _open(path, _O_RDWR | _O_CREAT | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
  return open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
#endif
}

bool cfile_lock_try(int lock, bool *busy) {
#if defined(_WIN32)
  OVERLAPPED overlapped = {0};
  if (LockFileEx((HANDLE)_get_osfhandle(lock),
                 LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                 &overlapped)) {
    *busy = false;
    return true;
  }
  *busy = GetLastError() == ERROR_LOCK_VIOLATION;
  return false;
#else
  if (flock(lock, LOCK_EX | LOCK_NB) == 0) {
    *busy = false;
    return true;
  }
  *busy = errno == EWOULDBLOCK || errno == EINTR;
  return false;
#endif
}

void cfile_lock_close(int lock) {
  if (lock < 0) {
    return;
  }
#if defined(_WIN32)
  (void)_close(lock);
#else
  (void)close(lock);
#endif
}

bool cfile_localtime(time_t time, struct tm *local) {
#if defined(_WIN32)
  return localtime_s(local, &time) == 0;
#else
  return localtime_r(&time, local) != NULL;
#endif
}

bool cfile_is_regular_file(FILE *stream) {
#if defined(_WIN32)
  struct _stat64 info;
  return _fstat64(_fileno(stream), &info) == 0 &&
         (info.st_mode & _S_IFMT) == _S_IFREG;
#else
  struct stat info;
  return fstat(fileno(stream), &info) == 0 && S_ISREG(info.st_mode);
#endif
}

long cfile_process_id(void) {
#if defined(_WIN32)
  return (long)_getpid();
#else
  return (long)getpid();
#endif
}

bool cfile_identity(const char *path, CFileIdentity *identity) {
  struct stat info;
  if (!path || stat(path, &info) != 0) {
    return false;
  }
  identity->size = (int64_t)info.st_size;
  identity->inode = (uint64_t)info.st_ino;
  identity->mtime_seconds = (int64_t)info.st_mtime;
  identity->ctime_seconds = (int64_t)info.st_ctime;
#if defined(_WIN32)
  identity->mtime_nanoseconds = 0;
  identity->ctime_nanoseconds = 0;
#elif defined(__APPLE__)
  identity->mtime_nanoseconds = (long)info.st_mtimespec.tv_nsec;
  identity->ctime_nanoseconds = (long)info.st_ctimespec.tv_nsec;
#else
  identity->mtime_nanoseconds = (long)info.st_mtim.tv_nsec;
  identity->ctime_nanoseconds = (long)info.st_ctim.tv_nsec;
#endif
  return true;
}

bool cfile_free_bytes(const char *directory, int64_t *free_bytes) {
  if (!directory) {
    return false;
  }
#if defined(_WIN32)
  ULARGE_INTEGER available;
  if (!GetDiskFreeSpaceExA(directory, &available, NULL, NULL)) {
    return false;
  }
  *free_bytes = available.QuadPart > (ULONGLONG)INT64_MAX
                    ? INT64_MAX
                    : (int64_t)available.QuadPart;
#else
  struct statvfs info;
  if (statvfs(directory, &info) != 0) {
    return false;
  }
  // What an unprivileged process may use: f_bavail, not f_bfree, which
  // counts the blocks the filesystem keeps back for root.
  const uint64_t block_size = info.f_frsize ? info.f_frsize : info.f_bsize;
  const uint64_t blocks = (uint64_t)info.f_bavail;
  *free_bytes = block_size != 0 && blocks > (uint64_t)INT64_MAX / block_size
                    ? INT64_MAX
                    : (int64_t)(blocks * block_size);
#endif
  return true;
}
