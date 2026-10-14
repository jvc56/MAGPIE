#include "cfile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <wchar.h>
#include <windows.h>
#else
#include <sys/file.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

enum {
  // Symlinks followed to find what an atomic write replaces, as the kernel's
  // own limit (ELOOP) does.
  CFILE_MAX_SYMLINKS = 40,
  // Names tried for an atomic write's temporary before giving up.
  CFILE_TEMPORARY_ATTEMPTS = 100,
  // getline's first buffer, doubled as lines outgrow it.
  CFILE_GETLINE_INITIAL_CAPACITY = 128,
  // Windows' FileRenameInfoEx information class and its flags (Windows 10
  // 1607), which the MinGW headers declare only to builds that target that
  // version through NTDDI_VERSION.
  CFILE_FILE_RENAME_INFO_EX = 22,
  CFILE_RENAME_FLAG_REPLACE_IF_EXISTS = 0x1,
  CFILE_RENAME_FLAG_POSIX_SEMANTICS = 0x2,
  // The longest symlink target followed: Linux's PATH_MAX.
  CFILE_LINK_TARGET_MAX = 4096,
  // Room for the suffix that names a replaced file moved aside on Windows.
  CFILE_ASIDE_SUFFIX_LENGTH = 64,
};

// Like asprintf, which C99 lacks; NULL with errno set if out of memory.
static char *cfile_format(const char *format, const char *first,
                          const char *second) {
  const int length = snprintf(NULL, 0, format, first, second);
  if (length < 0) {
    errno = EINVAL;
    return NULL;
  }
  char *result = malloc((size_t)length + 1);
  if (!result) {
    errno = ENOMEM;
    return NULL;
  }
  (void)snprintf(result, (size_t)length + 1, format, first, second);
  return result;
}

static char *cfile_duplicate(const char *text) {
  return cfile_format("%s%s", text, "");
}

#if defined(_WIN32)
// Sets errno to the nearest equivalent of a Win32 error.
static void cfile_set_errno(DWORD error) {
  switch (error) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_DRIVE:
    errno = ENOENT;
    break;
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
  case ERROR_LOCK_VIOLATION:
  case ERROR_WRITE_PROTECT:
    errno = EACCES;
    break;
  case ERROR_FILE_EXISTS:
  case ERROR_ALREADY_EXISTS:
    errno = EEXIST;
    break;
  case ERROR_DISK_FULL:
  case ERROR_HANDLE_DISK_FULL:
    errno = ENOSPC;
    break;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    errno = ENOMEM;
    break;
  case ERROR_DIR_NOT_EMPTY:
    errno = ENOTEMPTY;
    break;
  case ERROR_FILENAME_EXCED_RANGE:
    errno = ENAMETOOLONG;
    break;
  case ERROR_INVALID_NAME:
  case ERROR_BAD_PATHNAME:
  case ERROR_INVALID_PARAMETER:
    errno = EINVAL;
    break;
  default:
    errno = EIO;
    break;
  }
}

// `path`, UTF-8, as the UTF-16 the wide Win32 calls take, so non-ASCII paths
// work whatever the process code page. The caller frees it. NULL with errno
// set on failure.
static wchar_t *cfile_wide(const char *path) {
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (length <= 0) {
    errno = EINVAL;
    return NULL;
  }
  wchar_t *wide = malloc(sizeof(wchar_t) * (size_t)length);
  if (!wide) {
    errno = ENOMEM;
    return NULL;
  }
  (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            length);
  return wide;
}

// The inverse of cfile_wide.
static char *cfile_narrow(const wchar_t *wide) {
  const int length =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if (length <= 0) {
    errno = EINVAL;
    return NULL;
  }
  char *narrow = malloc((size_t)length);
  if (!narrow) {
    errno = ENOMEM;
    return NULL;
  }
  (void)WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, length, NULL, NULL);
  return narrow;
}
#endif

// What an fopen mode asks for. `mode` is "r", "w" or "a", then any of "+",
// "b" and glibc's "e", in any order.
typedef struct CFileMode {
  char kind;
  bool update;
  bool binary;
  // The mode with "e" removed, which UCRT's fdopen would reject.
  char clean[4];
} CFileMode;

