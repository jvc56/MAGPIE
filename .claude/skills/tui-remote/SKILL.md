---
name: tui-remote
description: >
  Remote-drive and screenshot the magpie_tui notcurses app on macOS. Use when
  asked to open/launch/run the TUI, take a screenshot of it, operate its
  interface one action at a time (press keys, type, navigate menus), or test
  TUI behavior visually. The TUI renders entirely via the terminal graphics
  protocol, so it must run in a real graphics terminal (Ghostty) and is driven
  from outside via AppleScript + System Events, observed via an off-terminal
  PNG dump.
---

# magpie-tui — remote-drive the TUI

The TUI draws its whole UI as graphics-protocol pixels + notcurses cells, so
it can't run in this shell (not a tty) and PTY-grid tools (termwright) are
blind to it. Drive it in a real **Ghostty** window and observe it with the
in-app **SIGUSR1 PNG dump** (frame_dump.c). All of this is wrapped by
`tui/remote.sh`.

## Prerequisites (verify once; if input doesn't land, this is why)
- Build it: `make magpie_tui` (binary at `bin/magpie_tui`).
- **Ghostty** installed (`brew install notcurses freetype` are build deps).
- Input uses macOS **System Events keystrokes**, which require **Accessibility**
  permission for the app this process runs under. Run Claude Code / the shell
  in **iTerm or Terminal** (NOT the Ghostty you're automating) and grant it
  Accessibility in System Settings → Privacy & Security → Accessibility.
- Keystrokes go to the frontmost focused app; `remote.sh` focuses the magpie
  Ghostty surface before each input. Keep your Claude session in a different
  terminal app so "frontmost Ghostty surface" is unambiguously the TUI.

## Driver: `tui/remote.sh`
- `tui/remote.sh launch [args...]` — open a Ghostty window running
  `magpie_tui [args]` (repo as cwd so it finds `data/`), store the session,
  and screenshot the first frame. No args → **startup menu**; `--watch` →
  bot-vs-bot game.
- `tui/remote.sh shot [out.png]` — re-screenshot the current state.
- `tui/remote.sh type <text>` — type literal text, then screenshot.
- `tui/remote.sh key <name...>` — send named keys then screenshot. Names:
  `enter esc tab space backspace del up down left right home end pageup
  pagedown`, or any single character (`a`, `5`, `/`). Multiple allowed:
  `key down down enter`.
- `tui/remote.sh stop` — quit the launched instance.

Every command prints the PNG path (default `/tmp/magpie_tui_frame.png`).

## How to run an action and show the user
1. Run the relevant `tui/remote.sh` subcommand via Bash.
2. **Read** the printed PNG path (so you can see the result and verify it did
   what was intended — re-check before any irreversible step).
3. **SendUserFile** the PNG so it renders inline for the user (desktop/web/iOS;
   the CLI shows a path only).
4. Describe the current screen and ask for / proceed to the next action.

## Notes
- Verify each step's screenshot before committing irreversible actions (e.g.
  pressing Enter to start a game). Clear a prefilled text field with repeated
  `key backspace` before typing.
- `RENDER_WAIT` env (default 2.0s) controls the post-launch settle before the
  first screenshot — raise it if the first frame is mid-render.
- For a one-off throwaway screenshot (launch → shot → close), `tui/screenshot.sh`
  also exists; `remote.sh` is for persistent sessions you drive step by step.
- Why not Ghostty's own AppleScript input? `send key` feeds Ghostty's keybinding
  layer (never reaches the PTY) and `input text` is focus/timing-flaky — only
  System Events reliably delivers keys to the notcurses app.
