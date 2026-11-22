#!/bin/sh
# Generate late-game positions (opponent rack hidden) and keep the close ones.
R=/Users/olaugh/sources/nov03-censussim/egswitch-run
cd /Users/olaugh/sources/nov03-censussim/egswitch || exit 1
$R/mt_peg pegpos:500000,14000 2>/dev/null | /usr/bin/grep '^B ' > $R/pegpos_all.txt
for b in 1 2 3 4; do
  # hide the opponent's rack; prefilter on a score difference of at most 70
  awk -v b=$b '$2==b{$1="";$2="";$3="";sub(/^ +/,""); sub(/\/[A-Z?]*$/,"/",$2); split($3,s,"/"); d=s[1]-s[2]; if(d<0)d=-d; if(d<=70)print}' $R/pegpos_all.txt > $R/pegpre_$b.txt
  $R/mt_peg pegclose:$R/pegpre_$b.txt,8 2>/dev/null | /usr/bin/grep '^PC ' > $R/pegpre_$b.pc
  awk 'NR==FNR{if($3!="SKIP"&&$3>=0.25&&$3<=0.75)keep[$2]=1; next} (FNR-1) in keep' $R/pegpre_$b.pc $R/pegpre_$b.txt > $R/pegclose_$b.txt
  echo "$(date) bag $b: raw $(awk -v b=$b '$2==b' $R/pegpos_all.txt | wc -l) pre $(wc -l < $R/pegpre_$b.txt) close $(wc -l < $R/pegclose_$b.txt)" >> $R/peg_log.txt
done
