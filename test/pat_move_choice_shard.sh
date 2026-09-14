#!/bin/sh
# Runs one patmovechoice comparison as N parallel shards and pools them.
#
#   test/pat_move_choice_shard.sh <spec> <num_shards> <output_prefix>
#
# spec is <baseline>:<candidate>:<seed_base>:<num_positions>:<num_worlds>
# optionally followed by :<bag_lo>:<bag_hi> (see pat_move_choice_run_spec);
# shard outputs go to
# <output_prefix>_<shard>.txt and the pooled result to <output_prefix>.txt.
# Run from the repository root with bin/magpie_test built.
set -e
if [ "$#" -ne 3 ]; then
  echo "usage: $0 <spec> <num_shards> <output_prefix>" >&2
  exit 2
fi
spec="$1"
num_shards="$2"
prefix="$3"
# Split an optional trailing :<bag_lo>:<bag_hi> off the five-field spec so
# the shard fields land in the right place.
base="$(printf '%s' "$spec" | cut -d: -f1-5)"
bag="$(printf '%s' "$spec" | cut -d: -f6-7 -s)"
shard=0
while [ "$shard" -lt "$num_shards" ]; do
  if [ -n "$bag" ]; then
    name="patmovechoice:${base}:${shard}:${num_shards}:${bag}"
  else
    name="patmovechoice:${base}:${shard}:${num_shards}"
  fi
  ./bin/magpie_test "$name" > "${prefix}_${shard}.txt" 2>&1 &
  shard=$((shard + 1))
done
wait
shard=0
files=""
while [ "$shard" -lt "$num_shards" ]; do
  files="$files ${prefix}_${shard}.txt"
  shard=$((shard + 1))
done
# shellcheck disable=SC2086
python3 test/pat_move_choice_pool.py $files | tee "${prefix}.txt"
