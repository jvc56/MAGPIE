#include "cterm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(_WIN32)
static UINT original_input_code_page = 0;
static UINT original_output_code_page = 0;

static void cterm_restore_code_pages(void) {
  if (original_input_code_page) {
    (void)SetConsoleCP(original_input_code_page);
  }
  if (original_output_code_page) {
    (void)SetConsoleOutputCP(original_output_code_page);
  }
}

static void cterm_enable_vt(DWORD standard_handle) {
  const HANDLE handle = GetStdHandle(standard_handle);
  DWORD mode;
  if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
    (void)SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
}
#endif

void cterm_init(void) {
#if defined(_WIN32)
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  original_input_code_page = GetConsoleCP();
  original_output_code_page = GetConsoleOutputCP();
  if (original_input_code_page || original_output_code_page) {
    (void)atexit(cterm_restore_code_pages);
  }
  (void)SetConsoleCP(CP_UTF8);
  (void)SetConsoleOutputCP(CP_UTF8);
  cterm_enable_vt(STD_OUTPUT_HANDLE);
  cterm_enable_vt(STD_ERROR_HANDLE);
#endif
}

bool cterm_is_terminal(FILE *stream) {
  if (!stream) {
    return false;
  }
#if defined(_WIN32)
  // _isatty also says yes for NUL and serial ports; only a console has a
  // console mode.
  const intptr_t handle = _get_osfhandle(_fileno(stream));
  DWORD mode;
  return handle != -1 && GetConsoleMode((HANDLE)handle, &mode) != 0;
#else
  return isatty(fileno(stream)) != 0;
#endif
}
