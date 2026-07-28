#!/bin/zsh
set -euo pipefail

repo=/Users/olaugh/sources/jul21-magpie/MAGPIE-positional
candidate_corpus=obj/combined-candidates-terminal-oracle-bag50-644x60x256-fixed-20260728.log
threads=10
oracle_plies=10
oracle_samples=100000

cd "$repo"

run_sweep() {
  local budget_label=$1
  local plies=$2
  local node_budget=$3
  local min_iterations=$4
  local log="obj/depth-replication-${budget_label}-p${plies}-20260728.log"
  local err="obj/depth-replication-${budget_label}-p${plies}-20260728.err"

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
    KLV3_ORACLE_CANDIDATE_KLV=CSW24_klv3_ctx400 \
    ./bin/magpie_test klv3oracle >"$log" 2>"$err"
}

run_oracle() {
  local budget_label=$1
  local corpus="obj/depth-replication-${budget_label}-p246-disagreements-20260728.tsv"
  local log="obj/depth-replication-${budget_label}-oracle-p${oracle_plies}x${oracle_samples}-20260728.log"
  local err="obj/depth-replication-${budget_label}-oracle-p${oracle_plies}x${oracle_samples}-20260728.err"

  python3 tools/build_depth_crossover_oracle_corpus.py \
    --candidate-corpus "$candidate_corpus" \
    --p2-log "obj/depth-replication-${budget_label}-p2-20260728.log" \
    --p4-log "obj/depth-replication-${budget_label}-p4-20260728.log" \
    --p6-log "obj/depth-replication-${budget_label}-p6-20260728.log" \
    --output "$corpus"

  env \
    KLV3_ORACLE_BASELINE_KLV=CSW24 \
    KLV3_ORACLE_CANDIDATE_KLV=CSW24 \
    KLV3_ORACLE_CORPUS_FILES="$corpus" \
    KLV3_ORACLE_NESTED=true \
    KLV3_ORACLE_OUTER_SAMPLES="$oracle_samples" \
    KLV3_ORACLE_OUTER_PLIES="$oracle_plies" \
    KLV3_ORACLE_NESTED_SAMPLES=2 \
    KLV3_ORACLE_THREADS="$threads" \
    KLV3_ORACLE_NESTED_MIN_BAG=50 \
    KLV3_ORACLE_CORPUS_MAX_DISAGREEMENTS=0 \
    ./bin/magpie_test klv3oracle >"$log" 2>"$err"
}

# Establish the sufficient-budget result first, then measure the low-budget
# side of the crossover, and finally check whether the effect grows at 3M.
run_sweep 1m 2 999999 556
run_sweep 1m 4 1000000 333
run_sweep 1m 6 1000006 238
run_oracle 1m

run_sweep 300k 2 300000 167
run_sweep 300k 4 300000 100
run_sweep 300k 6 299999 71
run_oracle 300k

run_sweep 3m 2 3000000 1667
run_sweep 3m 4 3000000 1000
run_sweep 3m 6 3000004 714
run_oracle 3m

echo "DEPTH_CROSSOVER_REPLICATION_DONE"
