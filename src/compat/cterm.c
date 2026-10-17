#include "cterm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <errno.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

#if defined(_WIN32)
// What cterm_init changed, to put back.
typedef struct CTermSaved {
  bool saved;
  UINT input_code_page;
  UINT output_code_page;
  // A handle's console mode, saved only if cterm_init changed it.
  HANDLE output;
  DWORD output_mode;
  HANDLE error;
  DWORD error_mode;
} CTermSaved;

static CTermSaved cterm_saved = {0};

// Turns on VT processing for `standard_handle`'s console, saving the mode it
// had in *handle and *mode, or leaves *handle NULL if it is not a console or
// already had it.
static void cterm_enable_vt(DWORD standard_handle, HANDLE *handle,
                            DWORD *mode) {
  HANDLE console = GetStdHandle(standard_handle);
  DWORD original;
  if (console != INVALID_HANDLE_VALUE && console != NULL &&
      GetConsoleMode(console, &original) &&
      !(original & ENABLE_VIRTUAL_TERMINAL_PROCESSING) &&
      SetConsoleMode(console, original | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    *handle = console;
    *mode = original;
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
  cterm_saved.input_code_page = GetConsoleCP();
  cterm_saved.output_code_page = GetConsoleOutputCP();
  (void)SetConsoleCP(CP_UTF8);
  (void)SetConsoleOutputCP(CP_UTF8);
  cterm_enable_vt(STD_OUTPUT_HANDLE, &cterm_saved.output,
                  &cterm_saved.output_mode);
  cterm_enable_vt(STD_ERROR_HANDLE, &cterm_saved.error,
                  &cterm_saved.error_mode);
  cterm_saved.saved = true;
  (void)atexit(cterm_restore);
#endif
}

void cterm_restore(void) {
#if defined(_WIN32)
  if (!cterm_saved.saved) {
    return;
  }
  cterm_saved.saved = false;
  if (cterm_saved.input_code_page) {
    (void)SetConsoleCP(cterm_saved.input_code_page);
  }
  if (cterm_saved.output_code_page) {
    (void)SetConsoleOutputCP(cterm_saved.output_code_page);
  }
  if (cterm_saved.output) {
    (void)SetConsoleMode(cterm_saved.output, cterm_saved.output_mode);
  }
  if (cterm_saved.error) {
    (void)SetConsoleMode(cterm_saved.error, cterm_saved.error_mode);
  }
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
  return GetConsoleProcessList(processes, 2) == 1 &&
         cterm_is_terminal(stdin) && cterm_is_terminal(stdout);
#else
  return false;
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

#if defined(_WIN32) || defined(__EMSCRIPTEN__)
CTermLineWait *cterm_line_wait_create(void) { return NULL; }

bool cterm_line_wait(CTermLineWait *line_wait, FILE *stream) {
  (void)line_wait;
  (void)stream;
  return true;
}

void cterm_line_wait_end(CTermLineWait *line_wait) { (void)line_wait; }
#else
// A pipe that cterm_line_wait_end writes a byte to, which a wait polls
// alongside the terminal.
struct CTermLineWait {
  int read_end;
  int write_end;
};

static bool cterm_set_flags(int descriptor) {
  const int status = fcntl(descriptor, F_GETFL);
  const int descriptor_flags = fcntl(descriptor, F_GETFD);
  return status >= 0 && descriptor_flags >= 0 &&
         fcntl(descriptor, F_SETFL, status | O_NONBLOCK) == 0 &&
         fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

CTermLineWait *cterm_line_wait_create(void) {
  int ends[2];
  if (pipe(ends) != 0) {
    return NULL;
  }
  if (!cterm_set_flags(ends[0]) || !cterm_set_flags(ends[1])) {
    (void)close(ends[0]);
    (void)close(ends[1]);
    return NULL;
  }
  CTermLineWait *line_wait = malloc(sizeof(CTermLineWait));
  if (!line_wait) {
    (void)close(ends[0]);
    (void)close(ends[1]);
    return NULL;
  }
  line_wait->read_end = ends[0];
  line_wait->write_end = ends[1];
  return line_wait;
}

bool cterm_line_wait(CTermLineWait *line_wait, FILE *stream) {
  struct pollfd descriptors[2] = {
      {.fd = fileno(stream), .events = POLLIN, .revents = 0},
      {.fd = line_wait->read_end, .events = POLLIN, .revents = 0},
  };
  while (poll(descriptors, 2, -1) < 0) {
    if (errno != EINTR) {
      // Let the read find out what is wrong.
      return true;
    }
  }
  if (!descriptors[1].revents) {
    return true;
  }
  char drained[16];
  ssize_t count = 0;
  do {
    count = read(line_wait->read_end, drained, sizeof(drained));
  } while (count > 0);
  return false;
}

void cterm_line_wait_end(CTermLineWait *line_wait) {
  // The pipe does not block: if it is full, a wait has an end waiting anyway.
  const char wake = 'x';
  const ssize_t written = write(line_wait->write_end, &wake, 1);
  (void)written;
}
#endif
