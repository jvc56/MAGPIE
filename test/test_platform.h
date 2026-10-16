#ifndef TEST_PLATFORM_H
#define TEST_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// What tests need from the platform beyond src/compat: temporary directories,
// file permissions and times, symlinks, pipes, silencing stdout, and the
// limits that stand in for a full disk. test_platform.c is the one file in
// test/ that may test platform macros and include platform headers.
//
// Paths are UTF-8. A function returning bool for something a platform cannot
// do returns false there, and its caller skips that case, saying so in one
// line. Failures the platform should not have end the process.

// The system's directory for temporary files: $TMPDIR, or /tmp without it,
// and GetTempPath's on Windows, without a trailing separator. Lives as long
// as the process.
const char *test_platform_temporary_root(void);

// `name` in test_platform_temporary_root, which the caller frees: where
// benchmarks leave the positions they generate for later runs.
char *test_platform_temporary_path(const char *name);

// Creates a new, empty directory in test_platform_temporary_root, named
// `prefix` followed by a unique suffix, and returns its path, which the
// caller frees. mkdtemp's job, which Windows lacks.
char *test_platform_make_temporary_directory(const char *prefix);

// Creates a new, empty file in test_platform_temporary_root, named `prefix`
// followed by a unique suffix, and returns its path, which the caller frees.
char *test_platform_make_temporary_file(const char *prefix);

// Removes `path` and everything under it, as a test's temporary directory.
void test_platform_remove_directory_tree(const char *path);

// The file that discards what is written to it: /dev/null, or NUL on Windows.
const char *test_platform_null_device(void);

// What stat says of a file beyond cfile_identity: the device holding it and
// its number there, which together tell whether a path still names the same
// file (on Windows, the volume serial number and file index, which stat
// leaves out), and its mode, the file type and permission bits.
typedef struct TestPlatformFileStatus {
  uint64_t device;
  uint64_t inode;
  unsigned int mode;
} TestPlatformFileStatus;

// Fills *status for `path`. Returns false if `path` cannot be stat'ed.
bool test_platform_stat(const char *path, TestPlatformFileStatus *status);

// Whether permission bits are POSIX's. Windows makes them up from a file's
// read-only attribute, so only "writable or not" is meaningful there.
bool test_platform_has_posix_permissions(void);

// Sets the mask applied to new files' permission bits and returns the one it
// replaces, as umask does. Does nothing on Windows.
unsigned int test_platform_set_umask(unsigned int mask);

// Sets the permission bits of `path`, as chmod does. Windows can only make a
// file read-only, which it does when `permissions` lacks the owner's write
// bit. Returns false with errno set on failure.
bool test_platform_set_permissions(const char *path, unsigned int permissions);

// Sets the access and modification times of `path` to `seconds`, as utime
// does. Returns false with errno set on failure.
bool test_platform_set_file_times(const char *path, time_t seconds);

// Creates the symlink `link_path` to `target` (taken relative to the link's
// directory unless absolute), which need not exist. Returns false where the
// process may not create one: on Windows, without the privilege or
// developer mode, and under Wine, which only pretends to. Ends the process if
// it fails otherwise.
bool test_platform_symlink(const char *target, const char *link_path);

// Whether `path` is itself a symlink, not following it.
bool test_platform_is_symlink(const char *path);

// Opens a pipe: what is written to *writer can be read from *reader.
void test_platform_open_pipe(FILE **reader, FILE **writer);

// Points this process's standard output at the null device, below stdio, so
// output that bypasses the stream goes too, and returns what
// test_platform_restore_stdout needs to undo it.
int test_platform_silence_stdout(void);
void test_platform_restore_stdout(int saved_stdout);

// Opens a stream that writes into `buffer` and fails any write past
// `capacity` bytes, as a full disk would, as fmemopen(buffer, capacity, "wb")
// does. Returns NULL where there is no such stream (Windows).
FILE *test_platform_open_bounded_stream(void *buffer, size_t capacity);

// Whether test_platform_limit_file_size works here. It does not on Windows,
// which has no per-process file size limit, nor under Emscripten.
bool test_platform_can_limit_file_size(void);

// Caps the size of any file this process writes at `bytes`, as a full disk
// would stop it, with writes past it failing (EFBIG) rather than ending the
// process; test_platform_unlimit_file_size lifts the cap. Only where
// test_platform_can_limit_file_size says so.
void test_platform_limit_file_size(int64_t bytes);
void test_platform_unlimit_file_size(void);

// Whether setlocale switches to the host's locales. Emscripten's C library
// has none of its own.
bool test_platform_has_host_locales(void);

// Whether this process's qsort is glibc's merge sort, which stable_sort
// copies step for step: on glibc but for 2.37 and 2.38, whose qsort was an
// introsort.
bool test_platform_qsort_is_glibc_merge_sort(void);

#endif
