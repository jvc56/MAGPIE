#include "test_platform.h"

#include "../src/compat/cdir.h"
#include "../src/compat/cfile.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <wchar.h>
#include <windows.h>
#else
#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#if !defined(__EMSCRIPTEN__)
#include <sys/resource.h>
#endif
#endif

enum {
  // Names tried for a temporary directory or file before giving up.
  TEST_PLATFORM_TEMPORARY_ATTEMPTS = 1000,
  // The owner's write permission bit.
  TEST_PLATFORM_OWNER_WRITE = 0200,
  // Room for the pipe's contents on Windows, where _pipe sizes it.
  TEST_PLATFORM_PIPE_BYTES = 65536,
  // CreateSymbolicLinkW's flag for creating a symlink without the privilege,
  // in developer mode (Windows 10 1703), which older MinGW headers lack.
  TEST_PLATFORM_SYMLINK_ALLOW_UNPRIVILEGED_CREATE = 0x2,
};

// Distinguishes the temporary names one process makes.
static int temporary_name_count = 0;

static char *temporary_root = NULL;

static char *find_temporary_root(void) {
#if defined(_WIN32)
  wchar_t wide[MAX_PATH + 1];
  const DWORD length = GetTempPathW(MAX_PATH + 1, wide);
  if (length == 0 || length > MAX_PATH) {
    log_fatal("cannot find the temporary directory");
  }
  const int bytes =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  char *root = malloc_or_die((size_t)bytes);
  (void)WideCharToMultiByte(CP_UTF8, 0, wide, -1, root, bytes, NULL, NULL);
#else
  const char *tmpdir = getenv("TMPDIR");
  char *root = string_duplicate(tmpdir && tmpdir[0] ? tmpdir : "/tmp");
#endif
  size_t root_length = strlen(root);
  while (root_length > 1 &&
         (root[root_length - 1] == '/' || root[root_length - 1] == '\\')) {
    root[--root_length] = '\0';
  }
  return root;
}

const char *test_platform_temporary_root(void) {
  // Tests make their temporaries from one thread, before any they start.
  if (!temporary_root) {
    temporary_root = find_temporary_root();
  }
  return temporary_root;
}

char *test_platform_temporary_path(const char *name) {
  return get_formatted_string("%s/%s", test_platform_temporary_root(), name);
}

static char *temporary_name(const char *prefix) {
  temporary_name_count++;
  return get_formatted_string("%s/%s%ld-%d", test_platform_temporary_root(),
                              prefix, cfile_process_id(),
                              temporary_name_count);
}

char *test_platform_make_temporary_directory(const char *prefix) {
  for (int attempt_idx = 0; attempt_idx < TEST_PLATFORM_TEMPORARY_ATTEMPTS;
       attempt_idx++) {
    char *path = temporary_name(prefix);
    if (cfile_mkdir(path) == 0) {
      return path;
    }
    const int error_number = errno;
    if (error_number != EEXIST) {
      log_fatal("cannot create the temporary directory %s: %s", path,
                strerror(error_number));
    }
    free(path);
  }
  log_fatal("found no free name for a temporary directory %s*", prefix);
  return NULL;
}

char *test_platform_make_temporary_file(const char *prefix) {
  for (int attempt_idx = 0; attempt_idx < TEST_PLATFORM_TEMPORARY_ATTEMPTS;
       attempt_idx++) {
    char *path = temporary_name(prefix);
    FILE *stream = cfile_open(path, "wb", true);
    if (stream) {
      if (fclose(stream) != 0) {
        log_fatal("cannot close the temporary file %s", path);
      }
      return path;
    }
    const int error_number = errno;
    if (error_number != EEXIST) {
      log_fatal("cannot create the temporary file %s: %s", path,
                strerror(error_number));
    }
    free(path);
  }
  log_fatal("found no free name for a temporary file %s*", prefix);
  return NULL;
}

void test_platform_remove_directory_tree(const char *path) {
  // The names come first: removing entries while listing them is unspecified.
  CDir *dir = cdir_open(path);
  if (!dir) {
    log_fatal("cannot list the directory %s: %s", path, strerror(errno));
  }
  StringList *names = string_list_create();
  const char *name = NULL;
  while ((name = cdir_next(dir)) != NULL) {
    string_list_add_string(names, name);
  }
  cdir_close(dir);
  const int count = string_list_get_count(names);
  for (int name_idx = 0; name_idx < count; name_idx++) {
    char *child = get_formatted_string("%s/%s", path,
                                       string_list_get_string(names, name_idx));
    if (cfile_is_directory(child) && !test_platform_is_symlink(child)) {
      test_platform_remove_directory_tree(child);
    } else if (remove(child) != 0) {
      log_fatal("cannot remove %s: %s", child, strerror(errno));
    }
    free(child);
  }
  string_list_destroy(names);
  if (cfile_rmdir(path) != 0) {
    log_fatal("cannot remove the directory %s: %s", path, strerror(errno));
  }
}

#if defined(_WIN32)

