#!/bin/bash
# Record a cycle profile (xctrace CPU Profiler) of one command and export its
# samples for xct.py.
#
# usage: record.sh OUT_PREFIX [--stdin CMDFILE] -- BINARY ARGS...
#
# Writes OUT_PREFIX.trace, OUT_PREFIX.xml (the cpu-profile table) and
# OUT_PREFIX.log. Runs in the current directory, so cd where the command needs
# to run (MAGPIE wants data/ and testdata/ there).
#
# A launched command's output does not reach OUT_PREFIX.log: xctrace keeps it,
# and the log holds only xctrace's messages. To check results, run the same
# work once without the profiler. With --stdin the log has the output.
#
# --stdin CMDFILE feeds CMDFILE to the binary's stdin, for the CLI. xctrace
# can't pass stdin to a launched process, so the binary is started first,
# xctrace attaches to it, and CMDFILE arrives a few seconds later. The
# recording ends when the binary exits, so the profile covers the whole job.
set -u
if [ $# -lt 3 ]; then
  sed -n '2,15p' "$0"
  exit 2
fi
OUT=$1
shift
CMDFILE=
if [ "$1" = "--stdin" ]; then
  CMDFILE=$2
  shift 2
fi
if [ "$1" != "--" ]; then
  echo "record.sh: expected -- before the command" >&2
  exit 2
fi
shift
if [ -e "$OUT.trace" ]; then
  rm -rf "${OUT:?}.trace"
fi
if [ -z "$CMDFILE" ]; then
  xcrun xctrace record --template 'CPU Profiler' --output "$OUT.trace" \
    --launch -- "$@" > "$OUT.log" 2>&1
else
  (sleep 4; cat "$CMDFILE") | "$@" > "$OUT.log" 2>&1 &
  pid=$!
  sleep 1
  # $! is the pipeline's last process, the binary.
  xcrun xctrace record --template 'CPU Profiler' --attach "$pid" \
    --output "$OUT.trace" > "$OUT.xctrace.log" 2>&1
  wait
fi
xcrun xctrace export --input "$OUT.trace" \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="cpu-profile"]' \
  > "$OUT.xml" 2> /dev/null
if [ ! -s "$OUT.xml" ]; then
  echo "record.sh: export of $OUT.trace is empty" >&2
  exit 1
fi
echo "$OUT.xml"
