#!/usr/bin/env bash
#
# headless.sh — drive magpie_tui in a headless PTY for text-grid assertions.
#
# No GUI terminal, no graphics, no Accessibility — pure CI-friendly. Runs the
# TUI under a termwright PTY (a display-only vt100 grid) and scrapes the cell
# text. The board/rack TILES are graphics-protocol pixels and do NOT appear
# here (that's what the SIGUSR1 PNG dump is for); everything drawn as
# notcurses cells does: menus, panels ([1]Board [2]Rack [3]Bag [4]History
# [5]Plays), player headers/scores/clocks, status bar, borders.
#
# The catch this solves: notcurses_core_init interrogates the terminal (DA /
# cursor-position queries) and BLOCKS waiting for replies. A display-only
# emulator never answers, so init hangs forever and nothing renders. We feed
# the replies ourselves (a Device Attributes response is the terminator), and
# init completes. See `sample` evidence in NOTES; this is the whole trick.
#
# Requires: termwright (cargo install termwright), python3, magpie_tui built.
#
# Subcommands:
#   launch [args...]   start the TUI headless and print the captured screen
#   capture            print the current screen as text
#   expect <substr>    exit 0 if <substr> is on screen, else 1 (for tests)
#   key <Name...>      press keys (termwright names: a, Enter, Escape, Up...)
#   type <text>        type literal text
#   stop               tear down the session
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/bin/magpie_tui"
SOCK="${MAGPIE_TUI_TW_SOCK:-/tmp/magpie_tui_tw.sock}"
COLS="${MAGPIE_TUI_COLS:-200}"
ROWS="${MAGPIE_TUI_ROWS:-64}"
# Interrogation replies notcurses waits for: cursor-position report,
# secondary DA, primary DA (the terminator that ends interrogation).
DA_B64="$(printf '\033[1;1R\033[>0;276;0c\033[?62;1;6c' | base64)"
# Timing knobs (override for faster suites; defaults are robust). The DA
# reply is injected DA_TRIES times spaced DA_DELAY apart, then we wait SETTLE
# for the first render. KEY_DELAY/KEY_SETTLE pace input.
DA_TRIES="${MAGPIE_TUI_DA_TRIES:-4}"
DA_DELAY="${MAGPIE_TUI_DA_DELAY:-0.4}"
SETTLE="${MAGPIE_TUI_SETTLE:-0.8}"
KEY_DELAY="${MAGPIE_TUI_KEY_DELAY:-0.15}"
KEY_SETTLE="${MAGPIE_TUI_KEY_SETTLE:-0.3}"

die() {
  echo "headless.sh: $*" >&2
  exit 1
}

_exec() { termwright exec --socket "$SOCK" --method "$1" --params "${2:-null}"; }

cmd_capture() {
  _exec screen '{"format":"text"}' |
    python3 -c "import sys,json;print(json.load(sys.stdin).get('result') or '')"
}

cmd_launch() {
  [ -x "$BIN" ] || die "$BIN not built — run 'make magpie_tui'"
  command -v termwright >/dev/null || die "termwright not found (cargo install termwright)"
  rm -f "$SOCK"
  termwright daemon --background --cols "$COLS" --rows "$ROWS" --socket "$SOCK" \
    -- /bin/bash -lc "cd '$REPO' && exec ./bin/magpie_tui $*" >/dev/null 2>&1
  local i
  for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
  [ -S "$SOCK" ] || die "termwright daemon socket not created"
  # Feed notcurses its interrogation replies a few times (timing-robust;
  # bytes also wait in the PTY buffer until the input thread reads them).
  local n
  for n in $(seq 1 "$DA_TRIES"); do
    sleep "$DA_DELAY"
    _exec raw "{\"bytes_base64\":\"$DA_B64\"}" >/dev/null 2>&1 || true
  done
  sleep "$SETTLE"
  cmd_capture
}

cmd_expect() {
  [ "$#" -ge 1 ] || die "usage: expect <substring>"
  if cmd_capture | grep -qF -- "$*"; then
    echo "OK: found \"$*\""
  else
    echo "MISSING: \"$*\"" >&2
    exit 1
  fi
}

cmd_key() {
  [ "$#" -ge 1 ] || die "usage: key <Name> [Name...]"
  local k
  for k in "$@"; do
    _exec press "{\"key\":\"$k\"}" >/dev/null
    sleep "$KEY_DELAY"
  done
  sleep "$KEY_SETTLE"
}

cmd_type() {
  [ "$#" -ge 1 ] || die "usage: type <text>"
  _exec type "{\"text\":\"$*\"}" >/dev/null
  sleep "$KEY_SETTLE"
}

cmd_stop() {
  _exec close >/dev/null 2>&1 || true
  rm -f "$SOCK"
  echo stopped
}

[ "$#" -ge 1 ] || die "usage: headless.sh {launch|capture|expect|key|type|stop} [args]"
sub="$1"
shift
case "$sub" in
launch) cmd_launch "$@" ;;
capture) cmd_capture ;;
expect) cmd_expect "$@" ;;
key) cmd_key "$@" ;;
type) cmd_type "$@" ;;
stop) cmd_stop ;;
*) die "unknown subcommand: $sub" ;;
esac
