# Fresh representative PEG quality calibration

Frozen panel maps: 200/200. Oracle interpretation: measured when direct 4 covers every disagreement; otherwise model-based direct-3 full panel plus bag-conditioned direct-4 correction with both sampling stages bootstrapped.

Agreements enter every panel-wide estimate as exact zero. CIs use a bag-stratified source-game bootstrap; every panel position has a different source game.

## Primary comparisons

| comparison | disagreements | utility (95% CI) | win pp (95% CI) | spread (95% CI) | oracle basis |
|---|---:|---:|---:|---:|---|
| full32_minus_adaptive | 23/200 | +0.00229 [-0.00264, +0.00896] | +0.22083 [-0.25278, +0.90000] | +0.81650 [+0.09250, +1.66024] | measured_direct4 |
| deep16_minus_adaptive | 42/200 | +0.00282 [-0.00028, +0.00763] | +0.26730 [-0.04040, +0.76431] | +1.43639 [+0.67508, +2.28014] | measured_direct4 |
| deep24_minus_deep16 | 10/200 | +0.00165 [-0.00000, +0.00443] | +0.16389 [+0.00000, +0.42778] | +0.10283 [-0.26217, +0.47245] | measured_direct4 |

Panel-wide values above include agreements as exact zero. Conditional disagreement means and exact root-win counts are:

| comparison | conditional utility | conditional win pp | conditional spread | direct-4 after/before/tie |
|---|---:|---:|---:|---:|
| full32_minus_adaptive | +0.01991 | +1.920 | +7.100 | 14/8/178 |
| deep16_minus_adaptive | +0.01341 | +1.273 | +6.840 | 31/10/159 |
| deep24_minus_deep16 | +0.03298 | +3.278 | +2.057 | 6/3/191 |

### Bag-conditioned primary effects

| comparison | bag | utility (95% CI) | win pp (95% CI) | spread (95% CI) |
|---|---:|---:|---:|---:|
| full32_minus_adaptive | 1 | +0.01022 [-0.00003, +0.03050] | +1.00000 [+0.00000, +3.00000] | +2.19000 [-0.40000, +5.20025] |
| full32_minus_adaptive | 2 | +0.00066 [-0.00885, +0.01237] | +0.05556 [-0.88889, +1.22222] | +1.08000 [+0.04000, +2.47786] |
| full32_minus_adaptive | 3 | -0.00172 [-0.01024, +0.00504] | -0.17222 [-1.01667, +0.50000] | -0.00400 [-0.78933, +0.61067] |
| full32_minus_adaptive | 4 | +0.00000 [+0.00000, +0.00000] | +0.00000 [+0.00000, +0.00000] | +0.00000 [+0.00000, +0.00000] |
| deep16_minus_adaptive | 1 | +0.00028 [+0.00009, +0.00053] | +0.00000 [+0.00000, +0.00000] | +2.84000 [+0.86000, +5.25000] |
| deep16_minus_adaptive | 2 | -0.00157 [-0.00536, +0.00121] | -0.16667 [-0.55556, +0.11111] | +0.96000 [-0.16111, +2.44339] |
| deep16_minus_adaptive | 3 | +0.00293 [-0.00075, +0.00788] | +0.28333 [-0.08889, +0.77222] | +0.96478 [-0.47372, +2.55956] |
| deep16_minus_adaptive | 4 | +0.00962 [+0.00004, +0.02730] | +0.95253 [+0.00101, +2.68348] | +0.98078 [+0.10544, +2.38620] |
| deep24_minus_deep16 | 1 | -0.00003 [-0.00015, +0.00007] | +0.00000 [+0.00000, +0.00000] | -0.27000 [-1.49000, +0.70000] |
| deep24_minus_deep16 | 2 | +0.00502 [+0.00000, +0.01449] | +0.50000 [+0.00000, +1.44444] | +0.19333 [+0.00000, +0.45000] |
| deep24_minus_deep16 | 3 | +0.00160 [+0.00000, +0.00481] | +0.15556 [+0.00000, +0.46667] | +0.48800 [+0.00000, +1.46400] |
| deep24_minus_deep16 | 4 | +0.00000 [+0.00000, +0.00000] | +0.00000 [+0.00000, +0.00000] | +0.00000 [+0.00000, +0.00000] |

## Adaptive noninferiority

Regret is `full 32 - min 8 / patience 8`; noninferiority requires the bootstrap upper 95% bound to be below the margin.

