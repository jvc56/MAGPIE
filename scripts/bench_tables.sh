#!/usr/bin/env bash
# What each derived table (wordmap, word info table, rack info table) is worth
# to a birdtest contributor's games: plays a fixed set of CSW24 games with a
# static player and with a 2-ply simmer under no tables, WIT alone, WMP,
# WMP+WIT, WMP+RIT and all three, and prints move generations a second and
# seconds a game for each. The moves and the move generations are the same
# under every set; only the time differs. contribute_choose_tables keeps the
# tables in the order these numbers give when a contributor's disk cannot hold
# them all.
#
# Needs data/lexica/CSW24.{kwg,klv2,wmp,wit,rit} (see AGENTS.md; `make
# prepare_data` builds the WIT and RIT). Runs on four cores for a few minutes.
#
#   scripts/bench_tables.sh [static_games] [sim_games]
set -euo pipefail
cd "$(dirname "$0")/.."

static_games="${1:-4000}"
sim_games="${2:-20}"
cores="${BENCH_CORES:-0-3}"

for table in wmp wit rit; do
  if [ ! -f "data/lexica/CSW24.${table}" ]; then
    echo "missing data/lexica/CSW24.${table}" >&2
    exit 1
  fi
done

if [ ! -x bin/magpie_test ] || [ "${BENCH_REBUILD:-1}" = 1 ]; then
  make magpie_test BUILD=test_no_pgo_release -j2 >/dev/null
fi

TABLE_BENCH_STATIC_GAMES="$static_games" TABLE_BENCH_SIM_GAMES="$sim_games" \
  taskset -c "$cores" ./bin/magpie_test tablebench |
  grep -E "^(player|static|sim2ply) "
