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
#include "cwin.h"
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
  // Replaces a read-only target, as POSIX rename does (Windows 10 1809).
  CFILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE = 0x40,
  // The longest symlink target followed: Linux's PATH_MAX.
  CFILE_LINK_TARGET_MAX = 4096,
  // Windows file times: 100 ns ticks since 1601, and 1970 in them.
  CFILE_TICKS_PER_SECOND = 10000000,
  // The most reparse data a Windows link can hold
  // (MAXIMUM_REPARSE_DATA_BUFFER_SIZE).
  CFILE_REPARSE_BUFFER_BYTES = 16 * 1024,
  // Room for the suffix that names a replaced file moved aside on Windows.
  CFILE_ASIDE_SUFFIX_LENGTH = 64,
  // Room for the name of the file cfile_access creates to test a directory.
  CFILE_ACCESS_PROBE_NAME_LENGTH = 64,
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
  wchar_t *wide = cwin_wide(path);
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
    cwin_set_errno(GetLastError());
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

#if defined(_WIN32)
// Whether a file can be created in the directory `wide`: _waccess says yes
// for any directory that exists, whatever its permissions, so this creates a
// file there, which goes again when it is closed.
static bool cfile_can_create_in(const wchar_t *wide) {
  static volatile LONG counter = 0;
  const size_t length = wcslen(wide) + CFILE_ACCESS_PROBE_NAME_LENGTH;
  wchar_t *probe = malloc(sizeof(wchar_t) * length);
  if (!probe) {
    return false;
  }
  (void)swprintf(probe, length, L"%ls\\.cfile-access.%lu-%ld", wide,
                 (unsigned long)GetCurrentProcessId(),
                 InterlockedIncrement(&counter));
  HANDLE handle = CreateFileW(
      probe, GENERIC_WRITE, 0, NULL, CREATE_NEW,
      FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN |
          FILE_FLAG_DELETE_ON_CLOSE,
      NULL);
  free(probe);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  (void)CloseHandle(handle);
  return true;
}
#endif

bool cfile_access(const char *path, int access_flags) {
  if (!path) {
    return false;
  }
#if defined(_WIN32)
  wchar_t *wide = cwin_wide(path);
  if (!wide) {
    return false;
  }
  const int mode = ((access_flags & CFILE_ACCESS_READ) ? 4 : 0) |
                   ((access_flags & CFILE_ACCESS_WRITE) ? 2 : 0);
  bool allowed = _waccess(wide, mode) == 0;
  if (allowed && (access_flags & CFILE_ACCESS_WRITE)) {
    const DWORD attributes = GetFileAttributesW(wide);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      allowed = cfile_can_create_in(wide);
    }
  }
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
  wchar_t *wide = cwin_wide(path);
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

