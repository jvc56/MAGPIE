#!/bin/zsh
set -euo pipefail

repo=${0:A:h:h}
cd "$repo"

threads=$(getconf _NPROCESSORS_ONLN)
candidate_corpus=tools/depth_crossover_data/candidates-bag50-644x60.log
experiment_klv=CSW24_klv3_ctx400_experiment
experiment_klv_path="data/lexica/${experiment_klv}.klv3"

# Use a distinct name so this temporary experiment never overwrites a local
# production KLV file.
if [[ ! -f "$experiment_klv_path" ]]; then
  cp tools/depth_crossover_data/CSW24_klv3_ctx400.klv3 \
    "$experiment_klv_path"
fi

make BUILD=test_no_pgo_release -j"$threads" magpie_test

run_sweep() {
  local plies=$1
  local node_budget=$2
  local min_iterations=$3
  local log="obj/depth-replication-10m-p${plies}-m5-20260728.log"
  local err="obj/depth-replication-10m-p${plies}-m5-20260728.err"

  if [[ -f "$log" ]] && grep -q '^COMBINED_SWEEP_DONE ' "$log"; then
    echo "already complete: 10M p${plies}"
    return
  fi
  env \
    KLV3_ORACLE_COMBINED_SWEEP_CORPUS=true \
    KLV3_ORACLE_CORPUS_FILES="$candidate_corpus" \
    KLV3_ORACLE_COMBINED_MAX_POSITIONS=644 \
    KLV3_ORACLE_COMBINED_CELL_PAIRS="60:${min_iterations}" \
    KLV3_ORACLE_COMBINED_PLIES="$plies" \
    KLV3_ORACLE_COMBINED_NODE_BUDGET="$node_budget" \
    KLV3_ORACLE_COMBINED_SIMILARITY_CHECK_PLAYS=10 \
    KLV3_ORACLE_COMBINED_POSITIONAL_SCALE=750 \
    KLV3_ORACLE_THREADS="$threads" \
    KLV3_ORACLE_BASELINE_KLV=CSW24 \
    KLV3_ORACLE_CANDIDATE_KLV="$experiment_klv" \
    ./bin/magpie_test klv3oracle >"$log" 2>"$err"
}

run_sweep 2 9999999 5556
run_sweep 4 10000000 3333
run_sweep 6 10000004 2381

disagreements=obj/depth-replication-10m-p246-disagreements-m5-20260728.tsv
python3 tools/build_depth_crossover_oracle_corpus.py \
  --candidate-corpus "$candidate_corpus" \
  --p2-log obj/depth-replication-10m-p2-m5-20260728.log \
  --p4-log obj/depth-replication-10m-p4-m5-20260728.log \
  --p6-log obj/depth-replication-10m-p6-m5-20260728.log \
  --output "$disagreements"

oracle_log=obj/depth-replication-10m-oracle-p10x100k-m5-20260728.log
oracle_err=obj/depth-replication-10m-oracle-p10x100k-m5-20260728.err
if ! [[ -f "$oracle_log" ]] ||
    ! grep -q '^KLV3_NESTED_DONE ' "$oracle_log"; then
  env \
    KLV3_ORACLE_BASELINE_KLV=CSW24 \
    KLV3_ORACLE_CANDIDATE_KLV=CSW24 \
    KLV3_ORACLE_CORPUS_FILES="$disagreements" \
    KLV3_ORACLE_NESTED=true \
    KLV3_ORACLE_OUTER_SAMPLES=100000 \
    KLV3_ORACLE_OUTER_PLIES=10 \
    KLV3_ORACLE_NESTED_SAMPLES=2 \
    KLV3_ORACLE_THREADS="$threads" \
    KLV3_ORACLE_NESTED_MIN_BAG=50 \
    KLV3_ORACLE_CORPUS_MAX_DISAGREEMENTS=0 \
    ./bin/magpie_test klv3oracle >"$oracle_log" 2>"$oracle_err"
fi

grep '^KLV3_NESTED_SUMMARY ' "$oracle_log" | tail -1
grep '^KLV3_NESTED_DONE ' "$oracle_log"
echo "DEPTH_CROSSOVER_10M_DONE"
