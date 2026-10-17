#include "cmap.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#include "cwin.h"
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

bool cmap_open(CMap *map, const char *path, bool random_access) {
  map->data = NULL;
  map->size = 0;
  if (!path) {
    errno = EINVAL;
    return false;
  }
#if defined(_WIN32)
  (void)random_access;
  wchar_t *wide = cwin_wide(path);
  if (!wide) {
    return false;
  }
  // Shares deletion like every other open, so a mapped file can still be
  // renamed over (whether the file system then lets go of it is its own
  // business: see cfile_rename).
  HANDLE file = CreateFileW(
      wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(wide);
  if (file == INVALID_HANDLE_VALUE) {
    cwin_set_errno(GetLastError());
    return false;
  }
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      (uint64_t)size.QuadPart > (uint64_t)SIZE_MAX) {
    (void)CloseHandle(file);
    errno = EINVAL;
    return false;
  }
  HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
  // The view keeps the file and the mapping alive once it exists.
  (void)CloseHandle(file);
  if (!mapping) {
    errno = ENOMEM;
    return false;
  }
  const void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  (void)CloseHandle(mapping);
  if (!view) {
    errno = ENOMEM;
    return false;
  }
  map->data = view;
  map->size = (size_t)size.QuadPart;
#else
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return false;
  }
  struct stat info;
  if (fstat(descriptor, &info) != 0) {
    const int error_number = errno;
    (void)close(descriptor);
    errno = error_number;
    return false;
  }
  if (info.st_size <= 0 || (uint64_t)info.st_size > (uint64_t)SIZE_MAX) {
    (void)close(descriptor);
    errno = EINVAL;
    return false;
  }
  const size_t size = (size_t)info.st_size;
  void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
  const int error_number = errno;
  (void)close(descriptor);
  if (mapped == MAP_FAILED) {
    errno = error_number;
    return false;
  }
  if (random_access) {
    (void)madvise(mapped, size, MADV_RANDOM);
  }
  map->data = mapped;
  map->size = size;
#endif
  return true;
}

void cmap_close(CMap *map) {
  if (!map->data) {
    return;
  }
#if defined(_WIN32)
  (void)UnmapViewOfFile(map->data);
#else
  // munmap takes a non-const pointer to memory it never writes through.
  (void)munmap((void *)map->data, map->size);
#endif
  map->data = NULL;
  map->size = 0;
}