// `path`, UTF-8, as the UTF-16 the wide Win32 calls take.
static wchar_t *wide_path(const char *path) {
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (length <= 0) {
    log_fatal("%s is not UTF-8", path);
  }
  wchar_t *wide = malloc_or_die(sizeof(wchar_t) * (size_t)length);
  (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            length);
  return wide;
}

const char *test_platform_null_device(void) { return "NUL"; }

bool test_platform_stat(const char *path, TestPlatformFileStatus *status) {
  wchar_t *wide = wide_path(path);
  struct _stat64 metadata;
  bool found = _wstat64(wide, &metadata) == 0;
  // stat's st_ino is always 0 on Windows; the file's handle has its index.
  // Backup semantics open directories too.
  HANDLE handle = INVALID_HANDLE_VALUE;
  if (found) {
    handle = CreateFileW(wide, 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  }
  free(wide);
  BY_HANDLE_FILE_INFORMATION information;
  found = handle != INVALID_HANDLE_VALUE &&
          GetFileInformationByHandle(handle, &information) != 0;
  if (handle != INVALID_HANDLE_VALUE) {
    (void)CloseHandle(handle);
  }
  if (found) {
    status->device = (uint64_t)information.dwVolumeSerialNumber;
    status->inode = ((uint64_t)information.nFileIndexHigh << 32) |
                    (uint64_t)information.nFileIndexLow;
    status->mode = (unsigned int)metadata.st_mode;
  }
  return found;
}

bool test_platform_has_posix_permissions(void) { return false; }

unsigned int test_platform_set_umask(unsigned int mask) {
  (void)mask;
  return 0;
}

bool test_platform_set_permissions(const char *path, unsigned int permissions) {
  wchar_t *wide = wide_path(path);
  const int windows_permissions = (permissions & TEST_PLATFORM_OWNER_WRITE)
                                      ? (_S_IREAD | _S_IWRITE)
                                      : _S_IREAD;
  const bool changed = _wchmod(wide, windows_permissions) == 0;
  free(wide);
  return changed;
}

bool test_platform_set_file_times(const char *path, time_t seconds) {
  wchar_t *wide = wide_path(path);
  struct __utimbuf64 times = {.actime = seconds, .modtime = seconds};
  const bool changed = _wutime64(wide, &times) == 0;
  free(wide);
  return changed;
}

bool test_platform_symlink(const char *target, const char *link_path) {
  wchar_t *wide_target = wide_path(target);
  // A symlink's target is not a path Windows parses, so '/' must be '\'.
  for (wchar_t *character = wide_target; *character; character++) {
    if (*character == L'/') {
      *character = L'\\';
    }
  }
  wchar_t *wide_link = wide_path(link_path);
  bool created =
      CreateSymbolicLinkW(wide_link, wide_target,
                          TEST_PLATFORM_SYMLINK_ALLOW_UNPRIVILEGED_CREATE) != 0;
  DWORD error = created ? ERROR_SUCCESS : GetLastError();
  if (error == ERROR_INVALID_PARAMETER) {
    // Windows before 10 1703 does not know the flag.
    created = CreateSymbolicLinkW(wide_link, wide_target, 0) != 0;
    error = created ? ERROR_SUCCESS : GetLastError();
  }
  free(wide_target);
  free(wide_link);
  if (!created && error != ERROR_PRIVILEGE_NOT_HELD) {
    log_fatal("cannot create the symlink %s: error %lu", link_path,
              (unsigned long)error);
  }
  // Wine reports success without creating anything.
  return created && test_platform_is_symlink(link_path);
}

bool test_platform_is_symlink(const char *path) {
  wchar_t *wide = wide_path(path);
  const DWORD attributes = GetFileAttributesW(wide);
  free(wide);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

void test_platform_open_pipe(FILE **reader, FILE **writer) {
  int descriptors[2];
  if (_pipe(descriptors, TEST_PLATFORM_PIPE_BYTES, _O_BINARY) != 0) {
    log_fatal("cannot open a pipe: %s", strerror(errno));
  }
  *reader = _fdopen(descriptors[0], "rb");
  *writer = _fdopen(descriptors[1], "wb");
  if (!*reader || !*writer) {
    log_fatal("cannot open streams on a pipe: %s", strerror(errno));
  }
}

int test_platform_silence_stdout(void) {
  (void)fflush(stdout);
  const int saved_stdout = _dup(_fileno(stdout));
  const int null_device = _open("NUL", _O_WRONLY);
  if (saved_stdout < 0 || null_device < 0 ||
      _dup2(null_device, _fileno(stdout)) != 0) {
    log_fatal("cannot silence stdout: %s", strerror(errno));
  }
  (void)_close(null_device);
  return saved_stdout;
}

void test_platform_restore_stdout(int saved_stdout) {
  (void)fflush(stdout);
  if (_dup2(saved_stdout, _fileno(stdout)) != 0) {
    log_fatal("cannot restore stdout: %s", strerror(errno));
  }
  (void)_close(saved_stdout);
}

FILE *test_platform_open_bounded_stream(void *buffer, size_t capacity) {
  (void)buffer;
  (void)capacity;
  return NULL;
}

#else

const char *test_platform_null_device(void) { return "/dev/null"; }

bool test_platform_stat(const char *path, TestPlatformFileStatus *status) {
  struct stat metadata;
  if (stat(path, &metadata) != 0) {
    return false;
  }
  status->device = (uint64_t)metadata.st_dev;
  status->inode = (uint64_t)metadata.st_ino;
  status->mode = (unsigned int)metadata.st_mode;
  return true;
}

bool test_platform_has_posix_permissions(void) { return true; }

unsigned int test_platform_set_umask(unsigned int mask) {
  return (unsigned int)umask((mode_t)mask);
}

bool test_platform_set_permissions(const char *path, unsigned int permissions) {
  return chmod(path, (mode_t)permissions) == 0;
}

bool test_platform_set_file_times(const char *path, time_t seconds) {
  const struct utimbuf times = {.actime = seconds, .modtime = seconds};
  return utime(path, &times) == 0;
}

bool test_platform_symlink(const char *target, const char *link_path) {
  if (symlink(target, link_path) != 0) {
    log_fatal("cannot create the symlink %s: %s", link_path, strerror(errno));
  }
  return true;
}

bool test_platform_is_symlink(const char *path) {
  struct stat status;
  return lstat(path, &status) == 0 && S_ISLNK(status.st_mode);
}

void test_platform_open_pipe(FILE **reader, FILE **writer) {
  int descriptors[2];
  if (pipe(descriptors) != 0) {
    log_fatal("cannot open a pipe: %s", strerror(errno));
  }
  *reader = fdopen(descriptors[0], "r");
  *writer = fdopen(descriptors[1], "w");
  if (!*reader || !*writer) {
    log_fatal("cannot open streams on a pipe: %s", strerror(errno));
  }
}

int test_platform_silence_stdout(void) {
  (void)fflush(stdout);
  const int saved_stdout = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
  const int null_device = open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (saved_stdout < 0 || null_device < 0 ||
      dup2(null_device, STDOUT_FILENO) < 0) {
    log_fatal("cannot silence stdout: %s", strerror(errno));
  }
  (void)close(null_device);
  return saved_stdout;
}

void test_platform_restore_stdout(int saved_stdout) {
  (void)fflush(stdout);
  if (dup2(saved_stdout, STDOUT_FILENO) < 0) {
    log_fatal("cannot restore stdout: %s", strerror(errno));
  }
  (void)close(saved_stdout);
}

FILE *test_platform_open_bounded_stream(void *buffer, size_t capacity) {
  return fmemopen(buffer, capacity, "wb");
}

#endif

#if defined(_WIN32) || defined(__EMSCRIPTEN__)

bool test_platform_can_limit_file_size(void) { return false; }

void test_platform_limit_file_size(int64_t bytes) {
  log_fatal("cannot limit file sizes to %lld bytes here", (long long)bytes);
}

void test_platform_unlimit_file_size(void) {
  log_fatal("cannot limit file sizes here");
}

#else

// The limit and SIGXFSZ handler test_platform_limit_file_size replaced.
static struct rlimit unlimited_file_size;
static void (*unlimited_file_size_handler)(int) = SIG_DFL;

bool test_platform_can_limit_file_size(void) { return true; }

void test_platform_limit_file_size(int64_t bytes) {
  if (getrlimit(RLIMIT_FSIZE, &unlimited_file_size) != 0) {
    log_fatal("cannot read the file size limit: %s", strerror(errno));
  }
  struct rlimit limited = unlimited_file_size;
  limited.rlim_cur = (rlim_t)bytes;
  // Ignored, the signal a write past the limit raises leaves the write to
  // fail with EFBIG instead of ending the process.
  unlimited_file_size_handler = signal(SIGXFSZ, SIG_IGN);
  if (unlimited_file_size_handler == SIG_ERR) {
    log_fatal("cannot ignore SIGXFSZ");
  }
  if (setrlimit(RLIMIT_FSIZE, &limited) != 0) {
    log_fatal("cannot limit file sizes: %s", strerror(errno));
  }
}

void test_platform_unlimit_file_size(void) {
  if (setrlimit(RLIMIT_FSIZE, &unlimited_file_size) != 0) {
    log_fatal("cannot lift the file size limit: %s", strerror(errno));
  }
  if (signal(SIGXFSZ, unlimited_file_size_handler) == SIG_ERR) {
    log_fatal("cannot restore the SIGXFSZ handler");
  }
}

#endif

bool test_platform_has_host_locales(void) {
#if defined(__EMSCRIPTEN__)
  return false;
#else
  return true;
#endif
}

bool test_platform_qsort_is_glibc_merge_sort(void) {
#if defined(__GLIBC__)
  // glibc 2.37 and 2.38 sorted with an introsort; every other glibc
  // merge-sorts whenever it can allocate its scratch space.
  char *after_major = NULL;
  const char *version = gnu_get_libc_version();
  const long major = strtol(version, &after_major, 10);
  if (after_major == version || *after_major != '.') {
    return false;
  }
  const long minor = strtol(after_major + 1, NULL, 10);
  return major == 2 && (minor < 37 || minor > 38);
#else
  return false;
#endif
}
