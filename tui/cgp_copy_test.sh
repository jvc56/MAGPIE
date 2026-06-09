#!/usr/bin/env bash
#
# cgp_copy_test.sh — quick end-to-end check of the copy-position-as-CGP
# hotkey ('c') and the /copy slash command, headless via termwright.
#
# Verifies: the status-bar "Copied CGP" notice appears, and the clipboard
# (via the TUI's pbcopy fallback) holds a well-formed 4-token CGP.
# The user's clipboard is saved on entry and restored on exit.
set -uo pipefail
export PATH="$HOME/.cargo/bin:$PATH"

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
CLIP_BACKUP="$(mktemp)"
pbpaste >"$CLIP_BACKUP" 2>/dev/null || true
cleanup() {
  "$HL" stop >/dev/null 2>&1 || true
  pbcopy <"$CLIP_BACKUP" 2>/dev/null || true
  rm -f "$CLIP_BACKUP"
}
trap cleanup EXIT

ok() {
  echo "  PASS  $*"
  pass=$((pass + 1))
}
bad() {
  echo "  FAIL  $*"
  fail=$((fail + 1))
}

check_cgp() { # $1 = label
  pbpaste | python3 -c "
import sys, re
cgp = sys.stdin.read().strip()
t = cgp.split(' ')
ok = (len(t) == 4 and t[0].count('/') == 14 and t[1].count('/') == 1 and
      re.fullmatch(r'-?\d+/-?\d+', t[2]) and re.fullmatch(r'\d+', t[3]))
print(('PASS' if ok else 'FAIL') + ' $1: ' + cgp[:90])
sys.exit(0 if ok else 1)
"
}

echo "cgp_copy_test: copy-position hotkey + /copy command"

"$HL" launch >/dev/null
# Start a watch game (W, walk to Start game, Enter) and let it play.
"$HL" key w Down Down Down Down Down Enter
sleep 8

# 1. Hotkey 'c'.
"$HL" key c
sleep 0.5
if "$HL" capture | grep -qF 'Copied CGP'; then
  ok "status-bar notice shown after 'c'"
else
  bad "no 'Copied CGP' notice after 'c'"
fi
if check_cgp "hotkey-c clipboard"; then
  ok "clipboard holds a valid 4-token CGP"
else
  bad "clipboard is not a valid CGP"
fi

# 2. /copy slash command. Wait for the notice to expire first so we can
# tell the second copy apart from the first.
sleep 3
"$HL" key /
"$HL" type "copy"
"$HL" key Enter
sleep 0.5
if "$HL" capture | grep -qF 'Copied CGP'; then
  ok "status-bar notice shown after /copy"
else
  bad "no 'Copied CGP' notice after /copy"
fi
if check_cgp "/copy clipboard"; then
  ok "/copy clipboard holds a valid 4-token CGP"
else
  bad "/copy clipboard is not a valid CGP"
fi

echo "cgp_copy_test: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