static bool cfile_parse_mode(const char *mode, CFileMode *parsed) {
  if (!mode || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) {
    return false;
  }
  parsed->kind = mode[0];
  parsed->update = false;
  parsed->binary = false;
  for (const char *flag = mode + 1; *flag; flag++) {
    if (*flag == '+') {
      parsed->update = true;
    } else if (*flag == 'b') {
      parsed->binary = true;
    } else if (*flag != 'e') {
      return false;
    }
  }
  int length = 0;
  parsed->clean[length++] = parsed->kind;
  if (parsed->update) {
    parsed->clean[length++] = '+';
  }
  if (parsed->binary) {
    parsed->clean[length++] = 'b';
  }
  parsed->clean[length] = '\0';
  return true;
}

FILE *cfile_open(const char *path, const char *mode, bool exclusive) {
  CFileMode parsed;
  if (!path || !cfile_parse_mode(mode, &parsed) ||
      (exclusive && parsed.kind != 'w')) {
    errno = EINVAL;
    return NULL;
  }
#if defined(_WIN32)
  DWORD access = 0;
  DWORD disposition = 0;
  int flags = parsed.binary ? _O_BINARY : _O_TEXT;
  switch (parsed.kind) {
  case 'r':
    access = GENERIC_READ | (parsed.update ? GENERIC_WRITE : 0);
    disposition = OPEN_EXISTING;
    flags |= parsed.update ? _O_RDWR : _O_RDONLY;
    break;
  case 'w':
    access = GENERIC_WRITE | (parsed.update ? GENERIC_READ : 0);
    disposition = exclusive ? CREATE_NEW : CREATE_ALWAYS;
    flags |= parsed.update ? _O_RDWR : _O_WRONLY;
    break;
  default:
    access = GENERIC_WRITE | (parsed.update ? GENERIC_READ : 0);
    disposition = OPEN_ALWAYS;
    flags |= _O_APPEND | (parsed.update ? _O_RDWR : _O_WRONLY);
    break;
  }
  wchar_t *wide = cfile_wide(path);
  if (!wide) {
    return NULL;
  }
  // Every share flag, FILE_SHARE_DELETE included, so that a file open here
  // never stops another process (or thread) replacing or removing it, as on
  // POSIX. The CRT's fopen shares only reading and writing. The handle is not
  // inherited by child processes (no SECURITY_ATTRIBUTES).
  HANDLE handle = CreateFileW(
      wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) {
    cfile_set_errno(GetLastError());
    return NULL;
  }
  const int descriptor = _open_osfhandle((intptr_t)handle, flags);
  if (descriptor < 0) {
    const int error_number = errno;
    (void)CloseHandle(handle);
    errno = error_number;
    return NULL;
  }
  FILE *stream = _fdopen(descriptor, parsed.clean);
  if (!stream) {
    const int error_number = errno;
    (void)_close(descriptor);
    errno = error_number;
  }
#else
  int flags = 0;
  switch (parsed.kind) {
  case 'r':
    flags = parsed.update ? O_RDWR : O_RDONLY;
    break;
  case 'w':
    flags = (parsed.update ? O_RDWR : O_WRONLY) | O_CREAT |
            (exclusive ? O_EXCL : O_TRUNC);
    break;
  default:
    flags = (parsed.update ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    break;
  }
  const int descriptor = open(path, flags | O_CLOEXEC, 0666);
  if (descriptor < 0) {
    return NULL;
  }
  FILE *stream = fdopen(descriptor, parsed.clean);
  if (!stream) {
    const int error_number = errno;
    (void)close(descriptor);
    errno = error_number;
  }
#endif
  return stream;
}

bool cfile_access(const char *path, int access_flags) {
  if (!path) {
    return false;
  }
#if defined(_WIN32)
  wchar_t *wide = cfile_wide(path);
  if (!wide) {
    return false;
  }
  const int mode = ((access_flags & CFILE_ACCESS_READ) ? 4 : 0) |
                   ((access_flags & CFILE_ACCESS_WRITE) ? 2 : 0);
  const bool allowed = _waccess(wide, mode) == 0;
  free(wide);
  return allowed;
#else
  const int mode = ((access_flags & CFILE_ACCESS_READ) ? R_OK : 0) |
                   ((access_flags & CFILE_ACCESS_WRITE) ? W_OK : 0);
  return access(path, mode == 0 ? F_OK : mode) == 0;
#endif
}

bool cfile_is_directory(const char *path) {
  if (!path) {
    return false;
  }
#if defined(_WIN32)
  wchar_t *wide = cfile_wide(path);
  if (!wide) {
    return false;
  }
  const DWORD attributes = GetFileAttributesW(wide);
  free(wide);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat info;
  return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

int cfile_mkdir(const char *path) {
  if (!path) {
    errno = EINVAL;
    return -1;
  }
#if defined(_WIN32)
  wchar_t *wide = cfile_wide(path);
  if (!wide) {
    return -1;
  }
  const int result = _wmkdir(wide);
  free(wide);
  return result == 0 ? 0 : -1;
#else
  return mkdir(path, 0777) == 0 ? 0 : -1;
#endif
}

int cfile_rmdir(const char *path) {
  if (!path) {
    errno = EINVAL;
    return -1;
  }
#if defined(_WIN32)
  wchar_t *wide = cfile_wide(path);
  if (!wide) {
    return -1;
  }
  const int result = _wrmdir(wide);
  free(wide);
  return result == 0 ? 0 : -1;
#else
  return rmdir(path) == 0 ? 0 : -1;
#endif
}

#if defined(_WIN32)
// The last resort for replacing a target that is open, where the file system
// cannot rename over it: moves the target aside (allowed while every handle
// on it shares deletion, as cfile_open's do), moves `from` into its place,
// then deletes the old file, which goes when its last handle closes. Not
// atomic -- for a moment the target does not exist -- so it is only a
// fallback. Restores the target if `from` cannot be moved.
static bool cfile_rename_around_open_target(const wchar_t *from,
                                            const wchar_t *to) {
  static volatile LONG counter = 0;
  const size_t to_length = wcslen(to);
  wchar_t *aside =
      malloc(sizeof(wchar_t) * (to_length + CFILE_ASIDE_SUFFIX_LENGTH));
  if (!aside) {
    return false;
  }
  (void)swprintf(aside, to_length + CFILE_ASIDE_SUFFIX_LENGTH, L"%ls.old.%lu-%ld",
                 to, (unsigned long)GetCurrentProcessId(),
                 InterlockedIncrement(&counter));
  bool renamed = false;
  if (MoveFileExW(to, aside, MOVEFILE_WRITE_THROUGH)) {
    renamed = MoveFileExW(from, to, MOVEFILE_WRITE_THROUGH) != 0;
    if (renamed) {
      (void)DeleteFileW(aside);
    } else {
      const DWORD error = GetLastError();
      (void)MoveFileExW(aside, to, MOVEFILE_WRITE_THROUGH);
      SetLastError(error);
    }
  }
  free(aside);
  return renamed;
}
#endif

int cfile_rename(const char *from, const char *to) {
  if (!from || !to) {
    errno = EINVAL;
    return -1;
  }
#if defined(_WIN32)
  wchar_t *wide_from = cfile_wide(from);
  wchar_t *wide_to = wide_from ? cfile_wide(to) : NULL;
  if (!wide_to) {
    free(wide_from);
    return -1;
  }
  // The rename information takes the new name as a full path.
  const DWORD full_length = GetFullPathNameW(wide_to, 0, NULL, NULL);
  wchar_t *full_to = full_length ? malloc(sizeof(wchar_t) * full_length) : NULL;
  if (!full_to ||
      GetFullPathNameW(wide_to, full_length, full_to, NULL) >= full_length) {
    const DWORD error = GetLastError();
    free(wide_from);
    free(wide_to);
    free(full_to);
    cfile_set_errno(error);
    return -1;
  }
  // POSIX semantics where the file system has them (NTFS on Windows 10 1607
  // and later): the name moves even while other handles hold the target open
  // with FILE_SHARE_DELETE, as every cfile_open handle does, and those
  // handles keep reading the replaced file, as on POSIX.
  bool renamed = false;
  DWORD error = ERROR_SUCCESS;
  HANDLE handle =
      CreateFileW(wide_from, DELETE | SYNCHRONIZE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (handle != INVALID_HANDLE_VALUE) {
    const size_t name_bytes = wcslen(full_to) * sizeof(wchar_t);
    const size_t info_size =
        offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t);
    FILE_RENAME_INFO *info = calloc(1, info_size);
    if (info) {
      // Flags shares its offset with the older ReplaceIfExists field, which
      // is how the headers spell it before Windows 10 1607.
      const DWORD flags = CFILE_RENAME_FLAG_REPLACE_IF_EXISTS |
                          CFILE_RENAME_FLAG_POSIX_SEMANTICS;
      memcpy(info, &flags, sizeof(flags));
      info->RootDirectory = NULL;
      info->FileNameLength = (DWORD)name_bytes;
      memcpy(info->FileName, full_to, name_bytes + sizeof(wchar_t));
      renamed = SetFileInformationByHandle(
                    handle, (FILE_INFO_BY_HANDLE_CLASS)CFILE_FILE_RENAME_INFO_EX,
                    info, (DWORD)info_size) != 0;
      error = GetLastError();
      free(info);
    }
    (void)CloseHandle(handle);
  } else {
    error = GetLastError();
  }
  if (!renamed) {
    // Other file systems (FAT, network shares), older Windows and Wine:
    // replaces the target unless something holds it open.
    renamed = MoveFileExW(wide_from, full_to,
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
              0;
    if (!renamed) {
      error = GetLastError();
    }
  }
  if (!renamed &&
      (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)) {
    renamed = cfile_rename_around_open_target(wide_from, full_to);
  }
  free(wide_from);
  free(wide_to);
  free(full_to);
  if (!renamed) {
    cfile_set_errno(error);
    return -1;
  }
  return 0;
#else
  return rename(from, to) == 0 ? 0 : -1;
#endif
}

int64_t cfile_getline(char **line, size_t *capacity, FILE *stream) {
#if defined(_WIN32)
  // UCRT has no getline. Read with fgets, a buffer at a time; a line ends
  // where fgets stops short of filling the buffer or stops at a newline.
  if (!line || !capacity || !stream) {
    errno = EINVAL;
    return -1;
  }
  if (!*line || *capacity < 2) {
    char *grown = realloc(*line, CFILE_GETLINE_INITIAL_CAPACITY);
    if (!grown) {
      errno = ENOMEM;
      return -1;
    }
    *line = grown;
    *capacity = CFILE_GETLINE_INITIAL_CAPACITY;
  }
  size_t length = 0;
  while (true) {
    if (*capacity - length < 2) {
      if (*capacity > (size_t)INT_MAX) {
        errno = EOVERFLOW;
        return -1;
      }
      char *grown = realloc(*line, *capacity * 2);
      if (!grown) {
        errno = ENOMEM;
        return -1;
      }
      *line = grown;
      *capacity *= 2;
    }
    if (!fgets(*line + length, (int)(*capacity - length), stream)) {
      break;
    }
    // fgets stops after a newline, so the text it read is all one line; a
    // NUL inside the line is not a concern for the text files read here.
    length += strlen(*line + length);
    if (length > 0 && (*line)[length - 1] == '\n') {
      break;
    }
  }
  if (length == 0) {
    return -1;
  }
  return (int64_t)length;
#else
  const ssize_t length = getline(line, capacity, stream);
  return length < 0 ? -1 : (int64_t)length;
#endif
}

#if defined(_WIN32)
// The file a path names, through any symlinks or junctions, so an atomic
// write replaces the target rather than the link. A path that does not exist
// yet is returned as given.
static char *cfile_resolve_destination(const char *path) {
  wchar_t *wide = cfile_wide(path);
  if (!wide) {
    return NULL;
  }
  HANDLE handle = CreateFileW(
      wide, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return cfile_duplicate(path);
    }
    cfile_set_errno(error);
    return NULL;
  }
  const DWORD length = GetFinalPathNameByHandleW(handle, NULL, 0, 0);
  wchar_t *final_path = length ? malloc(sizeof(wchar_t) * length) : NULL;
  char *resolved = NULL;
  if (final_path &&
      GetFinalPathNameByHandleW(handle, final_path, length, 0) < length) {
    resolved = cfile_narrow(final_path);
  } else {
    cfile_set_errno(GetLastError());
  }
  free(final_path);
  (void)CloseHandle(handle);
  return resolved;
}

// Opens a new temporary beside `destination` with a name nothing else uses.
static FILE *cfile_create_temporary(const char *destination,
                                    char **temporary) {
  static volatile LONG counter = 0;
  for (int attempt = 0; attempt < CFILE_TEMPORARY_ATTEMPTS; attempt++) {
    char suffix[64];
    (void)snprintf(suffix, sizeof(suffix), ".tmp.%lu-%ld-%llu",
                   (unsigned long)GetCurrentProcessId(),
                   InterlockedIncrement(&counter),
                   (unsigned long long)GetTickCount64());
    *temporary = cfile_format("%s%s", destination, suffix);
    if (!*temporary) {
      return NULL;
    }
    FILE *stream = cfile_open(*temporary, "wb", true);
    if (stream || errno != EEXIST) {
      return stream;
    }
    free(*temporary);
    *temporary = NULL;
  }
  errno = EEXIST;
  return NULL;
}
#else
// The file a path names, through any symlinks, so an atomic write replaces
// the target rather than the link. Sets *metadata to the target's, and
// *exists to whether there is one.
static char *cfile_resolve_destination(const char *path, struct stat *metadata,
                                       bool *exists) {
  char *destination = cfile_duplicate(path);
  if (!destination) {
    return NULL;
  }
  for (int links = 0;; links++) {
    if (lstat(destination, metadata) != 0) {
      if (errno != ENOENT) {
        free(destination);
        return NULL;
      }
      *exists = false;
      return destination;
    }
    if (!S_ISLNK(metadata->st_mode)) {
      *exists = true;
      return destination;
    }
    if (links == CFILE_MAX_SYMLINKS) {
      free(destination);
      errno = ELOOP;
      return NULL;
    }
    char target[CFILE_LINK_TARGET_MAX + 1];
    const ssize_t length =
        readlink(destination, target, CFILE_LINK_TARGET_MAX);
    if (length < 0 || length == CFILE_LINK_TARGET_MAX) {
      free(destination);
      if (length == CFILE_LINK_TARGET_MAX) {
        errno = ENAMETOOLONG;
      }
      return NULL;
    }
    target[length] = '\0';
    const char *slash = strrchr(destination, '/');
    char *resolved = NULL;
    if (target[0] == '/' || !slash) {
      resolved = cfile_duplicate(target);
    } else {
      // The link's directory, then its relative target.
      const size_t directory_length = (size_t)(slash - destination);
      char *directory = malloc(directory_length + 1);
      if (directory) {
        memcpy(directory, destination, directory_length);
        directory[directory_length] = '\0';
        resolved = cfile_format("%s/%s", directory, target);
        free(directory);
      } else {
        errno = ENOMEM;
      }
    }
    free(destination);
    if (!resolved) {
      return NULL;
    }
    destination = resolved;
  }
}

// Opens a new temporary beside `destination` with a name nothing else uses,
// with the target's permission bits if it exists and the umask's otherwise.
static FILE *cfile_create_temporary(const char *destination,
                                    const struct stat *metadata, bool exists,
                                    char **temporary) {
  *temporary = cfile_format("%s%s", destination, ".tmp.XXXXXX");
  if (!*temporary) {
    return NULL;
  }
  int descriptor = mkstemp(*temporary);
  if (descriptor < 0) {
    return NULL;
  }
  if (exists) {
    if (fchmod(descriptor, metadata->st_mode & 0777) != 0) {
      const int error_number = errno;
      (void)close(descriptor);
      (void)unlink(*temporary);
      errno = error_number;
      return NULL;
    }
  } else {
    // Let open apply the current umask without temporarily changing this
    // process-wide setting while other threads might be creating files.
    (void)close(descriptor);
    if (unlink(*temporary) != 0) {
      return NULL;
    }
    // O_EXCL rejects an intervening creator; never unlink their file.
    descriptor =
        open(*temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (descriptor < 0) {
      return NULL;
    }
  }
  FILE *stream = fdopen(descriptor, "wb");
  if (!stream) {
    const int error_number = errno;
    (void)close(descriptor);
    (void)unlink(*temporary);
    errno = error_number;
  }
  return stream;
}
#endif

FILE *cfile_atomic_write_begin(CFileAtomicWrite *atomic_write,
                               const char *path) {
  atomic_write->stream = NULL;
  atomic_write->destination = NULL;
  atomic_write->temporary = NULL;
  if (!path) {
    errno = EINVAL;
    return NULL;
  }
#if defined(_WIN32)
  atomic_write->destination = cfile_resolve_destination(path);
  if (atomic_write->destination) {
    atomic_write->stream = cfile_create_temporary(atomic_write->destination,
                                                  &atomic_write->temporary);
  }
#else
  struct stat metadata;
  bool exists = false;
  atomic_write->destination =
      cfile_resolve_destination(path, &metadata, &exists);
  if (atomic_write->destination) {
    atomic_write->stream =
        cfile_create_temporary(atomic_write->destination, &metadata, exists,
                               &atomic_write->temporary);
  }
#endif
  if (!atomic_write->stream) {
    const int error_number = errno;
    free(atomic_write->destination);
    free(atomic_write->temporary);
    atomic_write->destination = NULL;
    atomic_write->temporary = NULL;
    errno = error_number;
  }
  return atomic_write->stream;
}

bool cfile_atomic_write_end(CFileAtomicWrite *atomic_write, bool commit) {
  if (!atomic_write->stream) {
    errno = EINVAL;
    return false;
  }
  bool succeeded = fclose(atomic_write->stream) == 0;
  int error_number = succeeded ? 0 : errno;
  if (succeeded && commit &&
      cfile_rename(atomic_write->temporary, atomic_write->destination) != 0) {
    error_number = errno;
    succeeded = false;
  }
  if (!succeeded || !commit) {
    (void)remove(atomic_write->temporary);
  }
  free(atomic_write->destination);
  free(atomic_write->temporary);
  atomic_write->stream = NULL;
  atomic_write->destination = NULL;
  atomic_write->temporary = NULL;
  errno = error_number;
  return succeeded;
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
  // A CRT descriptor's Win32 handle comes back as an intptr_t.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
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

// localtime_s writes through `local`, which cppcheck's Windows pass does not
// know.
// cppcheck-suppress constParameterPointer
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

int cfile_seek(FILE *stream, int64_t offset, int whence) {
#if defined(_WIN32)
  return _fseeki64(stream, offset, whence) == 0 ? 0 : -1;
#else
  return fseeko(stream, (off_t)offset, whence) == 0 ? 0 : -1;
#endif
}

int64_t cfile_tell(FILE *stream) {
#if defined(_WIN32)
  return (int64_t)_ftelli64(stream);
#else
  return (int64_t)ftello(stream);
#endif
}

bool cfile_stream_size(FILE *stream, int64_t *size) {
  const int64_t position = cfile_tell(stream);
  if (position < 0 || cfile_seek(stream, 0, SEEK_END) != 0) {
    return false;
  }
  *size = cfile_tell(stream);
  return cfile_seek(stream, position, SEEK_SET) == 0 && *size >= 0;
}

long cfile_process_id(void) {
#if defined(_WIN32)
  return (long)_getpid();
#else
  return (long)getpid();
#endif
}

bool cfile_identity(const char *path, CFileIdentity *identity) {
  if (!path) {
    return false;
  }
#if defined(_WIN32)
  // The 64-bit stat: plain stat's size is 32 bits on Windows.
  wchar_t *wide = cfile_wide(path);
  if (!wide) {
    return false;
  }
  struct _stat64 info;
  const int result = _wstat64(wide, &info);
  free(wide);
  if (result != 0) {
    return false;
  }
#else
  struct stat info;
  if (stat(path, &info) != 0) {
    return false;
  }
#endif
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
  wchar_t *wide = cfile_wide(directory);
  if (!wide) {
    return false;
  }
  const BOOL queried = GetDiskFreeSpaceExW(wide, &available, NULL, NULL);
  free(wide);
  if (!queried) {
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
