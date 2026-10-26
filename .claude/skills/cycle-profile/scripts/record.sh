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
set -u
if [ $# -lt 3 ]; then
  sed -n '2,18p' "$0"
  exit 2
fi
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
if [ -e "$OUT.trace" ]; then
  rm -rf "${OUT:?}.trace"
fi
xcrun xctrace record --template "$TEMPLATE" --output "$OUT.trace" \
  --target-stdout "$OUT.out" ${STDIN_ARGS[@]+"${STDIN_ARGS[@]}"} \
  --launch -- "$@" > "$OUT.log" 2>&1
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
