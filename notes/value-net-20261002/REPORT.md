# Macondo value net in MAGPIE (macondo-nn-tf-nwl23s v1), 2026-10-01/02

## Correctness
- **Forward pass** on the handoff's 64 parity rows, max |value − reference|: CPU fp32 6.7e-7; Metal fp32 4.5e-7; Metal fp16 1.5e-3; Neural Engine (CoreML, fp16) 1.4e-3.
- **Input encoding** (`valuenet:dump`) compared with Macondo's `MLVectorsForMoves`, run through a local Go tool on the same positions and moves:
  - 8,000 of 8,000 rows over 160 NWL23 positions are identical within 1e-5, including exchanges and the opponent's last move.
  - MAGPIE's vertical-direction cross set is Macondo's horizontal one, the letters that fit with the left and right neighbors.

## Strength (FastMlBot-style player, NWL23)
The player takes the top 50 static moves, picks the highest value (ties: more tiles played), and plays static with an empty bag.

| run | a | b | a's score | p | spread |
|---|---|---|---:|---:|---:|
| v1 | value net, Metal fp32 | MAGPIE static | **55.85% ± 0.88%** | 4e-11 | −1.4 ± 1.8 |

Macondo reports 57.17% for this net against HastyBot.

## Throughput (M4)
| engine | rows/s | notes |
|---|---:|---|
| CPU, plain C fp32 | ~14 per core | reference only |
| Metal fp32 / fp16 | ~900 / ~1,200 | saturates at about 32 rows per call |
| Neural Engine | ~2,250 | batches of 8–16; larger batches are slower |
| GPU + Neural Engine | ~2,600 | whole calls go to whichever engine is free |

Accelerate's sgemm reaches 1.3–1.9 TFLOPS at these shapes, but no CPU backend beyond the reference was built.

Metal calls are capped at 256 rows. An uncapped 2,048-row benchmark batch needed several GB of activations and caused a kernel watchdog panic on a 16 GB machine.

## Sims with value net replies on the first rollout ply
These are 4-ply sims of 15 root plays on 10 threads. The opponent's first reply is the net's pick among its top K static replies, and later plies are static.
- **Rates:** static rollouts ~60,000 it/s. Value net replies, K = 15: 68 (Metal) / 142 (Neural Engine) / 175 (both). K = 8: 130 / 275 / 363.
- **A1, equal iterations:** both sims at 200 iterations per decision, K = 8, Metal fp16, 300 pairs.
  - Value net replies vs static rollouts scored **50.67% ± 1.85%** (p = 0.72), spread +6.4 ± 3.3.
  - Decision time was 11.2 s vs 32 ms, with 8 game processes sharing one GPU.
  - There is no detectable gain at equal samples.
