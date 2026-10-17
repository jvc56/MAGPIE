#ifndef CWIN_H
#define CWIN_H

#include <stddef.h>

// What the Windows code in src/compat shares: MAGPIE's strings are UTF-8 and
// the wide Win32 calls take UTF-16, so every path or name passes through
// here, and Win32 errors become errno values the same way everywhere.
// Nothing here exists elsewhere.

#if defined(_WIN32)
#include <wchar.h>

// `text`, UTF-8, as UTF-16, so non-ASCII paths and names work whatever the
// process code page. Invalid UTF-8 is refused rather than replaced. The
// caller frees it. NULL with errno set (EINVAL, ENOMEM) on failure.
wchar_t *cwin_wide(const char *text);

// The inverse of cwin_wide. The caller frees it. NULL with errno set on
// failure.
char *cwin_narrow(const wchar_t *wide);

// Sets errno to the nearest equivalent of the Win32 error `error`, as from
// GetLastError (a DWORD).
void cwin_set_errno(unsigned long error);
#endif

#endif
