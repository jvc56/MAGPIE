#include "tui_crash.h"

#include "frame_dump.h"
#include <execinfo.h>
#include <fcntl.h>
#include <notcurses/notcurses.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ── Crash diagnostics ─────────────────────────────────────────────────────
//
// When the engine hits log_fatal (which abort()s) or anything else
// trips SIGSEGV/SIGBUS, we want to (a) put the terminal back in a
// usable state and (b) leave behind enough information to debug.
//
// notcurses_stop is NOT async-signal-safe but it's our only practical
// way to restore the tty from a handler — without it the user is left
// staring at smashed terminal state. The backtrace goes to a fixed
// file path so it survives even when stderr is restored to the tty
// and immediately scrolled away.

#define MAGPIE_CRASH_LOG "/tmp/magpie_crash.log"
static struct notcurses *g_crash_nc; // set when nc is alive; NULL otherwise
static void crash_write(int fd, const char *s) {
  size_t n = 0;
  while (s[n] != '\0') {
    n++;
  }
  (void)!write(fd, s, n);
}
static void crash_handler(int signo) {
  // Restore the tty first so any text written afterward is readable.
  // Yes, technically not async-signal-safe — but the alternative is
  // an unusable terminal, which is worse.
  if (g_crash_nc != NULL) {
    struct notcurses *nc = g_crash_nc;
    g_crash_nc = NULL;
    notcurses_stop(nc);
  }

  int fd =
      open(MAGPIE_CRASH_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    fd = STDERR_FILENO;
  }
  crash_write(fd, "magpie_tui crashed: signal ");
  // signo is at most 2 digits; itoa manually since async-signal-safe.
  char buf[8];
  int n = 0;
  int s = signo < 0 ? -signo : signo;
  if (s == 0) {
    buf[n++] = '0';
  }
  while (s > 0 && n < (int)sizeof(buf)) {
    buf[n++] = (char)('0' + (s % 10));
    s /= 10;
  }
  for (int i = 0; i < n / 2; i++) {
    char t = buf[i];
    buf[i] = buf[n - 1 - i];
    buf[n - 1 - i] = t;
  }
  (void)!write(fd, buf, (size_t)n);
  crash_write(fd, " (");
  const char *name = "?";
  switch (signo) {
  case SIGABRT:
    name = "SIGABRT";
    break;
  case SIGSEGV:
    name = "SIGSEGV";
    break;
  case SIGBUS:
    name = "SIGBUS";
    break;
  case SIGFPE:
    name = "SIGFPE";
    break;
  default:
    break;
  case SIGILL:
    name = "SIGILL";
    break;
  }
  crash_write(fd, name);
  crash_write(fd, ")\n\nBacktrace:\n");

  void *frames[64];
  const int count = backtrace(frames, 64);
  backtrace_symbols_fd(frames, count, fd);
  crash_write(fd, "\n");
  if (fd != STDERR_FILENO) {
    close(fd);
  }

  // Also dump to stderr so a `2>` redirect sees it.
  crash_write(STDERR_FILENO, "magpie_tui crashed: ");
  crash_write(STDERR_FILENO, name);
  crash_write(STDERR_FILENO, " — backtrace written to " MAGPIE_CRASH_LOG "\n");

  // Re-raise with the default handler so the process exits with the
  // right status (and gets dumped to a corefile when configured).
  (void)signal(signo, SIG_DFL);
  (void)raise(signo);
}
// SIGUSR1 arms a one-shot off-terminal PNG screenshot. The main loop
// services the request just after notcurses_render(). The handler only
// stores to an atomic flag, so it is async-signal-safe. This is the
// trigger for automated UI testing / remote inspection: drive input via
// a PTY harness, then `kill -USR1 <pid>` to capture what the renderer
// produced (the board + rack pixel planes the terminal never hands back).
static void dump_handler(int signo) {
  (void)signo;
  tui_frame_dump_request();
}
void install_crash_handlers(void) {
  struct sigaction sa = {0};
  sa.sa_handler = crash_handler;
  sa.sa_flags = SA_RESETHAND; // one-shot; subsequent fault uses default
  sigemptyset(&sa.sa_mask);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);

  // SIGUSR1: capture a PNG screenshot. SA_RESTART so it doesn't EINTR
  // the input poll, and not SA_RESETHAND so repeated dumps work.
  struct sigaction dump_sa = {0};
  dump_sa.sa_handler = dump_handler;
  dump_sa.sa_flags = SA_RESTART;
  sigemptyset(&dump_sa.sa_mask);
  sigaction(SIGUSR1, &dump_sa, NULL);

  // Touch the log file so we can verify the handler at least got
  // installed — if /tmp/magpie_crash.log doesn't exist post-crash,
  // we know the install never ran or got clobbered.
  int fd =
      open(MAGPIE_CRASH_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd >= 0) {
    crash_write(fd, "magpie_tui started, crash handlers installed\n");
    close(fd);
  }
}

void tui_crash_set_nc(struct notcurses *nc) { g_crash_nc = nc; }
