#!/usr/bin/env bash
#
# Send keyboard input to the frontmost Ghostty terminal (the magpie_tui
# window launched by screenshot.sh) via Ghostty's AppleScript `send key`.
# Pair with screenshot.sh to drive interactive flows: send keys, then
# re-screenshot to observe the result.
#
# macOS + Ghostty only. Keep your Claude Code session in a DIFFERENT
# terminal app so "front Ghostty window" unambiguously means the TUI.
#
# Usage:   tui/keys.sh <key> [modifiers]
#   <key>        Ghostty key name: a single char ("a", "5"), or a named key
#                ("enter", "space", "left", "right", "up", "down", "escape",
#                "tab", "backspace", ...).
#   [modifiers]  comma-separated: shift,control,option,command
#
# Examples:
#   tui/keys.sh right            # arrow-right
#   tui/keys.sh 4                # focus History panel
#   tui/keys.sh c control        # Ctrl-C
set -euo pipefail

key="${1:?usage: keys.sh <key> [modifiers]}"
mods="${2:-}"
target="focused terminal of selected tab of front window"

if [ -n "$mods" ]; then
  osascript -e "tell application \"Ghostty\" to send key \"$key\" modifiers \"$mods\" to ($target)"
else
  osascript -e "tell application \"Ghostty\" to send key \"$key\" to ($target)"
fi
