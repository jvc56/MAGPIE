# macondo-nn-tf-streamopen v1 — handoff for a Metal port

The Macondo transformer value net (3,665,355 parameters) that wins 57.65% of
paired games against HastyBot (100,000 pairs, NWL23) from one batched forward
pass per move (2026-10-01). Same architecture, inputs, outputs and blob
layout as the earlier nwl23s handoff (57.2%); only the weights differ: this
one trained twice as long (~100k steps) on 54M sampled-openings games,
streamed with a fresh position per game per pass. It plays level with a
2-ply Monte Carlo sim of 100 candidates (head to head 49.0% +/- 1.6). Full usage doc, with the feature vector index by index and the
move-selection loop: the Claude doc "Macondo value net for Magpie". This file
is the short version so the tarball stands alone.

## Files

| file | what |
|---|---|
| `weights.f32`, `manifest.json` | all 121 tensors as one little-endian float32 blob, row-major (C order), with names, shapes and float offsets. `fread` the blob once and slice by offset. |
| `parity/` | 64 real input rows (`board.f32` 64x85x15x15, `scalars.f32` 64x72) and the reference outputs (`value.f32`, `spread.f32`, fp32, from ONNX Runtime). A correct port matches `value` to ~1e-5 in fp32, ~2e-3 in fp16. |
| `MD5SUMS` | checksums. |

This is the format for a C + Metal implementation: no runtime dependency,
and MPSGraph / MPSMatrix / hand-written kernels take raw float buffers as
they are (transpose `[out,in]` weights if your matmul wants `[in,out]`).

## Other formats (available on request; not included to keep this small)

- `model.onnx` (14.7 MB): the served graph, opset 17, fp32, dynamic batch;
  inputs `board` (B,85,15,15), `scalars` (B,72); outputs `value`, `spread`.
  Runs under ONNX Runtime (which has a CoreML execution provider) but that
  means linking onnxruntime from C.
- `best-tf-streamopen.pt` (14.7 MB): the PyTorch checkpoint, step 97,500. From
  it, in domino14/macondo branch claude/transformer-valuenet, `pytorch/`:
  `python export.py --ckpt best-tf-streamopen.pt --out model.onnx` makes the
  ONNX and `python export-raw.py --ckpt best-tf-streamopen.pt --onnx model.onnx
  --out DIR` makes this blob + parity set. A CoreML `.mlpackage` comes from
  the checkpoint with coremltools (`ct.convert` on a traced
  `export.ModelWrapper(net, "wdl")`), not from the ONNX; recent coremltools
  no longer converts ONNX directly.

The leave-value table is NWL23.klv2 (Magpie already has it); scalar 69 is
the candidate leave's value from that file.

## What the net scores

The position AFTER a candidate move, from the mover's side: the board with
the tiles placed, the mover holding only the leave, nothing drawn yet, and
the opponent's rack returned to the bag (the net never sees it).
`value` = P(win) - P(loss) for the mover. Do not call it with an empty bag
(never trained there).

## Input row: 85 planes (index c*225 + row*15 + col), then 72 scalars

Planes: 0-25 letters A..Z (a designated blank counts as its letter),
26 blank marker, 27-52 horizontal cross-check per letter A..Z (empty square,
letter valid with its left/right neighbors; all 26 set when no neighbor;
none on an occupied square), 53-78 vertical cross-check (above/below),
79 DLS, 80 TLS, 81 DWS, 82 TWS (empty premium squares only), 83 squares the
candidate placed tiles on, 84 squares the opponent's last play placed tiles on.

Scalars (tile order blank=0, A=1..Z=26; tr = bag + opponent rack; tanhs(x,c,s)
= tanh((x-c)/s)): 0-26 leave counts/7; 27-53 unseen counts/tr; 54 tanhs(opp
last move score,45,40); 55 opp last move tiles played/7; 56 opp last move
tiles exchanged/7; 57-62 unseen blanks/2, J, Q, S/4, X, Z; 63 unseen
vowels(AEIOU)/tr; 64 unseen consonants/tr; 65 leave vowels/leave size;
66 leave consonants/leave size; 67 opponent moves since their last bingo/25;
68 tanhs(candidate score,45,40); 69 tanhs(leave value,10,20); 70 tr/100;
71 tanhs(spread after the move, mover's side,0,130).

## Move selection (Macondo's FastMlBot)

Generate all legal moves, take the top 50 by static equity, build one row per
candidate (play on a scratch copy without drawing, opponent rack back in the
bag, build, unplay), one batched inference, play the highest `value`
(ties: more tiles played). Empty bag: static equity instead.

Decided games (as measured, 57.65%): when the best candidate's `value` is
beyond +/-0.97 the head saturates and its ranking is noise, so among the
candidates within 0.01 of the best value, play the one with the highest
expected final spread = mover's spread after the move + 130 * atanh(spread).
It does not change the win rate but adds ~7 points of spread per game.

## Forward pass (for a hand-written port)

Tokens (254 x 192): cls; 225 squares = square_proj(85->192)(plane values at
the square) + pos_emb[square]; 27 tile tokens = tile_proj(2->192)([scalars[i],
scalars[27+i]]) + tile_emb[i]; 1 game token = game_proj(72->192)(scalars).
8 pre-norm blocks: x += proj(attn(ln1(x))); x += fc2(gelu(fc1(ln2(x)))),
6 heads of 32, qkv (192->576) laid out [q|k|v][head][dim], scale 1/sqrt(32),
no mask, exact-erf GELU, LayerNorm eps 1e-5, fc1 192->768, fc2 768->192.
Head: ln_f(cls) -> fc1 (192->128) + ReLU -> heads.wdl (128->3), softmax over
[loss, draw, win], value = p_win - p_loss; heads.spread (128->1) + tanh.
Linear weights are stored [out, in]: y = x W^T + b.

## All six heads (all in the blob, for experimenting)

The five scalar heads read the same h = ReLU(fc1(ln_f(cls))) (128 floats);
the spatial head reads ln_f applied to each of the 225 square tokens.

| tensor | shape | output | predicts |
|---|---|---|---|
| heads.wdl | 3x128 | softmax -> [P(loss), P(draw), P(win)] | mover's final result; served ranking is P(win)-P(loss), P(win)+0.5 P(draw) is the other natural choice |
| heads.spread | 1x128 | tanh | final spread change, mover's side: points = 130 atanh(y) |
| heads.opp_bingo | 1x128 | sigmoid | P(opponent bingos next turn) |
| heads.opp_score | 1x128 | none | opponent's next-move score / 300 |
| heads.value | 1x128 | tanh | trained at weight 0 in this run: random, ignore |
| heads_spatial | 4x192 per square token | sigmoid per square | P(opp next move covers square), P(own next move covers), P(opp covers & wins), P(own covers & wins); a 4x15x15 map |

Only heads.wdl has been measured in play; the rest were auxiliary training
signal, uncalibrated for decisions.
