#!/usr/bin/env bash
#
# annotate_rack_test.sh — invariant smoke test for the annotation rack
# display: the on-turn player pill and the pending history cell's rack row
# must always show the SAME rack. Both now read one shared selection,
# tui_game_state_effective_editor_rack, so they can't drift; this guards
# against that helper (or its callers) regressing.
#
# Note: the historical divergence was specifically the carryover-leave case
# (the old renderer lacked that branch). Reproducing it needs a multi-turn
# game (a player's leave carried to their next turn), which is unreliable to
# drive with blind scripted keystrokes, so these scenarios assert the
# invariant in single-turn states rather than reproducing the carryover bug.
#
# Drives a few annotation states headless and asserts pill == cell. Exits 0
# if all pass, 1 otherwise.
set -uo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
HL="$DIR/headless.sh"
: "${MAGPIE_TUI_DA_TRIES:=3}"
: "${MAGPIE_TUI_DA_DELAY:=0.2}"
: "${MAGPIE_TUI_SETTLE:=0.45}"
: "${MAGPIE_TUI_KEY_DELAY:=0.06}"
: "${MAGPIE_TUI_KEY_SETTLE:=0.15}"
export MAGPIE_TUI_DA_TRIES MAGPIE_TUI_DA_DELAY MAGPIE_TUI_SETTLE \
  MAGPIE_TUI_KEY_DELAY MAGPIE_TUI_KEY_SETTLE

pass=0
fail=0
CAP=""
trap '"$HL" stop >/dev/null 2>&1 || true' EXIT

# Pull the on-turn (▶) player's rack out of the header line.
pill_rack() {
  printf '%s\n' "$CAP" | grep -F '▶' |
    sed -E 's/.*Player [0-9]+ +([A-Z?]+).*/\1/' | head -1
}
# Pull the rack row of the pending ("N>") history entry: it's the line right
# after the "N>" line; take its first run of rack letters.
cell_rack() {
  local n
  n="$(printf '%s\n' "$CAP" | grep -nE '[0-9]+>' | head -1 | cut -d: -f1)"
  [ -n "$n" ] || return 0
  printf '%s\n' "$CAP" | sed -n "$((n + 1))p" |
    grep -oE '[A-Z?]{2,}' | head -1
}

check() { # <desc>
  local desc="$1" p c
  p="$(pill_rack)"
  c="$(cell_rack)"
  if [ -n "$p" ] && [ "$p" = "$c" ]; then
    echo "  PASS  $desc — pill==cell ($p)"
    pass=$((pass + 1))
  else
    echo "  FAIL  $desc — pill='$p' cell='$c'"
    fail=$((fail + 1))
  fi
}

enter_annotation() {
  "$HL" launch >/dev/null 2>&1
  "$HL" key a >/dev/null 2>&1
  "$HL" key Enter Enter Enter >/dev/null 2>&1 # commit setup -> turn 1
}

echo "scenario 1: valid opening move infers a rack"
enter_annotation
"$HL" type "8h phone" >/dev/null 2>&1
CAP="$("$HL" capture)"
check "8H PHONE inferred rack"
"$HL" stop >/dev/null 2>&1

echo "scenario 2: invalid through-tile move (original bug trigger)"
enter_annotation
"$HL" type "j8 ode" >/dev/null 2>&1
CAP="$("$HL" capture)"
check "j8 ode inferred rack"
"$HL" stop >/dev/null 2>&1

echo "scenario 3: user-typed rack then a move, focus back on move"
enter_annotation
"$HL" key Tab >/dev/null 2>&1
"$HL" type "ab" >/dev/null 2>&1
"$HL" key Tab >/dev/null 2>&1
"$HL" type "8h phone" >/dev/null 2>&1
CAP="$("$HL" capture)"
check "typed rack + move"
"$HL" stop >/dev/null 2>&1

echo "----------------------------------------"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
