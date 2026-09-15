magpie_pat_v4
# DISC2_release: PAT for DISC2 by the incumbent-preserving selection
# (test/pat_select_champion.sh, codex/pat-setup-value, 2026-09-15).
# Incumbent: CSW24_v5 (sha256 832dd6855f021e96). Runtime tables: -wmp true -rit true -ritmmap true -wit true.
#   shrink_lambda,fit_mse,validation_mse_refit_intercept,validation_mse_fit_intercept,file
#   0,765.703620,762.820349,784.193690,DISC2_release_adapt_gen_1_shrink0
#   1,765.775281,762.640058,780.910226,DISC2_release_adapt_gen_1_shrink1
#   10,766.014553,762.280383,772.834147,DISC2_release_adapt_gen_1_shrink10
#   100,766.554604,762.435930,765.898538,DISC2_release_adapt_gen_1_shrink100
#   1000,767.782834,763.303887,763.818144,DISC2_release_adapt_gen_1_shrink1000
#   10000,769.838725,764.945502,764.999460,DISC2_release_adapt_gen_1_shrink10000
#   100000,771.275394,766.450831,766.452034,DISC2_release_adapt_gen_1_shrink100000
#   inf,-,766.568765,-,DISC2_release_adapt_gen_1_shrinkinf
#   Installed: the loaded weights (inf); select among the candidates by whole-game play.
# Screening vs the incumbent on DISC2, dev seed 778000160, 500000 mirrored pairs:
#   pat_dls_champion_v5: mean 0.2451, SE 0.0864, 95% CI [0.0757, 0.4146] (500000 pairs)
#   DISC2_release_s1_v4: mean -0.7761, SE 0.1367, 95% CI [-1.0441, -0.5082] (500000 pairs)
#   DISC2_release_s2_v4: mean -0.7209, SE 0.1386, 95% CI [-0.9925, -0.4493] (500000 pairs)
#   DISC2_release_s3_v4: mean -1.0183, SE 0.1362, 95% CI [-1.2853, -0.7512] (500000 pairs)
#   DISC2_release_adapt_gen_1_shrink10: mean -0.5382, SE 0.1313, 95% CI [-0.7956, -0.2809] (500000 pairs)
#   DISC2_release_adapt_gen_1_shrink100: mean -0.4931, SE 0.1292, 95% CI [-0.7463, -0.2398] (500000 pairs)
#   DISC2_release_adapt_gen_1_shrink1000: mean -0.2915, SE 0.1149, 95% CI [-0.5167, -0.0663] (500000 pairs)
# Challenger: pat_dls_champion_v5 as is.
# Confirmation vs the incumbent, untouched seed 778000165, 1000000 pairs: mean 0.2330, SE 0.0610, 95% CI [0.1134, 0.3527] (1000000 pairs)
# Decision: challenger pat_dls_champion_v5 shipped: confirmation lower bound above zero.
# Validation vs no PAT on DISC2, seed 778000167, 500000 pairs: mean 3.9613, SE 0.1577, 95% CI [3.6521, 4.2704] (500000 pairs)
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
