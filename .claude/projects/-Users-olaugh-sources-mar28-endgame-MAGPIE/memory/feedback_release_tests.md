---
name: Use BUILD=release for tests
description: Always use BUILD=release when running magpie_test; dev build is too slow with ASAN
type: feedback
---

Always run tests with `BUILD=release` — dev build (`-O0` + ASAN/UBSAN) is very slow.

**Why:** The CLAUDE.md says "Use BUILD=release for anything beyond a few seconds." The user corrected me when I tried to run endgame tests with the dev build.

**How to apply:** When building and running `magpie_test`, always use `make magpie_test BUILD=release` and `./bin/magpie_test <test>`.
