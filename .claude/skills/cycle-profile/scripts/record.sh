#!/bin/bash
# Record a profile (xctrace) of one command and export it for xct.py.
#
# usage: record.sh OUT_PREFIX [--template T] [--stdin CMDFILE] -- BINARY ARGS...
#
# T is an xctrace template name or path: 'CPU Profiler' (the default) samples
# cycles; a template from counters_template.py records CPU Counters.
# --stdin CMDFILE feeds CMDFILE to the command's standard input, for the CLI.
# Runs in the current directory, so cd where the command needs to run (MAGPIE
# wants data/ and testdata/ there).
#
# Writes OUT_PREFIX.trace, OUT_PREFIX.out (the command's output),
# OUT_PREFIX.log (xctrace's messages) and the export for xct.py:
# - CPU Profiler: OUT_PREFIX.xml, the cpu-profile table;
# - a CPU Counters sampling mode: OUT_PREFIX.xml, the pmi-sample table, and
#   OUT_PREFIX.time.xml, the time-profile table, which gives the binaries'
#   load addresses;
# - CPU Counters' bottlenecks mode: OUT_PREFIX.xml, the per-process metrics.
#
# xctrace writes a recording's raw kernel trace to the user temp folder, as
# instruments*.ktrace, and deletes it once the .trace is written. A recording
# that fails or is interrupted leaves it there, and a CPU Counters one can be
# tens of GB. This script deletes the ones its recording leaves, and refuses
# to start with less than MIN_FREE_GB free there.
set -u
if [ $# -lt 3 ]; then
  sed -n '2,24p' "$0"
  exit 2
fi
MIN_FREE_GB=20
KTRACE_DIR=$(getconf DARWIN_USER_TEMP_DIR)
OUT=$1
shift
TEMPLATE='CPU Profiler'
STDIN_ARGS=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  case $1 in
    --template) TEMPLATE=$2; shift 2 ;;
    --stdin) STDIN_ARGS=(--target-stdin "$2"); shift 2 ;;
    *) echo "record.sh: unknown option $1" >&2; exit 2 ;;
  esac
done
if [ $# -lt 2 ]; then
  echo "record.sh: expected -- and the command" >&2
  exit 2
fi
shift
free_gb=$(df -g "$KTRACE_DIR" | awk 'NR == 2 { print $4 }')
if [ "$free_gb" -lt "$MIN_FREE_GB" ]; then
  echo "record.sh: only ${free_gb} GB free for xctrace's raw trace in" \
    "$KTRACE_DIR; a recording can take tens of GB. Look there for" \
    "instruments*.ktrace left by earlier recordings." >&2
  exit 1
fi
if [ -e "$OUT.trace" ]; then
  rm -rf "${OUT:?}.trace"
fi

# The raw traces from before this recording, which belong to others.
ktraces_before=$(ls "$KTRACE_DIR" 2> /dev/null | grep '^instruments.*\.ktrace$')
xctrace_pid=
remove_new_ktraces() {
  local path
  for path in "$KTRACE_DIR"/instruments*.ktrace; do
    [ -e "$path" ] || continue
    if grep -qxF "$(basename "$path")" <<< "$ktraces_before"; then
      continue
    fi
    # Still open: another recording is writing it.
    if lsof -t "$path" > /dev/null 2>&1; then
      continue
    fi
    rm -f "$path"
  done
}
stop_recording() {
  if [ -n "$xctrace_pid" ]; then
    kill -INT "$xctrace_pid" 2> /dev/null
    wait "$xctrace_pid" 2> /dev/null
  fi
  exit 130
}
trap remove_new_ktraces EXIT
trap stop_recording INT TERM

# In the background, so that an interrupt can stop xctrace before the
# cleanup runs.
xcrun xctrace record --template "$TEMPLATE" --output "$OUT.trace" \
  --target-stdout "$OUT.out" ${STDIN_ARGS[@]+"${STDIN_ARGS[@]}"} \
  --launch -- "$@" > "$OUT.log" 2>&1 &
xctrace_pid=$!
wait "$xctrace_pid"
xctrace_pid=
toc=$(xcrun xctrace export --input "$OUT.trace" --toc 2> /dev/null)
export_table() {
  xcrun xctrace export --input "$OUT.trace" \
    --xpath "/trace-toc/run[@number=\"1\"]/data/table[@schema=\"$1\"]" \
    > "$2" 2> /dev/null
  if [ ! -s "$2" ]; then
    echo "record.sh: export of $1 from $OUT.trace is empty" >&2
    exit 1
  fi
}
if grep -q 'schema="cpu-profile"' <<< "$toc"; then
  export_table cpu-profile "$OUT.xml"
elif grep -q 'schema="pmi-sample"' <<< "$toc"; then
  export_table pmi-sample "$OUT.xml"
  export_table time-profile "$OUT.time.xml"
elif grep -q 'schema="MetricAggregationForProcess"' <<< "$toc"; then
  export_table MetricAggregationForProcess "$OUT.xml"
else
  echo "record.sh: $OUT.trace has no table xct.py reads; see $OUT.log" >&2
  exit 1
fi
echo "$OUT.xml"
