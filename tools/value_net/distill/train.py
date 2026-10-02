"""Distills the Macondo transformer value net into a smaller one with MLX.

Reads valuenet:distill records (data.py). Each batch is `decisions`
decisions from training games with `per_decision` of their candidates
each; the loss is the student's squared error on the teacher's value, on
the value centered within each decision (what move choice depends on), and
on the spread head. Validation uses every candidate of held-out games'
decisions (game_id % val_mod == 0) and reports how often the student picks
the teacher's move, by value and by MAGPIE's default utility, and the
teacher-measured regret of its picks.

Weights stay float32; the forward pass runs in float16. The student is
written in the teacher's format (weights.f32 + manifest.json, see
model.py), which model.py's load_blob reads back for a parity check.

    nn-venv/bin/python train.py --data '/path/*.bin' --out runs/d128l4 \\
        --d-model 128 --layers 4 --heads 4 --minutes 30
"""

import argparse
import json
import math
import os
import queue
import threading
import time

import mlx.core as mx
import mlx.optimizers as optim
import numpy as np
from mlx.utils import tree_flatten, tree_map, tree_unflatten

from data import BOARD_BYTES, open_record_file, open_records
from model import PLANES, SCALARS, SQUARES, TILE_TYPES

SPREAD_SCALE = 130.0
MAX_SPREAD_OUTPUT = 0.999999


# ---- model ---------------------------------------------------------------

def init_params(d_model, layers, heads, ff_mult, hidden, seed):
    rng = np.random.default_rng(seed)

    def linear(n_out, n_in, std=None):
        std = std if std is not None else 1.0 / math.sqrt(n_in)
        return {"weight": rng.normal(0, std, (n_out, n_in)).astype(np.float32),
                "bias": np.zeros(n_out, np.float32)}

    def norm(d):
        return {"weight": np.ones(d, np.float32), "bias": np.zeros(d, np.float32)}

    d, ff = d_model, ff_mult * d_model
    residual_std = 0.02 / math.sqrt(2 * layers)
    params = {
        "square_proj": linear(d, PLANES),
        "pos_emb": rng.normal(0, 0.02, (1, SQUARES, d)).astype(np.float32),
        "tile_proj": linear(d, 2),
        "tile_emb": rng.normal(0, 0.02, (1, TILE_TYPES, d)).astype(np.float32),
        "game_proj": linear(d, SCALARS),
        "cls": rng.normal(0, 0.02, (1, 1, d)).astype(np.float32),
        "blocks": [{"ln1": norm(d), "qkv": linear(3 * d, d),
                    "proj": linear(d, d, residual_std), "ln2": norm(d),
                    "fc1": linear(ff, d), "fc2": linear(d, ff, residual_std)}
                   for _ in range(layers)],
        "ln_f": norm(d),
        "fc1": linear(hidden, d),
        "heads": {"wdl": linear(3, hidden), "spread": linear(1, hidden)},
    }
    return tree_map(mx.array, params)


def teacher_params(model_dir, keep_layers):
    """The teacher's weights with only the blocks in keep_layers (renumbered
    from 0), as a student's starting point; returns (params, hparams)."""
    manifest = json.load(open(f"{model_dir}/manifest.json"))
    blob = np.fromfile(f"{model_dir}/weights.f32", dtype="<f4")
    tensors = {t["name"]: blob[t["offset_floats"]:t["offset_floats"] + t["count"]]
               .reshape(t["shape"]) for t in manifest["tensors"]}
    hp = manifest["hparams"]
    hparams = dict(d_model=hp["d_model"], layers=len(keep_layers),
                   heads=hp["heads"], ff_mult=hp["ff_mult"],
                   hidden=hp.get("hidden", 128))
    params = init_params(**hparams, seed=0)
    flat = dict(tree_flatten(params))
    for name in flat:
        source = name
        if name.startswith("blocks."):
            _, layer, rest = name.split(".", 2)
            source = f"blocks.{keep_layers[int(layer)]}.{rest}"
        flat[name] = mx.array(tensors[source].reshape(flat[name].shape))
    return tree_unflatten(list(flat.items())), hparams


