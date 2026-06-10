#!/usr/bin/env bash
#
# play_vs_computer_test.sh — on-demand end-to-end test: play a FULL game in
# the TUI's human-vs-computer mode, with the "human" side driven by the
# console engine's static (equity) evaluation.
#
# Loop, per human turn:
#   1. Press 'c' in the TUI — the copy-position hotkey puts the current
#      position on the system clipboard as CGP (via the TUI's pbcopy
#      fallback, which works headless where OSC 52 has no real terminal).
#      The computer's rack is concealed in the CGP; movegen only needs the
#      on-turn (human) rack, which is fully present.
#   2. Feed the CGP to `bin/magpie` (console) and `generate`; take the
#      best PLACEMENT row — that's the static-eval pick. (Exchange/pass
#      rows are skipped: the TUI's play-vs-computer scope is placement
#      moves only for now.)
#   3. Enter it through the pending history cell: focus History, End (jump
#      to the pending entry), Enter (open the MOVE field), type the play,
#      Enter — the cell editor commits through the same path as board
#      entry: real bag draw, clock charge, bot handoff.
#   4. Wait for the computer's reply; repeat until game over.
#
# Notes:
#   - The play is typed in lowercase: termwright `type` sends uppercase as
#     shifted keys, and Shift+letter means "blank" to the move editor. The
#     engine's blank designations (lowercase in console output) survive via
#     the play-vs-computer auto-blank rule (letter not on rack + blank
#     available => blank). Known edge: a play that blanks a letter the rack
#     ALSO holds a real copy of will consume the real tile instead; the
#     score may then differ from the console's pick. Rare; acceptable here.
#   - macOS only (pbpaste). The user's clipboard is saved and restored.
#
# Requires: termwright, python3, bin/magpie + bin/magpie_tui built, data/
# for the chosen lexicon.
set -uo pipefail
export PATH="$HOME/.cargo/bin:$PATH"

DIR="$(cd "$(dirname "$0")" && pwd)"
HL="$DIR/headless.sh"
BIN_CONSOLE="$DIR/../bin/magpie"
LEXICON="${MAGPIE_PVC_TEST_LEXICON:-CSW24}"
MAX_TURNS="${MAGPIE_PVC_TEST_MAX_TURNS:-40}"

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

note() { echo "  $*"; }
ok() {
  echo "  PASS  $*"
  pass=$((pass + 1))
}
bad() {
  echo "  FAIL  $*"
  fail=$((fail + 1))
}

cap() { "$HL" capture; }

# The on-turn player pill is marked with ▶; the human was named "You" in
# the setup modal. Both pills render on ONE line, so grep the name
# directly after the marker rather than anywhere on the line.
human_on_turn() {
  cap | grep -oE '▶ *[A-Za-z]+' | head -1 | grep -qF 'You'
}

# The Analysis panel ([5]) is hidden for the whole live play-vs-computer
# game and reappears at game over — use it as the game-over signal.
game_is_over() {
  cap | grep -qF '[5]'
}

# Highest committed history-entry number on screen ("N." labels; the
# pending entry renders "N>" instead). Monotonic over the game, so it's
# a race-free acceptance signal: after our Enter the human entry commits
# immediately, growing the count — no matter how fast the bot replies.
max_committed() {
  cap | grep -oE '[0-9]+\.' | tr -d '.' | sort -n | tail -1
}

# Static-eval pick: best placement row from `generate` (numplays 15 so a
# top-ranked exchange/pass doesn't leave us empty-handed). Echoes the play
# in cell-editor typing form: lowercase = real tiles (the editor folds
# them up), UPPERCASE = blank designation (Shift+letter). Blanks appear
# lowercase in console output, so the word is case-SWAPPED. Because a
# legacy PTY may not deliver the shift modifier for uppercase letters,
# prefer the best blank-free placement and only fall back to a blank play
# when nothing else is available.
static_eval_pick() { # $1 = cgp string
  printf 'set -mode sync -lex %s -wmp true -numplays 15\ncgp %s\ngenerate\nquit\n' \
    "$LEXICON" "$1" |
    "$BIN_CONSOLE" 2>/dev/null |
    python3 -c "
import sys, re

def typed_form(coord, word):
    # Parenthesized runs are played-through board tiles. The cell
    # editor AUTO-ABSORBS interior/trailing runs as you type (autofill
    # appends board letters at the word's END), so typing those again
    # would duplicate them ('PAN(E)LED' typed verbatim becomes
    # PANEELED) — drop them and let autofill pad the buffer. A LEADING
    # run is different: autofill never prepends, and the engine wants
    # the canonical form spelled from the coord — so leading board
    # letters are typed verbatim (typing onto an occupied square with
    # the matching letter parses as playthrough, no duplication).
    m = re.match(r'^\(([^)]*)\)(.*)$', word)
    lead = m.group(1) if m else ''
    rest = m.group(2) if m else word
    return coord.lower(), lead + re.sub(r'\([^)]*\)', '', rest)

# Move rows render as a column to the RIGHT of the board art, so a row
# shares its line with board glyphs / bag text. Regex out the
# 'N: <coord> <word>' fragment instead of relying on token position.
row_re = re.compile(
    r'(?:^|\s)\d+:\s+(\d{1,2}[A-Oa-o]|[A-Oa-o]\d{1,2})\s+(\S+)')
for line in sys.stdin:
    m = row_re.search(line)
    if m:
        # Rank-1 placement. Typed all-lowercase: the TUI's
        # play-vs-computer auto-blank rule resolves blanks (a letter
        # the rack doesn't hold becomes a blank when one is
        # available), so no Shift / case gymnastics needed. Known
        # divergence: when the rack holds BOTH the real tile and a
        # blank, the TUI consumes the real tile even if the console's
        # pick used the blank — same placement, different leave/score.
        coord, word = typed_form(m.group(1), m.group(2))
        print(coord + ' ' + word.lower())
        break
"
}

