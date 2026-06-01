#!/usr/bin/env bash
#
# Capture a PNG screenshot of magpie_tui, off-terminal.
#
# Launches magpie_tui in a NEW Ghostty window via AppleScript (with the repo
# as the working directory, so it finds data/lexica), waits for it to render,
# sends SIGUSR1 to trigger the in-app frame dump (see tui/frame_dump.c), and
# copies the resulting PNG to the requested path.
#
# macOS + Ghostty only. The TUI renders entirely via the terminal graphics
# protocol, so it needs a real graphics-capable terminal — a bare PTY reports
# zero pixel geometry and the dump produces nothing.
#
# Usage:   tui/screenshot.sh [output.png]
# Env:     MAGPIE_TUI_ARGS         args to magpie_tui   (default: --watch)
#          MAGPIE_TUI_RENDER_WAIT  seconds before dump  (default: 2.5)
#          MAGPIE_TUI_CLOSE=1      kill the launched process after capturing
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/bin/magpie_tui"
OUT="${1:-/tmp/magpie_tui_frame.png}"
ARGS="${MAGPIE_TUI_ARGS:---watch}"
DUMP="/tmp/magpie_tui_frame.png" # frame_dump.c's default output path

[ -x "$BIN" ] || {
  echo "screenshot.sh: $BIN not built — run 'make magpie_tui'" >&2
  exit 1
}

# Record existing instances so we can identify the one we launch.
before=" $(pgrep -x magpie_tui | tr '\n' ' ' || true)"

osascript >/dev/null <<EOF
tell application "Ghostty"
  set cfg to new surface configuration
  set command of cfg to "$BIN $ARGS"
  set initial working directory of cfg to "$REPO"
  new window with configuration cfg
end tell
EOF

pid=""
for _ in $(seq 1 80); do
  for p in $(pgrep -x magpie_tui || true); do
    case "$before" in *" $p "*) : ;; *) pid="$p"; break ;; esac
  done
  [ -n "$pid" ] && break
  sleep 0.1
done
[ -n "$pid" ] || {
  echo "screenshot.sh: magpie_tui did not start" >&2
  exit 1
}

sleep "${MAGPIE_TUI_RENDER_WAIT:-2.5}"
rm -f "$DUMP"
kill -USR1 "$pid"

for _ in $(seq 1 40); do
  [ -f "$DUMP" ] && break
  sleep 0.1
done
[ -f "$DUMP" ] || {
  echo "screenshot.sh: no PNG produced (window too small, or no pixel support?)" >&2
  [ "${MAGPIE_TUI_CLOSE:-0}" = 1 ] && kill "$pid" 2>/dev/null || true
  exit 1
}

[ "$DUMP" = "$OUT" ] || cp "$DUMP" "$OUT"
[ "${MAGPIE_TUI_CLOSE:-0}" = 1 ] && kill "$pid" 2>/dev/null || true
echo "$OUT"
