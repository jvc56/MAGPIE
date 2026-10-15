#include "cterm.h"

#include <stdbool.h>
#include <stdio.h>

#if defined(_WIN32)
#include <io.h>
#include <stdlib.h>
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
  HANDLE handle = GetStdHandle(standard_handle);
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

void cterm_disable_error_dialogs(void) {
#if defined(_WIN32)
  (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                     SEM_NOOPENFILEERRORBOX);
#endif
}

bool cterm_owns_console(void) {
#if defined(_WIN32)
  DWORD processes[2];
  return GetConsoleProcessList(processes, 2) == 1;
#else
  return false;
#endif
}

void cterm_wait_before_close(void) {
#if defined(_WIN32)
  if (!cterm_owns_console()) {
    return;
  }
  (void)fputs("\nPress Enter to close this window.", stderr);
  (void)fflush(stderr);
  char line[16];
  (void)fgets(line, sizeof(line), stdin);
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
  // A CRT descriptor's Win32 handle comes back as an intptr_t.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return handle != -1 && GetConsoleMode((HANDLE)handle, &mode) != 0;
#else
  return isatty(fileno(stream)) != 0;
#endif
}

bool cterm_shows_wide_glyphs(FILE *stream) {
#if defined(_WIN32)
  if (!cterm_is_terminal(stream)) {
    return true;
  }
  // Windows Terminal and VS Code set these for the programs they start, and
  // both fall back to other fonts for characters the main one lacks.
  const char *windows_terminal = getenv("WT_SESSION");
  const char *term_program = getenv("TERM_PROGRAM");
  return (windows_terminal != NULL && windows_terminal[0] != '\0') ||
         (term_program != NULL && term_program[0] != '\0');
#else
  (void)stream;
  return true;
#endif
}
