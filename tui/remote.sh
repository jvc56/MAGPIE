#!/usr/bin/env bash
#
# remote.sh — drive magpie_tui in a Ghostty window from outside the terminal.
#
# Encapsulates the working recipe for remote-controlling the TUI one action
# at a time (macOS + Ghostty only):
#   - launch in a Ghostty window with the repo as cwd (so it finds data/);
#   - capture the Ghostty terminal id so input targets that exact surface;
#   - send input via macOS System Events keystrokes (Ghostty's own `send key`
#     never reaches the PTY, and `input text` is focus/timing-flaky);
#   - capture state via the in-app SIGUSR1 PNG dump (see frame_dump.c).
#
# Requires: Ghostty, and Accessibility permission granted to whatever app
# this process runs under (run Claude Code / your shell in iTerm or Terminal
# and grant it Accessibility). Keystrokes go to the frontmost focused app, so
# the launch/focus steps bring the magpie Ghostty surface to the front.
#
# Subcommands:
#   launch [args...]   open a new Ghostty window running magpie_tui [args],
#                      store the session, and screenshot the first frame.
#                      (No args => startup menu. "--watch" => bot game.)
#   shot [out.png]     SIGUSR1 the running instance; copy the PNG to out.
#   type <text>        type literal text into the TUI, then screenshot.
#   key <name...>      send named keys (enter, esc, backspace, tab, space,
#                      up, down, left, right, home, end, del, or a single
#                      char), then screenshot.
#   stop               quit the launched instance.
#
# Session state lives in /tmp/magpie_tui_session.{termid,pid}. The PNG path
# defaults to /tmp/magpie_tui_frame.png.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/bin/magpie_tui"
DUMP="/tmp/magpie_tui_frame.png"
SESS_TERMID="/tmp/magpie_tui_session.termid"
SESS_PID="/tmp/magpie_tui_session.pid"
RENDER_WAIT="${MAGPIE_TUI_RENDER_WAIT:-2.0}"

die() {
  echo "remote.sh: $*" >&2
  exit 1
}

_focus() {
  # Focus the target surface and bring Ghostty frontmost. Must be its own
  # osascript invocation (separate from the System Events call) — focus set
  # in the same script as the keystroke doesn't take effect in time.
  local termid
  termid="$(cat "$SESS_TERMID" 2>/dev/null || true)"
  [ -n "$termid" ] || die "no session; run 'launch' first"
  osascript \
    -e "tell application \"Ghostty\" to focus terminal id \"$termid\"" \
    -e "tell application \"Ghostty\" to activate" >/dev/null 2>&1 || true
  sleep 0.4
}

cmd_shot() {
  local out="${1:-$DUMP}"
  local pid
  pid="$(cat "$SESS_PID" 2>/dev/null || true)"
  [ -n "$pid" ] || die "no session; run 'launch' first"
  kill -0 "$pid" 2>/dev/null || die "instance (pid $pid) is not running"
  rm -f "$DUMP"
  kill -USR1 "$pid"
  local i
  for i in $(seq 1 40); do
    [ -f "$DUMP" ] && break
    sleep 0.1
  done
  [ -f "$DUMP" ] || die "no PNG produced (window too small / no pixel support?)"
  [ "$DUMP" = "$out" ] || cp -f "$DUMP" "$out"
  echo "$out"
}

cmd_launch() {
  [ -x "$BIN" ] || die "$BIN not built — run 'make magpie_tui'"
  local before
  before=" $(pgrep -x magpie_tui | tr '\n' ' ' || true)"
  local argstr="$*"
  local cmdline="$BIN"
  [ -n "$argstr" ] && cmdline="$BIN $argstr"
  local termid
  termid="$(osascript <<EOF
tell application "Ghostty"
  activate
  set cfg to new surface configuration
  set command of cfg to "$cmdline"
  set initial working directory of cfg to "$REPO"
  set w to (new window with configuration cfg)
  return id of focused terminal of selected tab of w
end tell
EOF
)"
  [ -n "$termid" ] || die "Ghostty did not return a terminal id"
  printf '%s' "$termid" >"$SESS_TERMID"

  local pid="" p i
  for i in $(seq 1 80); do
    for p in $(pgrep -x magpie_tui || true); do
      case "$before" in *" $p "*) : ;; *) pid="$p"; break ;; esac
    done
    [ -n "$pid" ] && break
    sleep 0.1
  done
  [ -n "$pid" ] || die "magpie_tui did not start"
  printf '%s' "$pid" >"$SESS_PID"
  sleep "$RENDER_WAIT"
  cmd_shot
}

# Escape a string for embedding inside an AppleScript "..." literal.
_as_escape() {
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

cmd_type() {
  [ "$#" -ge 1 ] || die "usage: type <text>"
  local text="$*"
  _focus
  osascript -e "tell application \"System Events\" to keystroke \"$(_as_escape "$text")\"" >/dev/null
  sleep 0.5
  cmd_shot
}

_keycode() {
  case "$1" in
  enter | return) echo 36 ;;
  esc | escape) echo 53 ;;
  tab) echo 48 ;;
  space) echo 49 ;;
  backspace | bs) echo 51 ;;
  del | delete | forward-delete) echo 117 ;;
  up) echo 126 ;;
  down) echo 125 ;;
  left) echo 123 ;;
  right) echo 124 ;;
  home) echo 115 ;;
  end) echo 119 ;;
  pageup) echo 116 ;;
  pagedown) echo 121 ;;
  *) echo "" ;;
  esac
}

cmd_key() {
  [ "$#" -ge 1 ] || die "usage: key <name> [name...]"
  _focus
  local name code
  for name in "$@"; do
    code="$(_keycode "$name")"
    if [ -n "$code" ]; then
      osascript -e "tell application \"System Events\" to key code $code" >/dev/null
    elif [ "${#name}" -eq 1 ]; then
      osascript -e "tell application \"System Events\" to keystroke \"$(_as_escape "$name")\"" >/dev/null
    else
      die "unknown key: $name"
    fi
    sleep 0.15
  done
  sleep 0.4
  cmd_shot
}

cmd_stop() {
  local pid termid
  pid="$(cat "$SESS_PID" 2>/dev/null || true)"
  termid="$(cat "$SESS_TERMID" 2>/dev/null || true)"
  [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
  # Close the Ghostty window too — killing the process otherwise leaves a
  # lingering "Process exited. Press any key to close the terminal." window.
  if [ -n "$termid" ]; then
    osascript -e "tell application \"Ghostty\" to close terminal id \"$termid\"" >/dev/null 2>&1 || true
  fi
  rm -f "$SESS_TERMID" "$SESS_PID"
  echo "stopped"
}

[ "$#" -ge 1 ] || die "usage: remote.sh {launch|shot|type|key|stop} [args]"
sub="$1"
shift
case "$sub" in
launch) cmd_launch "$@" ;;
shot) cmd_shot "$@" ;;
type) cmd_type "$@" ;;
key) cmd_key "$@" ;;
stop) cmd_stop ;;
*) die "unknown subcommand: $sub" ;;
esac