bool cfile_is_symlink(const char *path) {
  if (!path) {
    return false;
  }
#if defined(_WIN32)
  wchar_t *wide = cwin_wide(path);
  if (!wide) {
    return false;
  }
  // Only links count: OneDrive's placeholders and other cloud files are
  // reparse points too, but they are the file or directory itself. The
  // reparse tag comes with a directory listing's entry.
  WIN32_FIND_DATAW entry;
  HANDLE find = FindFirstFileW(wide, &entry);
  free(wide);
  if (find == INVALID_HANDLE_VALUE) {
    return false;
  }
  (void)FindClose(find);
  return (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
         (entry.dwReserved0 == IO_REPARSE_TAG_SYMLINK ||
          entry.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
#else
  struct stat info;
  return lstat(path, &info) == 0 && S_ISLNK(info.st_mode);
#endif
}

int cfile_remove_symlink(const char *path) {
  if (!path) {
    errno = EINVAL;
    return -1;
  }
#if defined(_WIN32)
  wchar_t *wide = cwin_wide(path);
  if (!wide) {
    return -1;
  }
  // A link to a directory is itself a directory, removed as one; neither call
  // touches what the link points to.
  const DWORD attributes = GetFileAttributesW(wide);
  const BOOL removed = attributes != INVALID_FILE_ATTRIBUTES &&
                               (attributes & FILE_ATTRIBUTE_DIRECTORY)
                           ? RemoveDirectoryW(wide)
                           : DeleteFileW(wide);
  const DWORD error = GetLastError();
  free(wide);
  if (!removed) {
    cwin_set_errno(error);
    return -1;
  }
  return 0;
#else
  return unlink(path) == 0 ? 0 : -1;
#endif
}

int cfile_mkdir(const char *path) {
  if (!path) {
    errno = EINVAL;
    return -1;
  }
#if defined(_WIN32)
  wchar_t *wide = cwin_wide(path);
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
  wchar_t *wide = cwin_wide(path);
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
// Deletes `path`, making it writable first if it is read-only, which would
// otherwise stop it being deleted.
static void cfile_delete_file(const wchar_t *path) {
  if (!DeleteFileW(path) && GetLastError() == ERROR_ACCESS_DENIED &&
      SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL)) {
    (void)DeleteFileW(path);
  }
}

// The last resort for replacing a target that is open, where the file system
// cannot rename over it: moves the target aside (allowed while every handle
// on it shares deletion, as cfile_open's do), moves `from` into its place,
// then deletes the old file, which goes when its last handle closes. Not
// atomic -- for a moment the target does not exist -- so it is only a
// fallback, for when `from` is known to be free to move. Restores the target
// if `from` cannot be moved after all, unless another writer has put a file
// there meanwhile, which then stands. What cannot be deleted at once (a file
// mapped into memory, say) is left as <target>.old.<pid>-<n>, for the next
// write to remove (see remove_stale_temporary_siblings in io_util.c).
static bool cfile_rename_around_open_target(const wchar_t *from,
                                            const wchar_t *to) {
  static volatile LONG counter = 0;
  const size_t to_length = wcslen(to);
  wchar_t *aside =
      malloc(sizeof(wchar_t) * (to_length + CFILE_ASIDE_SUFFIX_LENGTH));
  if (!aside) {
    return false;
  }
  (void)swprintf(aside, to_length + CFILE_ASIDE_SUFFIX_LENGTH,
                 L"%ls.old.%lu-%ld", to, (unsigned long)GetCurrentProcessId(),
                 InterlockedIncrement(&counter));
  bool renamed = false;
  if (MoveFileExW(to, aside, MOVEFILE_WRITE_THROUGH)) {
    // Replacing whatever another writer has put there since, as POSIX rename
    // would have.
    renamed = MoveFileExW(from, to,
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
              0;
    const DWORD error = GetLastError();
    if (renamed || !MoveFileExW(aside, to, MOVEFILE_WRITE_THROUGH)) {
      cfile_delete_file(aside);
    }
    SetLastError(error);
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
  wchar_t *wide_from = cwin_wide(from);
  wchar_t *wide_to = wide_from ? cwin_wide(to) : NULL;
  if (!wide_to) {
    free(wide_from);
    return -1;
  }
  // As POSIX rename, a file never replaces a directory. (Without this the
  // fallback below would move the directory aside and take its place.)
  const DWORD target_attributes = GetFileAttributesW(wide_to);
  if (target_attributes != INVALID_FILE_ATTRIBUTES &&
      (target_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    free(wide_from);
    free(wide_to);
    errno = EISDIR;
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
    cwin_set_errno(error);
    return -1;
  }
  // POSIX semantics where the file system has them (NTFS on Windows 10 1607
  // and later): the name moves even while other handles hold the target open
  // with FILE_SHARE_DELETE, as every cfile_open handle does, and those
  // handles keep reading the replaced file, as on POSIX.
  bool renamed = false;
  DWORD error = ERROR_SUCCESS;
  // Whether `from` itself can be moved: if not, a failure is not the
  // target's, and moving the target aside would not help.
  bool from_movable = false;
  HANDLE handle =
      CreateFileW(wide_from, DELETE | SYNCHRONIZE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (handle != INVALID_HANDLE_VALUE) {
    from_movable = true;
    const size_t name_bytes = wcslen(full_to) * sizeof(wchar_t);
    const size_t info_size =
        offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t);
    FILE_RENAME_INFO *info = calloc(1, info_size);
    if (info) {
      // Flags shares its offset with the older ReplaceIfExists field, which
      // is how the headers spell it before Windows 10 1607.
      const DWORD flags = CFILE_RENAME_FLAG_REPLACE_IF_EXISTS |
                          CFILE_RENAME_FLAG_POSIX_SEMANTICS |
                          CFILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE;
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
  if (!renamed && from_movable &&
      (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)) {
    renamed = cfile_rename_around_open_target(wide_from, full_to);
  }
  free(wide_from);
  free(wide_to);
  free(full_to);
  if (!renamed) {
    cwin_set_errno(error);
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
// A symbolic link's reparse data, as FSCTL_GET_REPARSE_POINT returns it
// (REPARSE_DATA_BUFFER in the Windows driver kit, which user-mode headers do
// not declare). Offsets and lengths are in bytes, into path_buffer.
typedef struct CFileSymlinkReparse {
  ULONG tag;
  USHORT data_length;
  USHORT reserved;
  USHORT substitute_name_offset;
  USHORT substitute_name_length;
  USHORT print_name_offset;
  USHORT print_name_length;
  ULONG flags;
  WCHAR path_buffer[1];
} CFileSymlinkReparse;

// The target a symbolic link at `link` names, as written in the link: relative
// to the link's directory or absolute. NULL with errno set if `link` is not a
// symbolic link or cannot be read.
static wchar_t *cfile_read_link(const wchar_t *link) {
  HANDLE handle =
      CreateFileW(link, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING,
                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                  NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    cwin_set_errno(GetLastError());
    return NULL;
  }
  unsigned char *buffer = malloc(CFILE_REPARSE_BUFFER_BYTES);
  DWORD returned = 0;
  const bool read =
      buffer && DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0,
                                buffer, CFILE_REPARSE_BUFFER_BYTES, &returned,
                                NULL);
  const DWORD error = GetLastError();
  (void)CloseHandle(handle);
  const CFileSymlinkReparse *reparse = (const CFileSymlinkReparse *)buffer;
  if (!read || returned < offsetof(CFileSymlinkReparse, path_buffer) ||
      reparse->tag != IO_REPARSE_TAG_SYMLINK) {
    free(buffer);
    if (read) {
      errno = EINVAL;
    } else {
      cwin_set_errno(error);
    }
    return NULL;
  }
  // The print name is the target as given ("table", "C:\\data\\table"); the
  // substitute name is the same, absolute ones in NT form ("\\??\\C:\\...").
  USHORT offset = reparse->print_name_offset;
  USHORT length = reparse->print_name_length;
  if (length == 0) {
    offset = reparse->substitute_name_offset;
    length = reparse->substitute_name_length;
  }
  const size_t characters = length / sizeof(WCHAR);
  wchar_t *target = malloc(sizeof(wchar_t) * (characters + 1));
  if (target) {
    memcpy(target, (const unsigned char *)reparse->path_buffer + offset,
           length);
    target[characters] = L'\0';
    if (wcsncmp(target, L"\\??\\", 4) == 0) {
      memmove(target, target + 4, sizeof(wchar_t) * (characters - 3));
    }
  } else {
    errno = ENOMEM;
  }
  free(buffer);
  return target;
}

// `target`, relative to the directory holding `link` unless it is absolute.
static wchar_t *cfile_link_target_path(const wchar_t *link,
                                       const wchar_t *target) {
  const bool absolute =
      target[0] == L'\\' || target[0] == L'/' ||
      (target[0] != L'\0' && target[1] == L':');
  const wchar_t *directory_end = NULL;
  for (const wchar_t *character = link; *character; character++) {
    if (*character == L'\\' || *character == L'/') {
      directory_end = character + 1;
    }
  }
  const size_t prefix = absolute || !directory_end
                            ? 0
                            : (size_t)(directory_end - link);
  const size_t target_length = wcslen(target);
  wchar_t *path = malloc(sizeof(wchar_t) * (prefix + target_length + 1));
  if (path) {
    memcpy(path, link, sizeof(wchar_t) * prefix);
    memcpy(path + prefix, target, sizeof(wchar_t) * (target_length + 1));
  }
  return path;
}

// The final path of the existing file `wide` names, through any links, or
// NULL with GetLastError's error left in *error.
static char *cfile_final_path(const wchar_t *wide, DWORD *error) {
  HANDLE handle = CreateFileW(
      wide, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    *error = GetLastError();
    return NULL;
  }
  const DWORD length = GetFinalPathNameByHandleW(handle, NULL, 0, 0);
  wchar_t *final_path = length ? malloc(sizeof(wchar_t) * length) : NULL;
  char *resolved = NULL;
  *error = ERROR_SUCCESS;
  if (final_path &&
      GetFinalPathNameByHandleW(handle, final_path, length, 0) < length) {
    resolved = cwin_narrow(final_path);
  } else {
    *error = GetLastError();
  }
  free(final_path);
  (void)CloseHandle(handle);
  return resolved;
}

// The file a path names, through any symlinks or junctions, so an atomic
// write replaces the target rather than the link. A path that does not exist
// yet is returned as given, and a link to a file that does not exist yet
// resolves to that file, which the write creates, as on POSIX.
static char *cfile_resolve_destination(const char *path) {
  wchar_t *current = cwin_wide(path);
  for (int links = 0; current; links++) {
    DWORD error = ERROR_SUCCESS;
    char *resolved = cfile_final_path(current, &error);
    if (resolved) {
      free(current);
      return resolved;
    }
    // ERROR_CANT_RESOLVE_FILENAME: a chain of links Windows gave up on.
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND &&
        error != ERROR_CANT_RESOLVE_FILENAME) {
      free(current);
      cwin_set_errno(error);
      return NULL;
    }
    // Nothing there, or a link to something that is not there (yet).
    const DWORD attributes = GetFileAttributesW(current);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      char *destination = cwin_narrow(current);
      free(current);
      return destination;
    }
    if (links == CFILE_MAX_SYMLINKS) {
      free(current);
      errno = ELOOP;
      return NULL;
    }
    wchar_t *target = cfile_read_link(current);
    wchar_t *next = target ? cfile_link_target_path(current, target) : NULL;
    free(target);
    free(current);
    current = next;
  }
  return NULL;
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

// Starts an atomic write of `path`, through any symlinks there when
// `follow_links` is set.
static FILE *cfile_atomic_begin(CFileAtomicWrite *atomic_write,
                                const char *path, bool follow_links) {
  atomic_write->stream = NULL;
  atomic_write->destination = NULL;
  atomic_write->temporary = NULL;
  if (!path) {
    errno = EINVAL;
    return NULL;
  }
#if defined(_WIN32)
  atomic_write->destination =
      follow_links ? cfile_resolve_destination(path) : cfile_duplicate(path);
  if (atomic_write->destination) {
    atomic_write->stream = cfile_create_temporary(atomic_write->destination,
                                                  &atomic_write->temporary);
  }
#else
  struct stat metadata;
  memset(&metadata, 0, sizeof(metadata));
  bool exists = false;
  atomic_write->destination =
      follow_links ? cfile_resolve_destination(path, &metadata, &exists)
                   : cfile_duplicate(path);
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

FILE *cfile_atomic_write_begin(CFileAtomicWrite *atomic_write,
                               const char *path) {
  return cfile_atomic_begin(atomic_write, path, true);
}

FILE *cfile_atomic_replace_begin(CFileAtomicWrite *atomic_write,
                                 const char *path) {
  return cfile_atomic_begin(atomic_write, path, false);
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

bool cfile_localtime(time_t time, struct tm *local) {
#if defined(_WIN32)
  struct tm converted = {0};
  if (localtime_s(&converted, &time) != 0) {
    return false;
  }
  *local = converted;
  return true;
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

#if defined(_WIN32)
// 1970-01-01 in Windows file time: too large for an enum.
static const LONGLONG cfile_windows_epoch_ticks = 116444736000000000LL;

// A FILETIME-style count of 100-nanosecond intervals since 1601 as seconds and
// nanoseconds since 1970.
static void cfile_split_windows_time(LONGLONG ticks, int64_t *seconds,
                                     long *nanoseconds) {
  const LONGLONG since_1970 = ticks - cfile_windows_epoch_ticks;
  *seconds = (int64_t)(since_1970 / CFILE_TICKS_PER_SECOND);
  *nanoseconds = (long)((since_1970 % CFILE_TICKS_PER_SECOND) * 100);
}
#endif

char *cfile_absolute_path(const char *path) {
#if defined(_WIN32)
  wchar_t *wide = cwin_wide(path);
  if (!wide) {
    return NULL;
  }
  const DWORD length = GetFullPathNameW(wide, 0, NULL, NULL);
  wchar_t *full = length ? malloc(sizeof(wchar_t) * length) : NULL;
  char *absolute = NULL;
  if (full && GetFullPathNameW(wide, length, full, NULL) < length) {
    absolute = cwin_narrow(full);
  }
  free(full);
  free(wide);
  return absolute;
#else
  return realpath(path, NULL);
#endif
}

bool cfile_identity(const char *path, CFileIdentity *identity) {
  if (!path) {
    return false;
  }
#if defined(_WIN32)
  // _wstat64 gives no inode (st_ino is always 0), whole seconds, and the
  // creation time as st_ctime; the handle gives the file index, and the last
  // write and change times to 100 ns.
  wchar_t *wide = cwin_wide(path);
  if (!wide) {
    return false;
  }
  HANDLE handle = CreateFileW(
      wide, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) {
    cwin_set_errno(GetLastError());
    return false;
  }
  BY_HANDLE_FILE_INFORMATION file_information;
  FILE_BASIC_INFO basic_information;
  const bool known =
      GetFileInformationByHandle(handle, &file_information) &&
      GetFileInformationByHandleEx(handle, FileBasicInfo, &basic_information,
                                   sizeof(basic_information));
  const DWORD error = GetLastError();
  (void)CloseHandle(handle);
  if (!known) {
    cwin_set_errno(error);
    return false;
  }
  identity->size = (int64_t)(((uint64_t)file_information.nFileSizeHigh << 32) |
                             file_information.nFileSizeLow);
  identity->inode = ((uint64_t)file_information.nFileIndexHigh << 32) |
                    file_information.nFileIndexLow;
  cfile_split_windows_time(basic_information.LastWriteTime.QuadPart,
                           &identity->mtime_seconds,
                           &identity->mtime_nanoseconds);
  cfile_split_windows_time(basic_information.ChangeTime.QuadPart,
                           &identity->ctime_seconds,
                           &identity->ctime_nanoseconds);
#else
  struct stat info;
  if (stat(path, &info) != 0) {
    return false;
  }
  identity->size = (int64_t)info.st_size;
  identity->inode = (uint64_t)info.st_ino;
  identity->mtime_seconds = (int64_t)info.st_mtime;
  identity->ctime_seconds = (int64_t)info.st_ctime;
#if defined(__APPLE__)
  identity->mtime_nanoseconds = (long)info.st_mtimespec.tv_nsec;
  identity->ctime_nanoseconds = (long)info.st_ctimespec.tv_nsec;
#else
  identity->mtime_nanoseconds = (long)info.st_mtim.tv_nsec;
  identity->ctime_nanoseconds = (long)info.st_ctim.tv_nsec;
#endif
#endif
  return true;
}

bool cfile_free_bytes(const char *directory, int64_t *free_bytes) {
  if (!directory) {
    return false;
  }
#if defined(_WIN32)
  ULARGE_INTEGER available;
  wchar_t *wide = cwin_wide(directory);
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

bool cfile_total_bytes(const char *directory, int64_t *total_bytes) {
  if (!directory) {
    return false;
  }
#if defined(_WIN32)
  ULARGE_INTEGER total;
  wchar_t *wide = cwin_wide(directory);
  if (!wide) {
    return false;
  }
  const BOOL queried = GetDiskFreeSpaceExW(wide, NULL, &total, NULL);
  free(wide);
  if (!queried) {
    return false;
  }
  *total_bytes = total.QuadPart > (ULONGLONG)INT64_MAX
                     ? INT64_MAX
                     : (int64_t)total.QuadPart;
#else
  struct statvfs info;
  if (statvfs(directory, &info) != 0) {
    return false;
  }
  const uint64_t block_size = info.f_frsize ? info.f_frsize : info.f_bsize;
  const uint64_t blocks = (uint64_t)info.f_blocks;
  *total_bytes = block_size != 0 && blocks > (uint64_t)INT64_MAX / block_size
                     ? INT64_MAX
                     : (int64_t)(blocks * block_size);
#endif
  return true;
}
