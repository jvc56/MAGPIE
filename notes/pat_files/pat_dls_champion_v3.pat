magpie_pat_v3
# pat_dls_champion_v3: the v2 champion's training recipe with the two
# floater semantic corrections and the training pipeline's unseen-pool
# exclusion fixed. Reproduce from codex/pat-setup-value with
#   ./bin/magpie patgen 30000,30000,30000,30000,30000 <name> -lex CSW21 \
#     -gp true -threads 10 -seed 61000002 -wmp true -pat pat_zero_lexsigned_nofit
# where pat_zero_lexsigned_nofit is an all-zero v3 file with gamma 0.5,
# lexicon_floaters 1, signed_through 1, fit_scaled 0. Evidence against
# pat_dls_champion_v2: common-reference move-choice harness, 120K
# positions, 100 worlds: +0.53 +/- 0.10 per disagreement (5.7% of
# decisions), 95% CI [0.34, 0.73]; 1,000,000-game mirrored autoplay
# match (seed 777000003): 498,981 wins to 497,004, +0.27 points a game,
# 97.6% confidence. Its pre-move-trained twin (identical head-to-head,
# +0.04 +/- 0.14) confirmed on a second harness range (+0.91 +/- 0.10)
# and two more million-game matches (+0.31 and +0.32 a game, 97.1% and
# 99.8%).
gamma,0.500000
own_asset_discount,0.000000
lexicon_floaters,1
signed_through,1
fit_scaled,0
# trained PAT weights; units: milli-equity per feature unit; all values <= 0
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
float_through_score_d2,-4
float_through_score_d3,0
float_through_score_d4,0
float_through_score_d5,0
float_through_score_d6,0
float_through_score_d7,0
float_through_count_d1,0
float_through_count_d2,0
float_through_count_d3,0
float_through_count_d4,-1
float_through_count_d5,-5
float_through_count_d6,-5
float_through_count_d7,-5
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
