#!/bin/zsh
set -euo pipefail

repo=${0:A:h:h}
cd "$repo"

duration_seconds=${THINKING_CURVE_DURATION_SECONDS:-25200}
run_id=${THINKING_CURVE_RUN_ID:-m4-20260728}
max_nodes=${THINKING_CURVE_MAX_NODES:-10000000}
chunk_positions=${THINKING_CURVE_CHUNK_POSITIONS:-8}
total_positions=${THINKING_CURVE_TOTAL_POSITIONS:-644}
source_corpus=${THINKING_CURVE_SOURCE_CORPUS:-../MAGPIE-positional/tools/depth_crossover_data/candidates-bag50-644x60.log}
corpus=obj/thinking-curves-candidates-bag50-644x60.log
log=obj/thinking-curves-${run_id}.log
err=obj/thinking-curves-${run_id}.err
summary=obj/thinking-curves-${run_id}-summary.txt
csv=obj/thinking-curves-${run_id}.csv

mkdir -p obj
if [[ ! -f "$corpus" ]]; then
  cp "$source_corpus" "$corpus"
fi

threads=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.logicalcpu)
make BUILD=no_pgo_release -j"$threads" magpie_test

start_epoch=$(date +%s)
deadline_epoch=$((start_epoch + duration_seconds))
print -r -- "THINKING_CURVE_RUN_START run_id=$run_id start_epoch=$start_epoch duration_seconds=$duration_seconds threads=$threads max_nodes=$max_nodes" >>"$log"

next_skip_for_plies() {
  local requested_plies=$1
  awk -v requested_plies="$requested_plies" '
    $1 == "THINKING_CURVE_POSITION_DONE" {
      p = -1
      s = -1
      for (i = 2; i <= NF; i++) {
        split($i, pair, "=")
        if (pair[1] == "plies") p = pair[2] + 0
        if (pair[1] == "source_index") s = pair[2] + 0
      }
      if (p == requested_plies && s + 1 > max_skip) max_skip = s + 1
    }
    END { print max_skip + 0 }
  ' "$log"
}

while (( $(date +%s) < deadline_epoch )); do
  made_progress=false
  for plies in 2 4 6; do
    now=$(date +%s)
    remaining=$((deadline_epoch - now))
    if (( remaining <= 0 )); then
      break
    fi
    skip=$(next_skip_for_plies "$plies")
    if (( skip >= total_positions )); then
      continue
    fi
    made_progress=true
    THINKING_CURVE_CORPUS="$corpus" \
      THINKING_CURVE_SKIP_POSITIONS="$skip" \
      THINKING_CURVE_MAX_POSITIONS="$chunk_positions" \
      THINKING_CURVE_NUM_PLAYS=60 \
      THINKING_CURVE_PLIES="$plies" \
      THINKING_CURVE_THREADS="$threads" \
      THINKING_CURVE_MAX_NODES="$max_nodes" \
      THINKING_CURVE_CHECKPOINT_NODES=25000 \
      THINKING_CURVE_MIN_PLAY_ITERATIONS=100 \
      THINKING_CURVE_JUDGE_PLIES=10 \
      THINKING_CURVE_JUDGE_SAMPLES=100000 \
      THINKING_CURVE_WALL_SECONDS="$remaining" \
      ./bin/magpie_test thinkingcurve >>"$log" 2>>"$err"
    python3 tools/analyze_thinking_curves.py "$log" --csv "$csv" >"$summary"
  done
  if [[ "$made_progress" == false ]]; then
    break
  fi
done

python3 tools/analyze_thinking_curves.py "$log" --csv "$csv" >"$summary"
end_epoch=$(date +%s)
print -r -- "THINKING_CURVE_RUN_DONE run_id=$run_id end_epoch=$end_epoch elapsed_seconds=$((end_epoch - start_epoch))" >>"$log"
