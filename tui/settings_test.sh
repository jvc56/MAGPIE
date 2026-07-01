#!/usr/bin/env bash
#
# settings_test.sh — headless tests that settings load from --config and show
# up in the UI. For each config variant it launches magpie_tui headless
# (tui/headless.sh), opens the Settings modal, and asserts the displayed
# values. Fully isolated: every launch uses --config <tmpfile>, so the real
# ~/.config/magpie/tui.toml is never read or written.
#
# Exits 0 if all assertions pass, 1 otherwise. Prints a timing summary.
#
# Note: the headless text grid sees notcurses CELL text only — the pixel
# board/rack tiles are invisible here (that's the PNG dump's job). So we
# assert on the Settings modal, which renders each value as text. The
# 2x-only rows (antialias, score subscripts, border) are hidden when pixel
# graphics are unsupported (always, headless), so we test the rows that show:
# Premium, Blanks, Rack sort.
set -uo pipefail

# Faster-than-default headless timings (still with render-settle headroom).
# Override any of these in the environment for slower/faster machines.
: "${MAGPIE_TUI_DA_TRIES:=3}"
: "${MAGPIE_TUI_DA_DELAY:=0.2}"
: "${MAGPIE_TUI_SETTLE:=0.45}"
: "${MAGPIE_TUI_KEY_DELAY:=0.06}"
: "${MAGPIE_TUI_KEY_SETTLE:=0.15}"
export MAGPIE_TUI_DA_TRIES MAGPIE_TUI_DA_DELAY MAGPIE_TUI_SETTLE \
  MAGPIE_TUI_KEY_DELAY MAGPIE_TUI_KEY_SETTLE

DIR="$(cd "$(dirname "$0")" && pwd)"
HL="$DIR/headless.sh"
OVR="$(mktemp -t magpie_settings_test).toml"
CAP=""
pass=0
fail=0

cleanup() {
  "$HL" stop >/dev/null 2>&1 || true
  rm -f "$OVR"
}
trap cleanup EXIT

# open_settings <config-body> — write config, launch headless, dismiss the
# startup menu, open Settings, and capture the screen into $CAP.
open_settings() {
  printf '%s\n' "$1" >"$OVR"
  "$HL" launch --config "$OVR" >/dev/null 2>&1
  "$HL" key Escape >/dev/null 2>&1 # dismiss startup menu -> base view
  "$HL" key s >/dev/null 2>&1      # open Settings
  CAP="$("$HL" capture)"
  "$HL" stop >/dev/null 2>&1
}

# assert_row <desc> <row-label> <value-substring>
# Checks that the modal line containing <row-label> also contains <value>.
# grep -F throughout so values with regex metachars (vow+con+?) are literal.
assert_row() {
  local desc="$1" label="$2" value="$3" line
  line="$(printf '%s\n' "$CAP" | grep -F -- "$label" | head -1)"
  if printf '%s' "$line" | grep -qF -- "$value"; then
    echo "  PASS  $desc"
    pass=$((pass + 1))
  else
    echo "  FAIL  $desc — '$label' line missing '$value'"
    echo "        got: $(printf '%s' "$line" | tr -s ' ')"
    fail=$((fail + 1))
  fi
}

COMMON='theme = "dark"
lexicon = "CSW24"
time_per_side_seconds = 900'

start=$(date +%s)

echo "variant A: premium=none, rack_sort=vowels, blank_uppercase=true"
open_settings "[tui]
$COMMON
premium_labels = \"none\"
rack_sort = \"vowels\"
blank_uppercase = true"
assert_row "premium = none"        "Premium"   "none"
assert_row "rack_sort = vow+con+?" "Rack sort" "vow+con+?"
assert_row "blanks = uppercase"    "Blanks"    "uppercase"
assert_row "scale unsupported"     "Scale"     "unsupported"

echo "variant B: premium=uppercase, rack_sort=alpha, blank_uppercase=false"
open_settings "[tui]
$COMMON
premium_labels = \"uppercase\"
rack_sort = \"alpha\"
blank_uppercase = false"
assert_row "premium = uppercase" "Premium"   "uppercase"
assert_row "rack_sort = alpha+?" "Rack sort" "alpha+?"
assert_row "blanks = lowercase"  "Blanks"    "lowercase"

echo "variant C: premium=lowercase, rack_sort=blanks_alpha"
open_settings "[tui]
$COMMON
premium_labels = \"lowercase\"
rack_sort = \"blanks_alpha\""
assert_row "premium = lowercase" "Premium"   "lowercase"
assert_row "rack_sort = ?+alpha" "Rack sort" "?+alpha"

end=$(date +%s)
echo "----------------------------------------"
echo "$pass passed, $fail failed in $((end - start))s"
[ "$fail" -eq 0 ]
