# Style & Conventions

> **Status:** outline — prose to be written.
> **Source of truth:** [CLAUDE.md](https://github.com/jvc56/MAGPIE/blob/main/CLAUDE.md)

## C standard

<!-- NOTE: C99 with two C11 exceptions — _Atomic/<stdatomic.h> and
     static_assert. No other C11+ features without asking. -->

## Braces

<!-- NOTE: always `{}` for if/else if/else, even single statements. No
     exceptions. -->

## No forward declarations

<!-- NOTE: never forward-declare a struct defined in another module's header —
     include the header. Forward decls only for same-file definitions. -->

## Enum constants at file scope

<!-- NOTE: never declare enum constants inside a function; shared ones in
     src/def/*_defs.h, private ones in a file's top anonymous enum. -->

## Naming

<!-- NOTE: avoid terse names; row/col not i/j; `foo_idx` loop vars; `ml` for
     machine letter; no ambiguous single chars. -->

## Equity is millipoints

<!-- NOTE: int32_t millipoints; use the conversion helpers. Cross-link Leaves &
     Equity. -->

## Keep PRs focused

<!-- NOTE: split large multi-concern PRs. -->
