#!/bin/sh
# Overnight PAT experiments. Every step logs to /tmp/overnight and appends a
# line to SUMMARY.txt; a failing step does not stop the rest.
cd /Users/olaugh/sources/magpie-pat-setup-value || exit 1
OUT=/tmp/overnight
DATA=/Users/olaugh/sources/magpie-pr-20260910/data/strategy
SUM=$OUT/SUMMARY.txt
log() { echo "$(date '+%H:%M') $*" >> $SUM; }
compare() { # name spec [shards]
  name=$1; spec=$2; shards=${3:-10}
  test/pat_move_choice_shard.sh "$spec" "$shards" "$OUT/$name" > "$OUT/$name.log" 2>&1
  log "$name: $(grep -h 'paired effect' $OUT/$name.txt 2>/dev/null | head -1) | $(grep -h 'disagreements' $OUT/$name.txt | head -1 | sed -E 's/.*considered, //')"
}
wholegame() { # name pat1 pat2 seed pairs
  rm -f settings.txt
  ./bin/magpie autoplay games "$5" -lex CSW21 -gp true -threads 10 -seed "$4" -wmp true -pat1 "$2" -pat2 "$3" > "$OUT/$1.txt" 2>&1
  log "$1 (whole game, $5 pairs): $(grep -h 'mirrored pair' $OUT/$1.txt | head -1) | $(grep -h 'confidence' $OUT/$1.txt | head -1)"
}
mean_of() { grep -h 'paired effect' "$OUT/$1.txt" 2>/dev/null | head -1 | sed -E 's/.*mean ([-0-9.]+).*/\1/'; }

log "overnight run started; waiting for the queued hook_d1 pair"
until [ -f /tmp/hookd1_80_vs_v3.txt ]; do sleep 60; done
for f in seed4_vs_v3 hookd1_130_vs_v3 hookd1_80_vs_v3; do
  log "(earlier) $f: $(grep -h 'paired effect' /tmp/$f.txt | head -1)"
done

# 1. hook_d1 sweep
for v in 115 150; do
  sed "s/^hook_d1,-102$/hook_d1,-$v/" $DATA/pat_dls_champion_v3.pat | grep -v "^# " > $DATA/pat_v3_hookd1_$v.pat
done
compare hookd1_115_vs_v3 "pat_dls_champion_v3:pat_v3_hookd1_115:1210000000:120000:100"
compare hookd1_150_vs_v3 "pat_dls_champion_v3:pat_v3_hookd1_150:1220000000:120000:100"

# 2. seed tournament: train 8 seeds of the v3 recipe, screen vs v3, confirm the top two
i=5
while [ $i -le 12 ]; do
  seed=$((61000001 + i))
  rm -f settings.txt
  ./bin/magpie patgen 30000,30000,30000,30000,30000 pat_iter_lexsigned_leave_seed$i -lex CSW21 -gp true -threads 10 -seed $seed -wmp true -pat pat_zero_lexsigned_nofit > $OUT/patgen_seed$i.txt 2>&1
  i=$((i + 1))
done
log "seeds 5-12 trained"
i=5
while [ $i -le 12 ]; do
  compare screen_seed${i}_vs_v3 "pat_dls_champion_v3:pat_iter_lexsigned_leave_seed$i:$((1400000000 + i * 10000000)):60000:100"
  i=$((i + 1))
done
# rank by screened mean, confirm the top two on fresh ranges
best=""
i=5
while [ $i -le 12 ]; do
  m=$(mean_of screen_seed${i}_vs_v3)
  [ -n "$m" ] && best="$best
$m seed$i"
  i=$((i + 1))
done
top=$(printf '%s\n' "$best" | grep . | sort -g -r | head -2 | awk '{print $2}')
n=0
for s in $top; do
  n=$((n + 1))
  compare confirm_${s}_vs_v3 "pat_dls_champion_v3:pat_iter_lexsigned_leave_$s:$((1500000000 + n * 10000000)):120000:100"
  wholegame wholegame_${s}_vs_v3 pat_iter_lexsigned_leave_$s pat_dls_champion_v3 $((777000020 + n)) 300000
done

# 3. stage-scale diagnostic (Astra's B): rescale v3's PAT term per stage
for stage in "early:55:86" "mid:30:54" "late:1:29"; do
  st=$(echo $stage | cut -d: -f1); lo=$(echo $stage | cut -d: -f2); hi=$(echo $stage | cut -d: -f3)
  k=0
  for sc in 0.7 1.3; do
    k=$((k + 1))
    compare stage_${st}_scale${sc} "pat_dls_champion_v3:patscale${sc}@pat_dls_champion_v3:$((1600000000 + lo * 1000000 + k * 1000)):120000:100:$lo:$hi"
  done
done

# 4. tighteners
compare overlay_vs_v3_range2 "pat_dls_champion_v3:pat_iter_lexsigned_overlay:1700000000:120000:100"
wholegame wholegame_v3_vs_v2_seed5 pat_dls_champion_v3 pat_dls_champion_v2 777000005 500000

# 5. finer stage scales
for stage in "early:55:86" "mid:30:54" "late:1:29"; do
  st=$(echo $stage | cut -d: -f1); lo=$(echo $stage | cut -d: -f2); hi=$(echo $stage | cut -d: -f3)
  k=0
  for sc in 0.85 1.15; do
    k=$((k + 1))
    compare stage_${st}_scale${sc} "pat_dls_champion_v3:patscale${sc}@pat_dls_champion_v3:$((1800000000 + lo * 1000000 + k * 1000)):120000:100:$lo:$hi"
  done
done

# 6. more seeds while time remains
i=13
while [ $i -le 20 ]; do
  seed=$((61000001 + i))
  rm -f settings.txt
  ./bin/magpie patgen 30000,30000,30000,30000,30000 pat_iter_lexsigned_leave_seed$i -lex CSW21 -gp true -threads 10 -seed $seed -wmp true -pat pat_zero_lexsigned_nofit > $OUT/patgen_seed$i.txt 2>&1
  compare screen_seed${i}_vs_v3 "pat_dls_champion_v3:pat_iter_lexsigned_leave_seed$i:$((1900000000 + i * 10000000)):60000:100"
  i=$((i + 1))
done
log "overnight run finished"
