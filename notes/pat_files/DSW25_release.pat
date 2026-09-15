magpie_pat_v5
# DSW25_release: PAT for DSW25 by the incumbent-preserving selection
# (test/pat_select_champion.sh, codex/pat-setup-value, 2026-09-15).
# Incumbent: CSW24_v5 (sha256 832dd6855f021e96). Runtime tables: -wmp true -rit true -ritmmap true -wit true.
#   shrink_lambda,fit_mse,validation_mse_refit_intercept,validation_mse_fit_intercept,file
#   0,810.937345,816.086531,839.859022,DSW25_release_adapt_gen_1_shrink0
#   1,811.042262,815.589033,836.459256,DSW25_release_adapt_gen_1_shrink1
#   10,811.301788,814.904995,828.986579,DSW25_release_adapt_gen_1_shrink10
#   100,811.866086,814.786595,819.055964,DSW25_release_adapt_gen_1_shrink100
#   1000,813.586619,815.938976,816.436293,DSW25_release_adapt_gen_1_shrink1000
#   10000,817.519065,819.270833,819.280835,DSW25_release_adapt_gen_1_shrink10000
#   100000,820.687306,822.134180,822.151054,DSW25_release_adapt_gen_1_shrink100000
#   inf,-,822.778596,-,DSW25_release_adapt_gen_1_shrinkinf
#   Installed: the loaded weights (inf); select among the candidates by whole-game play.
# Screening vs the incumbent on DSW25, dev seed 778008320, 500000 mirrored pairs:
#   pat_dls_champion_v5: mean -0.0370, SE 0.0816, 95% CI [-0.1970, 0.1230] (500000 pairs)
#   DSW25_release_s1_v4: mean -0.8988, SE 0.1428, 95% CI [-1.1787, -0.6189] (500000 pairs)
#   DSW25_release_s2_v4: mean -1.1813, SE 0.1440, 95% CI [-1.4637, -0.8990] (500000 pairs)
#   DSW25_release_s3_v4: mean -1.2268, SE 0.1457, 95% CI [-1.5125, -0.9412] (500000 pairs)
#   DSW25_release_adapt_gen_1_shrink10: mean -1.3295, SE 0.1403, 95% CI [-1.6044, -1.0546] (500000 pairs)
#   DSW25_release_adapt_gen_1_shrink100: mean -1.3292, SE 0.1407, 95% CI [-1.6050, -1.0533] (500000 pairs)
#   DSW25_release_adapt_gen_1_shrink1000: mean -0.7866, SE 0.1317, 95% CI [-1.0448, -0.5284] (500000 pairs)
# Decision: incumbent retained: no challenger beat it on the development seed.
# Validation vs no PAT on DSW25, seed 778008327, 500000 pairs: mean 3.2999, SE 0.1585, 95% CI [2.9892, 3.6107] (500000 pairs)
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
