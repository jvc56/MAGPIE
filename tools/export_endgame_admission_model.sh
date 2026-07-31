#!/usr/bin/env bash
set -euo pipefail

# Rebuild the frozen C admission artifact from the exact fit/calibration
# corpora described in notes/TIME_MANAGER.md. The large raw logs are ignored
# build artifacts, so this script names and checks every required input rather
# than silently fitting whichever files happen to be present.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

output="${1:-src/impl/endgame_admission_model_default_data.h}"
hard1="obj/endgame-calibration-hard12to14-depth6-cap60-r1.log"
hard1_tail2="obj/endgame-calibration-hard12to14-tail-pos2-depth6-cap300.log"
hard1_tail33="obj/endgame-calibration-hard12to14-tail-pos33-depth6-cap300.log"
hard1_tail54="obj/endgame-calibration-hard12to14-tail-pos54-depth6-cap300.log"
hard1_cgp="obj/endgame-calibration-heldout-hard12to14-20260729.txt"
hard2="obj/endgame-calibration-hard12to14-rep2-depth6-cap60-r1.log"
hard2_tail40="obj/endgame-calibration-hard12to14-rep2-tail-pos40-depth6-cap300.log"
hard2_tail43="obj/endgame-calibration-hard12to14-rep2-tail-pos43-depth5-cap300.log"
hard2_cgp="obj/endgame-calibration-heldout-hard12to14-rep2-20260729.txt"
new300="obj/endgame-admission-heldout-300-depth5-cap120-20260729.log"
new300_tails="obj/endgame-admission-heldout-300-depth5-tails-cap600-20260729.log"
new300_tail209="obj/endgame-admission-heldout-pos209-depth5-tail-cap1800-20260729.log"
new300_cgp="obj/endgame-admission-heldout-300-20260729.txt"

inputs=(
  "$hard1"
  "$hard1_tail2"
  "$hard1_tail33"
  "$hard1_tail54"
  "$hard1_cgp"
  "$hard2"
  "$hard2_tail40"
  "$hard2_tail43"
  "$hard2_cgp"
  "$new300"
  "$new300_tails"
  "$new300_tail209"
  "$new300_cgp"
)
for input in "${inputs[@]}"; do
  if [[ ! -f "$input" ]]; then
    echo "missing frozen admission input: $input" >&2
    exit 1
  fi
done

python3 tools/analyze_endgame_admission.py \
  --fit-corpus "$hard1,$hard1_tail2,$hard1_tail33,$hard1_tail54" \
  --fit-cgp "$hard1_cgp" \
  --fit-corpus "$hard2,$hard2_tail40,$hard2_tail43" \
  --fit-cgp "$hard2_cgp" \
  --fit-corpus "$new300,$new300_tails,$new300_tail209" \
  --fit-cgp "$new300_cgp" \
  --fit-range : \
  --fit-range : \
  --fit-range 0:150 \
  --calibration-corpus "$new300,$new300_tails,$new300_tail209" \
  --calibration-cgp "$new300_cgp" \
  --calibration-range 150:300 \
  --depths 4,5 \
  --penalty 10 \
  --model ridge \
  --target ratio \
  --metric nodes \
  --confidence 0.99 \
  --export-c-model "$output" \
  --artifact-id csw24-eg-ridge-p99-10t-v1-20260729 \
  --artifact-workers 10 \
  --artifact-tt-fraction 0.05 \
  --artifact-min-enforceable-depth 5

if command -v clang-format >/dev/null 2>&1; then
  clang-format -i "$output"
fi
