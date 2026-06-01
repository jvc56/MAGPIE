#!/usr/bin/env bash
#
# Deprecated shim. Ghostty's AppleScript `send key` does NOT reach the PTY,
# so the original implementation never worked. Input now goes through macOS
# System Events keystrokes via remote.sh. This wrapper forwards to it.
#
#   tui/keys.sh <name...>   ==  tui/remote.sh key <name...>
exec "$(dirname "$0")/remote.sh" key "$@"
