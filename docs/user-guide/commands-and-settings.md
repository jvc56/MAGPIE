# Commands & Settings

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/config.c`, `src/impl/exec.c`
> **Reference:** [Commands](../reference/commands.md), [Settings](../reference/settings.md)

## Commands vs. settings

<!-- NOTE: the core mental model. Commands perform an action; settings (prefixed
     `-`) change how actions behave and persist across commands until
     overwritten. Use the README autoplay example. -->

## Positional arguments

<!-- NOTE: some commands take required/optional positional args (e.g.
     `autoplay games 100`). Args apply only to that command. -->

## Shortest unambiguous matching

<!-- NOTE: `generate` = `gen` = `genera`. Show the rule and the ambiguity
     failure mode. -->

## One-character shortcuts

<!-- NOTE: `g` (generate), `s` (shgame), etc. Note these are explicit shortcuts,
     not just prefix matches. -->

## Getting help

<!-- NOTE: `help`, `help <command>`, `help <setting>`. -->

## Saving & loading settings

<!-- NOTE: settings.txt auto-load on startup, auto-save after successful
     non-script commands; disable with `savesettings false`. -->
