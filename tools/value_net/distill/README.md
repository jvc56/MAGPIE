# Distilling the value net

A smaller student learns to reproduce the teacher
(`models/macondo-nn-tf-nwl23s-v1`) on the rows MAGPIE actually scores: the
top 50 static candidates at each decision of teacher self-play games.

## 1. Data: teacher self-play (`valuenet:distill`)

```sh
./bin/magpie_test valuenet:distill:models/macondo-nn-tf-nwl23s-v1:fp16:NWL23:\
<games>:<seed>:<worker>:<workers>:<out>.bin:threads=8
```

Each decision with tiles in the bag writes one 2,712-byte record per
candidate (`VntDistillRecord` in `test/value_net_test.c`, `data.py`): the
input row (the 85 planes are all 0/1, so they are packed as bits), the
teacher's value and spread (fp16 Metal, within ~1.5e-3 of fp32), the move's
static equity and the mover's spread after it, and the game, turn and
candidate. Moves are the teacher's best by MAGPIE's utility, with softmax
exploration over static equity (temperature 1 point) while the bag holds
more than 60 tiles and 5% of the time after. About 1,000 records per game.

`python data.py '<glob>' <teacher_dir>` decodes a sample and reruns the
teacher on it in PyTorch as a check.

## 2. Training (`train.py`, MLX)

```sh
nn-venv/bin/python train.py --data '<dir>/*.bin' --out runs/t4 \
    --init-teacher models/macondo-nn-tf-nwl23s-v1 --keep-layers 0,1,2,3 \
    --lr 3e-4 --warmup 200 --w-centered 5 --minutes 30
```

`--data` takes comma-separated globs (`.npy` index caches are skipped).
Batches are 64 decisions x 8 of their candidates (`--top k`: always the
teacher's top k by utility, the rest at random); the loss is squared error
on the teacher's value, on the value centered within each decision (move
choice depends only on differences), and on the spread, plus with
`--w-rank` a listwise term: the KL divergence from the teacher's softmax
over each decision's utilities (temperature `--rank-temp`) to the
student's. Neither helped: over 3,000 steps from teacher blocks 0-3 on the
pilot data, utility regret was 0.00397 with neither, 0.00449 with `--top 4`,
0.00453 with `--w-rank 0.1 --rank-temp 0.01`, and 0.00468 with both
(standard error ~0.0003; all on the same 4,166 held-out decisions).
Weights are fp32 and the forward pass fp16, with LayerNorm in
fp32 (its fp16 backward overflows on the low-variance cls and tile
tokens). The learning rate warms up, then follows a cosine over `--steps`
or, without it, over the `--minutes` budget.

Each file's decisions are indexed once (`<file>.idx.npy`, extended when
the file grows). Files still being written can be trained on: a partial
last decision is left out, and `--refresh-minutes` adds what has been
written since.

Validation uses whole decisions from held-out games (`game_id % 50 == 0`,
of `--val-data` if given, so runs on different data share a validation
set) and reports top-1 agreement with the teacher, the teacher-measured
regret of the student's picks by value and by MAGPIE's default utility
(with its standard error), and `regret_k`: the utility regret if the
teacher rescored the student's top k.

The student is written in the teacher's format (`weights.f32` +
`manifest.json` with `hparams`), which MAGPIE's CPU and Metal backends and
`model.py`'s `load_blob` read.

## 3. In MAGPIE

```sh
./bin/magpie_test valuenet:games:<teacher_dir>:fp16:NWL23:1000:<seed>:<w>:<workers>:\
<out>:b=nn:model_a=<student_dir>
```

`cands=` sets how many top static moves an nn player scores (default 50),
and `rescore=<dir>` with `rescore_top=k` makes it a cascade: the net at
`<dir>` rescores the player's top k and decides. The Metal backend pads
each evaluation to a power of two rows (at least 8), so 50 candidates
cost 64 rows, 17-32 cost 32 and 9-16 cost 16. Against the teacher at 50
candidates (1,000 pairs, fp32, MAGPIE's default utility, seed 20266201;
speed per move on an idle M5 Max GPU, 8 game processes / 1):

| player a | a's score | speed vs teacher |
|---|---|---|
| teacher, `cands=32` | 50.05% +/- 0.27 | 1.98x / 2.08x |
| teacher, `cands=16` | 49.35% +/- 0.47 | 3.94x / 3.76x |
| student (blocks 0-3, 3,000 steps) + teacher on its top 5 | 48.95% +/- 0.56 | 1.49x / 1.52x |
| student alone | 45.77% +/- 0.88 | 1.86x / 1.96x |

Offline, the teacher's utility regret from scoring only static's top k
is 0.00052 for k = 16 and 0.00017 for k = 32; each 0.001 of regret has
cost about 1.1 points of win rate in these games.

PyTorch training on this Mac's GPU ran at about half MLX's speed (the
backward pass dominates), so MLX trains; `model.py` is the reference
implementation and parity check.
