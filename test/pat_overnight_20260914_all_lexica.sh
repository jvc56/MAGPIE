#!/bin/sh
# Overnight 2026-09-14: a PAT release file for every lexicon with a KWG and
# a KLV that does not have one yet, by the incumbent-preserving selection
# (test/pat_select_champion.sh) with CSW24_v5 as the incumbent. Each
# lexicon's tables are built and, once its file validates, deleted. The
# release files themselves are data, not code -- they belong in
# MAGPIE-DATA (data/strategy/<lex>.pat), never checked into this repo.
# Polish (OSPS49, no WMP) runs last, without tables.
set -u
cd /Users/olaugh/sources/magpie-pat-setup-value
out=/tmp/pat_lm/overnight_all
mkdir -p $out
for lex in NWL20 TWL06 TWL14 TWL98 FRA20 DISC2 DSW25 RD29 OSPS49; do
  echo "[$(date +%H:%M:%S)] ===== $lex =====" | tee -a $out/summary.txt
  sh test/pat_select_champion.sh "$lex" CSW24_v5 "${lex}_release" "$out/$lex" > "$out/$lex.log" 2>&1
  status=$?
  grep -E "Decision|Validation vs no PAT|NOT shippable|shippable" "$out/$lex/provenance.txt" "$out/$lex.log" 2>/dev/null | sed 's/^/  /' | tee -a $out/summary.txt
  echo "  exit $status; free: $(df -h /Users/olaugh/sources | tail -1 | awk '{print $4}')" | tee -a $out/summary.txt
done
echo "[$(date +%H:%M:%S)] ===== all done =====" | tee -a $out/summary.txt
