#!/bin/sh
# Overnight 2026-09-14: a PAT release file for every lexicon with a KWG and
# a KLV that does not have one yet, by the incumbent-preserving selection
# (test/pat_select_champion.sh) with CSW24_v5 as the incumbent. Each
# lexicon's tables are built and, once its file validates, deleted; each
# release file is copied into notes/pat_files and pushed as it lands.
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
  if [ -f "data/strategy/${lex}_release.pat" ]; then
    cp "data/strategy/${lex}_release.pat" notes/pat_files/
    git add notes/pat_files >/dev/null 2>&1
    git commit -q -m "notes/pat_files: ${lex}_release (overnight incumbent-preserving selection)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01BxtN4jvLjaWPG4HBb1vamN" >/dev/null 2>&1
    git push -q origin codex/pat-setup-value >/dev/null 2>&1 && echo "  pushed" | tee -a $out/summary.txt
  fi
done
echo "[$(date +%H:%M:%S)] ===== all done =====" | tee -a $out/summary.txt