echo "play_vs_computer_test: full static-eval game (lexicon $LEXICON)"

# ── Launch + start a play-vs-computer game, human moving first ─────────
"$HL" launch >/dev/null
"$HL" key c # startup menu: C = Play against the computer
sleep 0.3
# The play-setup modal opens focused on "Start game" (row 8). Walk up to
# "First move" (row 2), cycle Random -> Human, then back down to Start.
"$HL" key Up Up Up Up Up Up
"$HL" key Right
if cap | grep -qE 'First move.*Human'; then
  ok "first-move set to Human"
else
  bad "could not set first-move to Human"
  cap | grep -i 'first move' || true
fi
"$HL" key Down Down Down Down Down Down
"$HL" key Enter
sleep 1

# ── Game loop ───────────────────────────────────────────────────────────
turns=0
finished=0
while [ "$turns" -lt "$MAX_TURNS" ]; do
  # Wait (up to ~60s) for the human turn or game over. The computer's
  # think time is bounded by the sim params from the setup modal.
  waited=0
  while ! human_on_turn; do
    if game_is_over; then
      finished=1
      break
    fi
    sleep 1
    waited=$((waited + 1))
    if [ "$waited" -ge 60 ]; then
      bad "timed out waiting for human turn (after $turns human turns)"
      break
    fi
  done
  if [ "$finished" -eq 1 ] || [ "$waited" -ge 60 ]; then
    break
  fi

  # 1. Copy the position as CGP (global hotkey, any panel focus).
  "$HL" key c
  sleep 0.4
  CGP="$(pbpaste)"
  ntok="$(printf '%s' "$CGP" | awk '{print NF}')"
  if [ "$ntok" != "4" ]; then
    bad "turn $turns: clipboard is not a 4-token CGP: '$CGP'"
    break
  fi

  # 2. Static-eval pick from the console engine.
  PLAY="$(static_eval_pick "$CGP")"
  if [ -z "$PLAY" ]; then
    bad "turn $turns: no placement among the top static-eval plays"
    break
  fi
  note "turn $turns: static-eval pick: $PLAY"

  # 3. Enter the play through the pending history cell.
  committed_before="$(max_committed)"
  : "${committed_before:=0}"
  "$HL" key 4     # focus History
  "$HL" key End   # jump cursor to the pending (newest) entry
  "$HL" key Enter # step into the MOVE field
  "$HL" type "$PLAY"
  "$HL" key Enter # play-vs-computer commit: draw, clock, bot handoff
  sleep 0.5

  # On a successful commit the human's pending entry finalizes, growing
  # the highest committed entry number. Poll the count rather than the
  # on-turn marker: a fast bot reply can hand the turn straight back to
  # the human between samples, which fooled the turn-flip check. The
  # window is generous — the typed burst is validated per keystroke and
  # the Enter queues behind it, so the commit can land many seconds
  # after the keys were sent.
  accepted=0
  for poll in $(seq 1 60); do
    committed_now="$(max_committed)"
    : "${committed_now:=0}"
    if [ "$committed_now" -gt "$committed_before" ] || game_is_over; then
      accepted=1
      break
    fi
    sleep 0.5
  done
  if [ "$accepted" -ne 1 ]; then
    bad "turn $turns: play '$PLAY' was not accepted (still human's turn)"
    echo "  --- CGP: $CGP"
    echo "  --- raw console move rows ---"
    printf 'set -mode sync -lex %s -wmp true -numplays 15\ncgp %s\ngenerate\nquit\n' \
      "$LEXICON" "$CGP" | "$BIN_CONSOLE" 2>/dev/null |
      grep -oE '[0-9]+:\s+\S+\s+\S+.*' | head -6
    echo "  --- pending cell at rejection ---"
    cap | grep -E '[0-9]+>' -A 1 | cut -c34-130 | head -4
    echo "  --- history panel at rejection ---"
    cap | sed -n '1,8p' | cut -c34-130
    break
  fi
  ok "turn $turns: played $PLAY"
  turns=$((turns + 1))
done

if game_is_over; then
  ok "game completed after $turns human turns"
else
  bad "game did not finish (played $turns human turns)"
fi

echo "play_vs_computer_test: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
