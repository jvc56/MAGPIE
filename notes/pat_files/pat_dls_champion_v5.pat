magpie_pat_v4
# pat_dls_champion_v5: pat_dls_champion_v4 plus an opening table; every
# weight and flag byte-identical to v4. The table (opening_tiles_N,
# opening_exchange; milli-equity, relative to a 2-tile opening) is the
# gap between a 4-ply simulation's value and the static equity of an
# opening candidate, by the tiles it plays, measured on 1000 seeded
# opening racks x the top 12 static candidates under v4 with 400
# iterations per candidate (test patopeningsim, seed base 7300000000):
# static evaluation undervalues 2-tile openings by ~3.2 points and
# exchanges by ~2.6 relative to 5-tile plays, bingos by ~1.3, 3-tile
# plays by ~1.0. A whole-game residual under v4 self-play (1M games:
# opener's final spread less opening static equity, by tiles) shows the
# same 2-tile effect. Evidence vs v4, mirrored pairs: seed 777000095
# (500K pairs) +0.109 +/- 0.048, CI [0.015, 0.203]; fresh seed 777000097
# (1M pairs) +0.071 +/- 0.034, CI [0.004, 0.138]; fresh seed 777000098
# (1M pairs, after the pass-handling change, which cannot alter play)
# +0.053 +/- 0.034, CI [-0.014, 0.121]; pooled over the three +0.072
# +/- 0.022. The whole-game-derived table (pat_v4_open_game) scored
# +0.094 +/- 0.056 on its one match. An opening-only change: about 8%
# of openings move, by ~0.8 points.
# Base: pat_dls_champion_v4: pat_dls_champion_v3 with the floater through
# Base: channels measured by the run-keyed table (run_through,1: words that
# Base: contain the whole run at the required end, not a per-tile sum of
# Base: single-letter statistics) and only the 14 float_through_* weights
# Base: refitted as a residual under v3's frozen policy (150K games, seed
# Base: 4242, with fit_residual 3 during that fit; the shipped file carries
# Base: fit_residual 0 so a retrain from it fits every channel); every other
# Base: weight byte-identical to v3.
# Base: Reproduce from codex/pat-setup-value with
# Base:   ./bin/magpie patgen 150000 <name> -lex CSW21 -gp true -threads 10 \
# Base:     -seed 4242 -wmp true -pat pat_dls_champion_v3_runres
# Base: where pat_dls_champion_v3_runres is v3 plus the rows run_through,1 and
# Base: fit_residual,3. Evidence against v3, 500K mirrored pairs each:
# Base: seed 777000071 +0.369 +/- 0.109 per pair, CI [0.16, 0.58]; seed
# Base: 777000072 +0.250 +/- 0.108, CI [0.04, 0.46]. Fixed v3 with only the
# Base: flag (no refit): +0.17 +/- 0.10.
gamma,0.500000
own_asset_discount,0.000000
lexicon_floaters,1
signed_through,1
fit_scaled,0
train_overlay,0
fit_residual,0
exact_created_hooks,0
run_through,1
opening_tiles_2,0
opening_tiles_3,-2160
opening_tiles_4,-2550
opening_tiles_5,-3170
opening_tiles_6,-2630
opening_tiles_7,-1860
opening_exchange,-530
hook_d1,-102
hook_d2,-30
hook_d3,-23
hook_d4,-25
hook_d5,-27
hook_d6,-25
hook_d7,-26
float_flex_d1,0
float_flex_d2,-20
float_flex_d3,0
float_flex_d4,0
float_flex_d5,-9
float_flex_d6,-4
float_flex_d7,-8
float_score_d1,-64
float_score_d2,-72
float_score_d3,-75
float_score_d4,-53
float_score_d5,-33
float_score_d6,-35
float_score_d7,-60
float_through_score_d1,0
float_through_score_d2,0
float_through_score_d3,0
float_through_score_d4,0
float_through_score_d5,0
float_through_score_d6,0
float_through_score_d7,0
float_through_count_d1,0
float_through_count_d2,-8
float_through_count_d3,-2
float_through_count_d4,-15
float_through_count_d5,-10
float_through_count_d6,0
float_through_count_d7,-17
hook_scaled_d1,0
hook_scaled_d2,0
hook_scaled_d3,0
hook_scaled_d4,0
hook_scaled_d5,0
hook_scaled_d6,0
hook_scaled_d7,0
float_flex_scaled_d1,0
float_flex_scaled_d2,0
float_flex_scaled_d3,0
float_flex_scaled_d4,0
float_flex_scaled_d5,0
float_flex_scaled_d6,0
float_flex_scaled_d7,0
hook_score_d1,0
hook_score_d2,0
hook_score_d3,0
hook_score_d4,0
hook_score_d5,0
hook_score_d6,0
hook_score_d7,0
dws_hook_d1,-29
dws_hook_d2,-6
dws_hook_d3,-15
dws_hook_d4,0
dws_hook_d5,-1
dws_hook_d6,-2
dws_hook_d7,-7
dws_float_score_d1,-7
dws_float_score_d2,-9
dws_float_score_d3,-26
dws_float_score_d4,-28
dws_float_score_d5,-7
dws_float_score_d6,-29
dws_float_score_d7,-37
tls_hook_d1,-29
tls_hook_d2,0
tls_hook_d3,-9
tls_hook_d4,-8
tls_hook_d5,-2
tls_hook_d6,-18
tls_hook_d7,-4
tls_float_score_d1,0
tls_float_score_d2,-23
tls_float_score_d3,-41
tls_float_score_d4,-13
tls_float_score_d5,-15
tls_float_score_d6,-17
tls_float_score_d7,-1
dls_hook_d1,-10
dls_hook_d2,0
dls_hook_d3,-13
dls_hook_d4,-6
dls_hook_d5,-7
dls_hook_d6,-4
dls_hook_d7,-4
dls_float_score_d1,0
dls_float_score_d2,-15
dls_float_score_d3,-5
dls_float_score_d4,-32
dls_float_score_d5,0
dls_float_score_d6,0
dls_float_score_d7,0
qws_hook_d1,0
qws_hook_d2,0
qws_hook_d3,0
qws_hook_d4,0
qws_hook_d5,0
qws_hook_d6,0
qws_hook_d7,0
qws_float_score_d1,0
qws_float_score_d2,0
qws_float_score_d3,0
qws_float_score_d4,0
qws_float_score_d5,0
qws_float_score_d6,0
qws_float_score_d7,0
qls_hook_d1,0
qls_hook_d2,0
qls_hook_d3,0
qls_hook_d4,0
qls_hook_d5,0
qls_hook_d6,0
qls_hook_d7,0
qls_float_score_d1,0
qls_float_score_d2,0
qls_float_score_d3,0
qls_float_score_d4,0
qls_float_score_d5,0
qls_float_score_d6,0
qls_float_score_d7,0
tt_floater,-60
tt_hook_only,0
dd_floater,-104
dd_hook_only,-54
dd_tiles_saved,-71
w6_floater,0
w6_hook_only,0
w6_tiles_saved,0
w9_floater,-94
w9_hook_only,0
w9_tiles_saved,0
w12_floater,0
w12_hook_only,0
w12_tiles_saved,0
