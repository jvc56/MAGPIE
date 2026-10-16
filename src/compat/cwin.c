#include "cwin.h"

#if defined(_WIN32)
#include <errno.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

wchar_t *cwin_wide(const char *text) {
  const int length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (length <= 0) {
    errno = EINVAL;
    return NULL;
  }
  wchar_t *wide = malloc(sizeof(wchar_t) * (size_t)length);
  if (!wide) {
    errno = ENOMEM;
    return NULL;
  }
  (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                            length);
  return wide;
}

char *cwin_narrow(const wchar_t *wide) {
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

void cwin_set_errno(unsigned long error) {
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
#endif