def _linear(p, x):
    return x @ p["weight"].T + p["bias"]


def _norm(p, x):
    # In float32: a token with small variance (cls and the tile tokens start
    # that way) overflows LayerNorm's float16 backward.
    out = mx.fast.layer_norm(x.astype(mx.float32),
                             p["weight"].astype(mx.float32),
                             p["bias"].astype(mx.float32), 1e-5)
    return out.astype(x.dtype)


def forward(params, board, scalars, heads, dtype=mx.float16):
    """board (B, 85, 225), scalars (B, 72) -> value, spread logit (float32;
    the spread output is its tanh)."""
    p = tree_map(lambda a: a.astype(dtype), params)
    board, scalars = board.astype(dtype), scalars.astype(dtype)
    batch = board.shape[0]
    squares = _linear(p["square_proj"], board.transpose(0, 2, 1)) + p["pos_emb"]
    tiles = mx.stack([scalars[:, :TILE_TYPES],
                      scalars[:, TILE_TYPES:2 * TILE_TYPES]], axis=-1)
    tiles = _linear(p["tile_proj"], tiles) + p["tile_emb"]
    game = _linear(p["game_proj"], scalars)[:, None]
    x = mx.concatenate([p["cls"] + mx.zeros_like(game), squares, tiles, game],
                       axis=1)
    tokens, d = x.shape[1], x.shape[2]
    for block in p["blocks"]:
        qkv = _linear(block["qkv"], _norm(block["ln1"], x))
        qkv = qkv.reshape(batch, tokens, 3, heads, d // heads)
        qkv = qkv.transpose(2, 0, 3, 1, 4)
        attended = mx.fast.scaled_dot_product_attention(
            qkv[0], qkv[1], qkv[2], scale=1.0 / math.sqrt(d // heads))
        x = x + _linear(block["proj"],
                        attended.transpose(0, 2, 1, 3).reshape(batch, tokens, d))
        hidden = _linear(block["fc1"], _norm(block["ln2"], x))
        x = x + _linear(block["fc2"], mx.erf(hidden * 0.7071067811865476)
                        * hidden * 0.5 + hidden * 0.5)
    hidden = mx.maximum(_linear(p["fc1"], _norm(p["ln_f"], x[:, 0])), 0)
    probs = mx.softmax(_linear(p["heads"]["wdl"], hidden).astype(mx.float32),
                       axis=-1)
    spread = _linear(p["heads"]["spread"], hidden).astype(mx.float32)
    return probs[:, 2] - probs[:, 0], spread[:, 0]


def unpack(bits):
    """(B, BOARD_BYTES) uint8 -> (B, 85, 225) of 0/1, on the GPU."""
    shifts = mx.array([7, 6, 5, 4, 3, 2, 1, 0], dtype=mx.uint8)
    flat = ((bits[:, :, None] >> shifts) & 1).reshape(bits.shape[0], -1)
    return flat[:, :PLANES * SQUARES].reshape(-1, PLANES, SQUARES)


# ---- data ----------------------------------------------------------------

def decision_index(part):
    """(first row, candidates, game_id) of each complete decision in a
    records file, cached beside it. A decision's candidates are consecutive
    records, so the scan hops from one decision's first record to the next
    (reading ~1/50 of the file), and a cache for a file that has since grown
    is extended from where it ended."""
    path = part.filename + ".idx.npy"
    index = np.zeros((0, 3), np.int64)
    if os.path.exists(path):
        index = np.load(path)
        if os.path.getmtime(path) >= os.path.getmtime(part.filename):
            return index
    row = int(index[-1, 0] + index[-1, 1]) if len(index) else 0
    new = []
    while row < len(part):
        record = part[row]
        count = int(record["candidates"])
        if record["candidate"] != 0 or count < 1:
            raise ValueError(f"{part.filename}: row {row} starts no decision")
        if row + count > len(part):
            break  # still being written
        new.append((row, count, int(record["game_id"])))
        row += count
    if new:
        index = np.concatenate([index, np.array(new, np.int64)])
    np.save(path, index)
    return index


class Records:
    """All record files as decisions: (part, first row, row count). Training
    decisions have at least min_candidates candidates, so batches keep one
    shape. Games with game_id % val_mod == 0 are held out; validation uses
    those of val_pattern's files (default: pattern's)."""

    def __init__(self, pattern, val_mod, min_candidates, val_pattern=None):
        self.val_mod, self.min_candidates = val_mod, min_candidates
        self.parts = open_records(pattern)
        self.train_parts = len(self.parts)
        val_parts = open_records(val_pattern) if val_pattern else []
        # Decisions of each training file seen so far.
        self.seen = [0] * self.train_parts
        train, val = [], []
        for part_idx, part in enumerate(self.parts + val_parts):
            index = decision_index(part)
            if part_idx < self.train_parts:
                train.append(self._train_entries(part_idx, index))
                self.seen[part_idx] = len(index)
            if part_idx >= self.train_parts or not val_parts:
                val.append(self._entries(part_idx, index)[
                    index[:, 2] % val_mod == 0])
        self.parts += val_parts
        self.train = np.concatenate(train)
        self.val = np.concatenate(val)
        # Validation decisions in a fixed shuffled order, so a prefix spans
        # many games.
        self.val = self.val[np.random.default_rng(0).permutation(len(self.val))]

    @staticmethod
    def _entries(part_idx, index):
        return np.concatenate(
            [np.full((len(index), 1), part_idx), index[:, :2]], axis=1)

    def _train_entries(self, part_idx, index):
        keep = ((index[:, 2] % self.val_mod != 0)
                & (index[:, 1] >= self.min_candidates))
        return self._entries(part_idx, index)[keep]

    def refresh(self):
        """Adds the training decisions written to the training files since
        they were last read (files still being written); returns how many.
        Validation stays as it was. Safe while batches() runs: parts are
        replaced before train grows, and both only grow."""
        added = []
        for part_idx in range(self.train_parts):
            part = open_record_file(self.parts[part_idx].filename)
            index = decision_index(part)
            if len(index) > self.seen[part_idx]:
                self.parts[part_idx] = part
                added.append(self._train_entries(
                    part_idx, index[self.seen[part_idx]:]))
                self.seen[part_idx] = len(index)
        if added:
            self.train = np.concatenate([self.train] + added)
        return sum(len(entries) for entries in added)

    def rows(self, part_idx, indices):
        return self.parts[part_idx][indices]


def batches(records, decisions, per_decision, top, seed, out_queue, stop):
    """Fills out_queue with training batches (numpy) until stop is set. Each
    decision gives per_decision candidates: the teacher's top (by utility)
    and the rest drawn at random from the others."""
    rng = np.random.default_rng(seed)
    while not stop.is_set():
        picks = records.train[rng.integers(0, len(records.train), decisions)]
        chunks = []
        for part_idx, start, count in picks:
            rows = np.asarray(records.parts[part_idx][start:start + count])
            order = np.argsort(-utility(rows["value"], rows["spread"],
                                        rows["spread_after"]), kind="stable")
            offsets = np.concatenate([order[:top], rng.choice(
                order[top:], per_decision - top, replace=False)])
            chunks.append(rows[offsets])
        rows = np.concatenate(chunks)
        out_queue.put((np.ascontiguousarray(rows["board_bits"]),
                       np.ascontiguousarray(rows["scalars"]),
                       rows["value"].copy(), rows["spread"].copy(),
                       rows["spread_after"].copy()))


# ---- training ------------------------------------------------------------

def utility(value, spread, spread_after, w_winpct=1.0, w_spread=0.5,
            spread_scale=100.0):
    """MAGPIE's value_net_utility (numpy)."""
    win = (1.0 + value) / 2.0
    out = np.clip(spread, -MAX_SPREAD_OUTPUT, MAX_SPREAD_OUTPUT)
    final = spread_after + SPREAD_SCALE * np.arctanh(out)
    sigmoid = 1.0 / (1.0 + np.exp(-final / spread_scale))
    return (w_winpct * win + w_spread * sigmoid) / (w_winpct + w_spread)


MAX_SPREAD_LOGIT = math.atanh(MAX_SPREAD_OUTPUT)


def utility_mx(value, spread_logit, spread_after, w_winpct=1.0, w_spread=0.5,
               spread_scale=100.0):
    """utility() from the spread head's logit (MLX, differentiable)."""
    win = (1.0 + value) / 2.0
    final = spread_after + SPREAD_SCALE * mx.clip(
        spread_logit, -MAX_SPREAD_LOGIT, MAX_SPREAD_LOGIT)
    return ((w_winpct * win + w_spread * mx.sigmoid(final / spread_scale))
            / (w_winpct + w_spread))


CASCADE_KS = (2, 3, 5)


def evaluate(params, records, heads, max_decisions):
    """Student vs teacher on held-out decisions (all candidates). regret_k
    is the utility regret if the teacher rescored the student's top k."""
    picks = records.val[:max_decisions]
    errors_v, errors_s = [], []
    agree_v = agree_u = 0
    regret_v, regret_u = [], []
    regret_k = {k: [] for k in CASCADE_KS}
    for part_idx, start, count in picks:
        rows = records.rows(part_idx, np.arange(start, start + count))
        value, spread = forward(params, unpack(mx.array(rows["board_bits"])),
                                mx.array(rows["scalars"]), heads)
        value, spread = np.array(value), np.tanh(np.array(spread))
        errors_v.append(value - rows["value"])
        errors_s.append(spread - rows["spread"])
        teacher_v, student_v = int(np.argmax(rows["value"])), int(np.argmax(value))
        agree_v += teacher_v == student_v
        regret_v.append(rows["value"][teacher_v] - rows["value"][student_v])
        teacher_u_all = utility(rows["value"], rows["spread"], rows["spread_after"])
        student_u_all = utility(value, spread, rows["spread_after"])
        teacher_u, student_u = int(np.argmax(teacher_u_all)), int(np.argmax(student_u_all))
        agree_u += teacher_u == student_u
        regret_u.append(teacher_u_all[teacher_u] - teacher_u_all[student_u])
        student_order = np.argsort(-student_u_all, kind="stable")
        for k in CASCADE_KS:
            regret_k[k].append(teacher_u_all[teacher_u]
                               - teacher_u_all[student_order[:k]].max())
    errors_v, errors_s = np.concatenate(errors_v), np.concatenate(errors_s)
    n = len(picks)
    metrics = {"val_decisions": n,
               "value_rmse": float(np.sqrt((errors_v ** 2).mean())),
               "spread_rmse": float(np.sqrt((errors_s ** 2).mean())),
               "top1_value": agree_v / n, "top1_utility": agree_u / n,
               "regret_value": float(np.mean(regret_v)),
               "regret_utility": float(np.mean(regret_u)),
               "regret_utility_se": float(np.std(regret_u) / math.sqrt(n))}
    for k in CASCADE_KS:
        metrics[f"regret_{k}"] = float(np.mean(regret_k[k]))
    return metrics


def save(params, hparams, out_dir, extra):
    """The student in the teacher's blob format (model.py's save_blob)."""
    os.makedirs(out_dir, exist_ok=True)
    tensors, chunks, offset = [], [], 0
    for name, value in tree_flatten(params):
        array = np.array(value.astype(mx.float32)).astype("<f4")
        tensors.append({"name": name, "shape": list(array.shape),
                        "offset_floats": offset, "count": int(array.size)})
        chunks.append(array.ravel())
        offset += array.size
    np.concatenate(chunks).tofile(f"{out_dir}/weights.f32")
    manifest = {
        "arch": "transformer", "hparams": hparams,
        "dtype": "float32 little-endian, row-major (C order), one blob",
        "total_floats": int(offset),
        "inputs": {"board": [PLANES, 15, 15], "scalars": [SCALARS]},
        "served_outputs": {
            "value": "P(win) - P(loss) from softmax(heads.wdl) [loss, draw, win]",
            "spread": "tanh(heads.spread)"},
        "tensors": tensors}
    manifest.update(extra)
    json.dump(manifest, open(f"{out_dir}/manifest.json", "w"), indent=1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True, help="glob of record files")
    parser.add_argument("--out", required=True)
    parser.add_argument("--d-model", type=int, default=128)
    parser.add_argument("--layers", type=int, default=4)
    parser.add_argument("--heads", type=int, default=4)
    parser.add_argument("--ff-mult", type=int, default=4)
    parser.add_argument("--hidden", type=int, default=128)
    parser.add_argument("--decisions", type=int, default=64)
    parser.add_argument("--per-decision", type=int, default=8)
    parser.add_argument("--top", type=int, default=0,
                        help="of per_decision, always the teacher's top")
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--weight-decay", type=float, default=0.01)
    parser.add_argument("--warmup", type=int, default=500)
    parser.add_argument("--minutes", type=float, default=30.0)
    parser.add_argument("--steps", type=int, default=0,
                        help="total steps (0: estimate from --minutes)")
    parser.add_argument("--w-value", type=float, default=1.0)
    parser.add_argument("--w-centered", type=float, default=1.0)
    parser.add_argument("--init-teacher", default="",
                        help="start from this teacher's weights (its width)")
    parser.add_argument("--keep-layers", default="0,2,4,6",
                        help="teacher blocks the student keeps")
    parser.add_argument("--w-spread", type=float, default=0.5)
    parser.add_argument("--w-rank", type=float, default=0.0,
                        help="listwise loss: KL from the teacher's softmax "
                        "over each decision's utilities to the student's")
    parser.add_argument("--rank-temp", type=float, default=0.01)
    parser.add_argument("--val-mod", type=int, default=50)
    parser.add_argument("--val-data", default="",
                        help="validate on these files' held-out games")
    parser.add_argument("--val-decisions", type=int, default=2000)
    parser.add_argument("--eval-every", type=int, default=1000)
    parser.add_argument("--refresh-minutes", type=float, default=0.0,
                        help="look for new training records this often "
                        "(files still being written)")
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    records = Records(args.data, args.val_mod, args.per_decision,
                      args.val_data)
    print(f"{len(records.train)} train decisions, {len(records.val)} val "
          f"decisions", flush=True)
    if args.init_teacher:
        keep = [int(layer) for layer in args.keep_layers.split(",")]
        params, hparams = teacher_params(args.init_teacher, keep)
        args.heads = hparams["heads"]
        print(f"from {args.init_teacher}, blocks {keep}", flush=True)
    else:
        hparams = dict(d_model=args.d_model, layers=args.layers,
                       heads=args.heads, ff_mult=args.ff_mult,
                       hidden=args.hidden)
        params = init_params(**hparams, seed=args.seed)
    n_params = sum(v.size for _, v in tree_flatten(params))
    print(f"student {hparams}: {n_params / 1e6:.2f}M parameters", flush=True)

    def loss_fn(params, bits, scalars, value_t, spread_t, after):
        value, logit = forward(params, unpack(bits), scalars, args.heads)
        # Rows come per_decision at a time from one decision.
        shape = (args.decisions, args.per_decision)
        value_g, target_g = value.reshape(shape), value_t.reshape(shape)
        centered = ((value_g - value_g.mean(axis=1, keepdims=True))
                    - (target_g - target_g.mean(axis=1, keepdims=True)))
        loss = (args.w_value * mx.mean((value - value_t) ** 2)
                + args.w_centered * mx.mean(centered ** 2)
                + args.w_spread * mx.mean((mx.tanh(logit) - spread_t) ** 2))
        if args.w_rank:
            logit_t = mx.arctanh(mx.clip(spread_t, -MAX_SPREAD_OUTPUT,
                                         MAX_SPREAD_OUTPUT))
            scores = (utility_mx(value, logit, after).reshape(shape)
                      / args.rank_temp)
            scores_t = (utility_mx(value_t, logit_t, after).reshape(shape)
                        / args.rank_temp)
            log_p_t = scores_t - mx.logsumexp(scores_t, axis=1, keepdims=True)
            log_p = scores - mx.logsumexp(scores, axis=1, keepdims=True)
            loss = loss + args.w_rank * mx.mean(
                mx.sum(mx.exp(log_p_t) * (log_p_t - log_p), axis=1))
        return loss

    grad_fn = mx.value_and_grad(loss_fn)
    optimizer = optim.AdamW(learning_rate=args.lr,
                            weight_decay=args.weight_decay)
    optimizer.init(params)
    state = [optimizer.state]

    def step_fn(params, bits, scalars, value_t, spread_t, after):
        loss, grads = grad_fn(params, bits, scalars, value_t, spread_t, after)
        return loss, optimizer.apply_gradients(grads, params)

    step_fn = mx.compile(step_fn, inputs=state, outputs=state)
    # Without --steps, the cosine spans the time budget: the step rate
    # changes when other jobs share the GPU.
    def progress(step):
        if args.steps:
            return step / args.steps
        return (time.time() - start) / (60 * args.minutes)

    def schedule(step):
        warm = (step + 1) / args.warmup
        cosine = 0.5 * (1 + math.cos(math.pi * min(progress(step), 1.0)))
        return args.lr * min(warm, cosine)

    batch_queue = queue.Queue(maxsize=8)
    stop = threading.Event()
    loaders = [threading.Thread(target=batches, daemon=True,
                                args=(records, args.decisions,
                                      args.per_decision, args.top,
                                      args.seed + idx, batch_queue, stop))
               for idx in range(3)]
    for loader in loaders:
        loader.start()

    def refresh():
        while not stop.wait(60 * args.refresh_minutes):
            added = records.refresh()
            print(f"refresh: +{added} -> {len(records.train)} train "
                  f"decisions", flush=True)

    if args.refresh_minutes > 0:
        threading.Thread(target=refresh, daemon=True).start()

    log = open(f"{args.out}/log.jsonl", "a")
    metrics = evaluate(params, records, args.heads, args.val_decisions)
    metrics.update(step=0)
    print(json.dumps(metrics), flush=True)
    log.write(json.dumps(metrics) + "\n")
    start = time.time()
    step, rows_seen, loss_sum, loss_count = 0, 0, 0.0, 0
    best = None
    done = False
    while not done:
        bits, scalars, value_t, spread_t, after = batch_queue.get()
        optimizer.learning_rate = schedule(step)
        loss, params = step_fn(params, mx.array(bits), mx.array(scalars),
                               mx.array(value_t), mx.array(spread_t),
                               mx.array(after))
        mx.eval(params, optimizer.state, loss)
        step += 1
        rows_seen += len(value_t)
        loss_sum += loss.item()
        loss_count += 1
        done = progress(step) >= 1.0
        if step % args.eval_every == 0 or done:
            metrics = evaluate(params, records, args.heads, args.val_decisions)
            metrics.update(step=step, rows=rows_seen,
                           minutes=(time.time() - start) / 60,
                           train_decisions=len(records.train),
                           train_loss=loss_sum / loss_count,
                           lr=schedule(step))
            loss_sum, loss_count = 0.0, 0
            print(json.dumps(metrics), flush=True)
            log.write(json.dumps(metrics) + "\n")
            log.flush()
            save(params, hparams, args.out, {"distill": metrics})
            if best is None or metrics["regret_utility"] < best:
                best = metrics["regret_utility"]
                save(params, hparams, f"{args.out}/best", {"distill": metrics})
    stop.set()
    metrics = evaluate(params, records, args.heads, args.val_decisions)
    metrics.update(step=step, rows=rows_seen,
                   minutes=(time.time() - start) / 60)
    print("final", json.dumps(metrics), flush=True)
    log.write(json.dumps(metrics) + "\n")
    save(params, hparams, args.out, {"distill": metrics})


if __name__ == "__main__":
    main()