| margin | mean regret | upper 95% | noninferior | one-sided p |
|---:|---:|---:|---|---:|
| 0.00025 | +0.002290 | +0.008913 | False | 0.7341 |
| 0.00050 | +0.002290 | +0.008913 | False | 0.7006 |
| 0.00100 | +0.002290 | +0.008913 | False | 0.6363 |

## Candidate-count regret curve

| completed 2-ply candidates | disagreement with full 32 | full-minus-checkpoint utility (95% CI) |
|---:|---:|---:|
| 2 | 57/200 | +0.01325 [+0.00330, +0.02487] |
| 4 | 40/200 | +0.00545 [-0.00334, +0.01500] |
| 8 | 26/200 | +0.00053 [-0.00617, +0.00793] |
| 12 | 18/200 | +0.00410 [+0.00005, +0.01061] |
| 32 | 0/200 | +0.00000 [+0.00000, +0.00000] |

## Value of each completed 2-ply stage

| added completed candidates | panel utility gain (95% CI) | disagreements | mean added scenarios | mean added nested nodes | M5 mean added s |
|---|---:|---:|---:|---:|---:|
| 2→4 | +0.00780 [+0.00170, +0.01664] | 30/200 | 753 | 444791 | 0.80 |
| 4→8 | +0.00492 [+0.00017, +0.01144] | 18/200 | 1609 | 799677 | 1.46 |
| 8→12 | -0.00357 [-0.00891, +0.00004] | 11/200 | 1592 | 950357 | 1.71 |
| 12→32 | +0.00410 [+0.00005, +0.01061] | 18/200 | 7978 | 5060633 | 9.21 |

## Adaptive stopping choices

| stop policy | disagreement with full 32 | full-minus-stop utility (95% CI) | mean completed candidates | mean scenarios | mean nested nodes | M5 mean s |
|---|---:|---:|---:|---:|---:|---:|
| min 8 / patience 4 | 25/200 | +0.00025 [-0.00683, +0.00772] | 8.20 | 595109 | 1812799 | 6.05 |
| min 8 / patience 8 | 23/200 | +0.00229 [-0.00264, +0.00896] | 9.33 | 595207 | 2103295 | 6.52 |
| min 12 / patience 8 | 15/200 | +0.00407 [+0.00002, +0.01026] | 12.62 | 596726 | 2858268 | 7.95 |
| full 32 | 0/200 | +0.00000 [+0.00000, +0.00000] | 32.00 | 604673 | 7789044 | 16.90 |

| adaptive extension | panel utility gain (95% CI) | disagreements | mean added candidates | mean added nested nodes | M5 mean added s |
|---|---:|---:|---:|---:|---:|
| patience 4→8 after min 8 | -0.00204 [-0.00617, +0.00004] | 2/200 | 1.14 | 290496 | 0.48 |
| min 8→12 at patience 8 | -0.00178 [-0.00431, +0.00004] | 8/200 | 3.29 | 754973 | 1.42 |

## Oracle sensitivity

Common direct-3/direct-4 positions: 73; best-nominee flips: 9 (12.3%).

| comparison | direct-3 panel utility | direct-4 panel utility | direct-4 minus direct-3 |
|---|---:|---:|---:|
| full32_minus_adaptive | +0.00276 | +0.00229 | -0.00047 |
| deep16_minus_adaptive | +0.00585 | +0.00282 | -0.00303 |
| deep24_minus_deep16 | +0.00165 | +0.00165 | -0.00001 |

Stride-2 sensitivity was prospectively checked on two positions per bag:

| depth | common | stride-4/stride-2 best flips | mean absolute nominee utility difference |
|---:|---:|---:|---:|
| 3 | 8 | 1 (12.5%) | 0.00599 |
| 4 | 8 | 1 (12.5%) | 0.00601 |

## Work interpretation

Scenarios, nested nodes, and completed candidates are the strength/work axes. Utility per second is an M5 scheduling conversion only; selected-machine NPS is never a strength feature.

| comparison | utility / M scenarios | utility / M nested nodes | utility / M5 local s |
|---|---:|---:|---:|
| full32_minus_adaptive | 0.241920 | 0.000403 | 0.000221 |
| deep16_minus_adaptive | 0.455000 | 0.000035 | 0.000063 |
| deep24_minus_deep16 | 0.334863 | 0.000052 | 0.000092 |

## Load diagnostics

41 identical sentinels: normalized-throughput CV 0.085; early/late drift -4.1%; median scheduled-core occupancy 0.853. Noisy segments: 0.

Timing from a noisy segment is only a local conversion; completed quality observations remain structurally valid.
