#include "cmemfile.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#endif

FILE *cmemfile_open(const void *data, size_t size) {
#if defined(_WIN32)
  wchar_t directory[MAX_PATH + 1];
  wchar_t path[MAX_PATH + 1];
  const DWORD length = GetTempPathW(MAX_PATH + 1, directory);
  if (length == 0 || length > MAX_PATH ||
      GetTempFileNameW(directory, L"mag", 0, path) == 0) {
    errno = EIO;
    return NULL;
  }
  // T: keep it in the cache rather than on disk; D: delete it on close.
  FILE *stream = _wfopen(path, L"w+bTD");
  if (!stream) {
    const int error_number = errno;
    (void)DeleteFileW(path);
    errno = error_number;
    return NULL;
  }
  if ((size > 0 && fwrite(data, 1, size, stream) != size) ||
      fflush(stream) != 0 || fseek(stream, 0, SEEK_SET) != 0) {
    const int error_number = errno;
    (void)fclose(stream);
    errno = error_number;
    return NULL;
  }
  return stream;
#else
  // fmemopen takes a non-const pointer, which a read never writes through.
  return fmemopen((void *)(uintptr_t)data, size, "rb");
#endif
}
