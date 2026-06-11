#!/usr/bin/env bash
#
# move_entry_test.sh — exercises the unified move-entry core
# (tui/move_entry.c) through BOTH input surfaces, headless:
#
#   A. Keyboard board entry: focus Board, Enter to begin (anchors at the
#      open center), type tiles, Space toggles direction (origin-based
#      re-anchor), Enter submits.
#   B. Cell entry: the same pipeline driven through the history cell
#      ("7H AX" typed + Enter/Enter), committing the next turn.
#   C. Board entry with arrows + leading-position navigation +
#      playthrough absorption ("8G U" absorbing POND -> UPOND).
#
# All in annotation mode (deterministic: no bot, racks inferred).
set -uo pipefail
export PATH="$HOME/.cargo/bin:$PATH"

DIR="$(cd "$(dirname "$0")" && pwd)"
HL="$DIR/headless.sh"
: "${MAGPIE_TUI_DA_TRIES:=3}"
: "${MAGPIE_TUI_DA_DELAY:=0.2}"
: "${MAGPIE_TUI_SETTLE:=0.45}"
: "${MAGPIE_TUI_KEY_DELAY:=0.06}"
: "${MAGPIE_TUI_KEY_SETTLE:=0.2}"
export MAGPIE_TUI_DA_TRIES MAGPIE_TUI_DA_DELAY MAGPIE_TUI_SETTLE \
  MAGPIE_TUI_KEY_DELAY MAGPIE_TUI_KEY_SETTLE

pass=0
fail=0
trap '"$HL" stop >/dev/null 2>&1 || true' EXIT

expect() { # <desc> <substring>
  if "$HL" capture | grep -qF "$2"; then
    echo "  PASS  $1"
    pass=$((pass + 1))
  else
    echo "  FAIL  $1 — missing \"$2\""
    "$HL" capture | grep -E '[0-9]+[>.]' | head -4 | cut -c1-100
    fail=$((fail + 1))
  fi
}

"$HL" launch >/dev/null 2>&1
"$HL" key a >/dev/null 2>&1
"$HL" key Enter Enter Enter >/dev/null 2>&1 # annotate setup -> turn 1

echo "A: keyboard board entry"
"$HL" key Escape >/dev/null 2>&1 # close the auto-opened cell editor
"$HL" key 1 >/dev/null 2>&1      # focus Board
"$HL" key Enter >/dev/null 2>&1  # begin board entry (center open -> 8H)
expect "Enter on focused board anchors at 8H" "1. 8H"
"$HL" type "pond" >/dev/null 2>&1
expect "typed tiles build the move" "8H POND"
"$HL" type " " >/dev/null 2>&1
expect "Space toggles to down (origin re-anchor)" "H8 POND"
"$HL" type " " >/dev/null 2>&1
expect "Space toggles back to across" "8H POND"
"$HL" key Enter >/dev/null 2>&1
expect "Enter commits the board-entered move" "1. 8H POND"

echo "B: cell entry through the same core"
"$HL" key 4 >/dev/null 2>&1     # focus History
"$HL" key End >/dev/null 2>&1   # cursor to the pending entry
"$HL" key Enter >/dev/null 2>&1 # step into the MOVE cell
"$HL" type "7h ax" >/dev/null 2>&1
expect "cell-typed move previews" "7H AX"
"$HL" key Enter Enter >/dev/null 2>&1 # MOVE-commit, then RACK-commit
expect "cell-entered move commits" "2. 7H AX"

echo "C: board entry arrows + playthrough absorption"
"$HL" key Escape >/dev/null 2>&1 # close the re-opened editor (turn 3)
"$HL" key 1 >/dev/null 2>&1
"$HL" key Enter >/dev/null 2>&1 # center occupied -> first empty cell (A1)
expect "fallback anchor lands on first empty cell" "1A"
"$HL" key Right Right Right Right Right Right >/dev/null 2>&1 # -> col G
"$HL" key Down Down Down Down Down Down Down >/dev/null 2>&1  # -> row 8
expect "arrows relocate the origin" "8G"
"$HL" type "u" >/dev/null 2>&1
expect "typed tile absorbs played-through POND" "8G UPOND"
"$HL" key Enter >/dev/null 2>&1
expect "playthrough move commits" "3. 8G UPOND"

"$HL" stop >/dev/null 2>&1
echo "----------------------------------------"
echo "move_entry_test: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
