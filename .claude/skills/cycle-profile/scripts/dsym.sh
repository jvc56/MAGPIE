#!/bin/bash
# Make BINARY.dSYM for a binary you are about to profile, and refuse a stale one.
#
# usage: dsym.sh BINARY
#
# dsymutil reads the debug information from the object files the binary was
# linked from. After a later build in the same tree overwrites them, it warns
# (a timestamp mismatch, or an object it can't find) and writes a dSYM that
# still carries the binary's UUID but can name the wrong function or line for
# any code that changed. A fresh build gives no warnings, so any warning means
# the dSYM can't be trusted: this deletes it and fails. Copy the binary and run
# this right after building it.
set -u
if [ $# -ne 1 ] || [ ! -f "$1" ]; then
  sed -n '2,12p' "$0"
  exit 2
fi
BIN=$1
OUT=$BIN.dSYM
if [ -e "$OUT" ]; then
  rm -rf "${OUT:?}"
fi
warnings=$(xcrun dsymutil "$BIN" -o "$OUT" 2>&1)
rc=$?
if [ $rc -ne 0 ] || [ -n "$warnings" ]; then
  echo "$warnings" | head -5 >&2
  echo "dsym.sh: dsymutil warned or failed, so $OUT would be stale; rebuild $BIN, copy it and run this again" >&2
  rm -rf "${OUT:?}"
  exit 1
fi
echo "$OUT"
