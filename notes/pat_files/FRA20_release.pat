magpie_pat_v5
# FRA20_release: PAT for FRA20 by the incumbent-preserving selection
# (test/pat_select_champion.sh, codex/pat-setup-value, 2026-09-15).
# Incumbent: CSW24_v5 (sha256 832dd6855f021e96). Runtime tables: -wmp true -rit true -ritmmap true -wit true.
#   shrink_lambda,fit_mse,validation_mse_refit_intercept,validation_mse_fit_intercept,file
#   0,704.251628,706.409328,728.154543,FRA20_release_adapt_gen_1_shrink0
#   1,704.302512,706.269533,725.883310,FRA20_release_adapt_gen_1_shrink1
#   10,704.495771,705.879307,717.362848,FRA20_release_adapt_gen_1_shrink10
#   100,705.103983,706.018705,709.398619,FRA20_release_adapt_gen_1_shrink100
#   1000,706.540772,706.933214,707.239036,FRA20_release_adapt_gen_1_shrink1000
#   10000,709.067017,708.999712,709.009649,FRA20_release_adapt_gen_1_shrink10000
#   100000,710.689797,710.556403,710.559235,FRA20_release_adapt_gen_1_shrink100000
#   inf,-,710.787069,-,FRA20_release_adapt_gen_1_shrinkinf
#   Installed: the loaded weights (inf); select among the candidates by whole-game play.
# Screening vs the incumbent on FRA20, dev seed 778009270, 500000 mirrored pairs:
#   pat_dls_champion_v5: mean 0.0642, SE 0.0789, 95% CI [-0.0905, 0.2189] (500000 pairs)
#   FRA20_release_s1_v4: mean -1.0213, SE 0.1309, 95% CI [-1.2779, -0.7646] (500000 pairs)
#   FRA20_release_s2_v4: mean -1.0519, SE 0.1311, 95% CI [-1.3088, -0.7949] (500000 pairs)
#   FRA20_release_s3_v4: mean -0.9425, SE 0.1302, 95% CI [-1.1977, -0.6872] (500000 pairs)
#   FRA20_release_adapt_gen_1_shrink10: mean -0.5992, SE 0.1195, 95% CI [-0.8334, -0.3651] (500000 pairs)
#   FRA20_release_adapt_gen_1_shrink100: mean -0.8903, SE 0.1222, 95% CI [-1.1298, -0.6509] (500000 pairs)
#   FRA20_release_adapt_gen_1_shrink1000: mean -0.6595, SE 0.1109, 95% CI [-0.8769, -0.4421] (500000 pairs)
# Challenger: pat_dls_champion_v5 as is.
# Confirmation vs the incumbent, untouched seed 778009275, 1000000 pairs: mean 0.0250, SE 0.0559, 95% CI [-0.0845, 0.1344] (1000000 pairs)
# Decision: incumbent retained: challenger pat_dls_champion_v5 did not confirm.
# Validation vs no PAT on FRA20, seed 778009277, 500000 pairs: mean 3.0987, SE 0.1512, 95% CI [2.8024, 3.3949] (500000 pairs)
# Rows below are byte-identical to CSW24_v5; its own provenance:
# | CSW24_v5: PAT champion for CSW24 by the v5 recipe
# | (notes/pat_champion_recipe.md, codex/pat-setup-value). Weights: the
# | best of 4 seeds (61000002 + i - 1) of the iterative recipe (5 x 30K
# | games from pat_zero_lexsigned_nofit) with the 14 float_through_*
# | weights refitted under run_through,1 (150K games, seed 4242,
# | fit_residual 3 during that fit). Tournament, whole game, 500000
# | mirrored pairs, per pair:
# |   seed 2 vs seed 1: mean -0.2221, SE 0.0870, 95% CI [-0.3927, -0.0515] (500000 pairs)
# |   seed 3 vs seed 1: mean -0.1446, SE 0.0821, 95% CI [-0.3056, 0.0163] (500000 pairs)
# |   seed 4 vs seed 1: mean -0.2871, SE 0.0909, 95% CI [-0.4654, -0.1089] (500000 pairs)
# |   winner: seed 1
# | Opening table: sim - static by tiles on 1000 seeded opening racks
# | (patopeningsim:CSW24:CSW24_v5_s1_v4:1000), relative to the best bin.
# | Validation vs no PAT (seed 777100099, 500000 pairs): mean 3.3187, SE 0.1363, 95% CI [3.0515, 3.5859] (500000 pairs)
gamma,0.500000
own_asset_discount,0.000000
lexicon_floaters,1
signed_through,1
fit_scaled,0
train_overlay,0
fit_residual,0
exact_created_hooks,0
run_through,1
stage_scale_early,1.000000
stage_scale_mid,1.000000
stage_scale_late,1.000000
hook_d1,-107
hook_d2,-37
hook_d3,-24
hook_d4,-24
hook_d5,-26
hook_d6,-24
hook_d7,-27
float_flex_d1,0
float_flex_d2,-18
float_flex_d3,0
float_flex_d4,0
float_flex_d5,-12
float_flex_d6,-2
float_flex_d7,-7
float_score_d1,-76
float_score_d2,-64
float_score_d3,-86
float_score_d4,-51
float_score_d5,-21
float_score_d6,-28
float_score_d7,-43
float_through_score_d1,0
float_through_score_d2,0
float_through_score_d3,0
float_through_score_d4,0
float_through_score_d5,0
float_through_score_d6,0
float_through_score_d7,0
float_through_count_d1,0
float_through_count_d2,-7
float_through_count_d3,-2
float_through_count_d4,-13
float_through_count_d5,-8
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
dws_hook_d1,-28
dws_hook_d2,-9
dws_hook_d3,-13
dws_hook_d4,0
dws_hook_d5,-2
dws_hook_d6,0
dws_hook_d7,-5
dws_float_score_d1,0
dws_float_score_d2,-1
dws_float_score_d3,-22
dws_float_score_d4,-24
dws_float_score_d5,-8
dws_float_score_d6,-21
dws_float_score_d7,-46
tls_hook_d1,-27
tls_hook_d2,0
tls_hook_d3,-12
tls_hook_d4,-7
tls_hook_d5,0
tls_hook_d6,-18
tls_hook_d7,-2
tls_float_score_d1,0
tls_float_score_d2,-16
tls_float_score_d3,-38
tls_float_score_d4,-9
tls_float_score_d5,-13
tls_float_score_d6,-5
tls_float_score_d7,-1
dls_hook_d1,-9
dls_hook_d2,-1
dls_hook_d3,-13
dls_hook_d4,-4
dls_hook_d5,-10
dls_hook_d6,0
dls_hook_d7,-4
dls_float_score_d1,-2
dls_float_score_d2,-19
dls_float_score_d3,-4
dls_float_score_d4,-26
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
tt_floater,-63
tt_hook_only,0
dd_floater,-102
dd_hook_only,-53
dd_tiles_saved,-65
w6_floater,0
w6_hook_only,0
w6_tiles_saved,0
w9_floater,-93
w9_hook_only,0
w9_tiles_saved,0
w12_floater,0
w12_hook_only,0
w12_tiles_saved,0
lm_span_d1,0
lm_span_d2,0
lm_span_d3,0
lm_span_d4,0
lm_span_d5,0
lm_span_d6,0
lm_span_d7,0
lm_ext_d1,0
lm_ext_d2,0
lm_ext_d3,0
lm_ext_d4,0
lm_ext_d5,0
lm_ext_d6,0
lm_ext_d7,0
dws_lm_span_d1,0
dws_lm_span_d2,0
dws_lm_span_d3,0
dws_lm_span_d4,0
dws_lm_span_d5,0
dws_lm_span_d6,0
dws_lm_span_d7,0
dws_lm_ext_d1,0
dws_lm_ext_d2,0
dws_lm_ext_d3,0
dws_lm_ext_d4,0
dws_lm_ext_d5,0
dws_lm_ext_d6,0
dws_lm_ext_d7,0
opening_tiles_2,0
opening_tiles_3,-1296
opening_tiles_4,-1496
opening_tiles_5,-2060
opening_tiles_6,-1975
opening_tiles_7,-2175
opening_exchange,-220
