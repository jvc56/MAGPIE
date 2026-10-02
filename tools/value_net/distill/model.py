"""The Macondo transformer value net (macondo-nn-tf) in PyTorch, with its
size as parameters, so one class holds the teacher (d_model 192, 8 layers,
6 heads) and smaller students. Parameter names match the teacher's
manifest.json, so load_blob reads the teacher's weights.f32 and save_blob
writes a student in the same format for MAGPIE.

Forward pass (see models/<net>/README.md): 254 tokens (cls, 225 squares,
27 tile types, one game token), pre-norm blocks, then ln_f(cls) -> fc1
(ReLU) -> heads.wdl (softmax: value = P(win) - P(loss)) and heads.spread
(tanh).
"""

import json
import math

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

PLANES, SQUARES, SCALARS, TILE_TYPES = 85, 225, 72, 27
BOARD_FLOATS = PLANES * SQUARES
TOKENS = 1 + SQUARES + TILE_TYPES + 1


class Block(nn.Module):
    def __init__(self, d, heads, ff):
        super().__init__()
        self.heads = heads
        self.ln1 = nn.LayerNorm(d)
        self.qkv = nn.Linear(d, 3 * d)
        self.proj = nn.Linear(d, d)
        self.ln2 = nn.LayerNorm(d)
        self.fc1 = nn.Linear(d, ff)
        self.fc2 = nn.Linear(ff, d)

    def forward(self, x):
        batch, tokens, d = x.shape
        # qkv is laid out [q|k|v][head][head_dim].
        qkv = self.qkv(self.ln1(x)).reshape(batch, tokens, 3, self.heads,
                                            d // self.heads)
        q, k, v = qkv.permute(2, 0, 3, 1, 4)
        attended = F.scaled_dot_product_attention(q, k, v)
        x = x + self.proj(attended.transpose(1, 2).reshape(batch, tokens, d))
        return x + self.fc2(F.gelu(self.fc1(self.ln2(x))))


class ValueNet(nn.Module):
    def __init__(self, d_model=192, layers=8, heads=6, ff_mult=4, hidden=128):
        super().__init__()
        d = d_model
        self.hparams = dict(d_model=d_model, layers=layers, heads=heads,
                            ff_mult=ff_mult, hidden=hidden)
        self.square_proj = nn.Linear(PLANES, d)
        self.pos_emb = nn.Parameter(torch.randn(1, SQUARES, d) * 0.02)
        self.tile_proj = nn.Linear(2, d)
        self.tile_emb = nn.Parameter(torch.randn(1, TILE_TYPES, d) * 0.02)
        self.game_proj = nn.Linear(SCALARS, d)
        self.cls = nn.Parameter(torch.randn(1, 1, d) * 0.02)
        self.blocks = nn.ModuleList(
            [Block(d, heads, ff_mult * d) for _ in range(layers)])
        self.ln_f = nn.LayerNorm(d)
        self.fc1 = nn.Linear(d, hidden)
        self.heads = nn.ModuleDict({"wdl": nn.Linear(hidden, 3),
                                    "spread": nn.Linear(hidden, 1)})
        for block in self.blocks:
            # GPT-2 style: residual projections scaled by depth.
            for layer in (block.proj, block.fc2):
                nn.init.normal_(layer.weight, std=0.02 / math.sqrt(2 * layers))

    def forward(self, board, scalars):
        """board (B, 85, 225) or (B, 85, 15, 15), scalars (B, 72) ->
        value (B,), spread (B,), wdl logits (B, 3)."""
        batch = board.shape[0]
        squares = self.square_proj(
            board.reshape(batch, PLANES, SQUARES).transpose(1, 2))
        squares = squares + self.pos_emb
        tiles = torch.stack((scalars[:, :TILE_TYPES],
                             scalars[:, TILE_TYPES:2 * TILE_TYPES]), dim=-1)
        tiles = self.tile_proj(tiles) + self.tile_emb
        game = self.game_proj(scalars).unsqueeze(1)
        cls = self.cls + torch.zeros_like(game)
        x = torch.cat([cls, squares, tiles, game], dim=1)
        for block in self.blocks:
            x = block(x)
        hidden = F.relu(self.fc1(self.ln_f(x[:, 0])))
        logits = self.heads["wdl"](hidden)
        probs = logits.float().softmax(dim=-1)
        value = probs[:, 2] - probs[:, 0]
        spread = torch.tanh(self.heads["spread"](hidden).float()).squeeze(-1)
        return value, spread, logits


def _blob_name(name):
    # ModuleDict keys read heads.wdl.weight, as in the teacher's manifest.
    return name


def load_blob(model_dir):
    """The teacher (or any net saved by save_blob) from weights.f32 and
    manifest.json; tensors the class does not use (training-only heads) are
    skipped."""
    manifest = json.load(open(f"{model_dir}/manifest.json"))
    hp = manifest["hparams"]
    net = ValueNet(hp["d_model"], hp["layers"], hp["heads"], hp["ff_mult"],
                   hp.get("hidden", 128))
    blob = np.fromfile(f"{model_dir}/weights.f32", dtype="<f4")
    state = net.state_dict()
    loaded = set()
    for tensor in manifest["tensors"]:
        name = tensor["name"]
        if name not in state:
            continue
        values = blob[tensor["offset_floats"]:
                      tensor["offset_floats"] + tensor["count"]]
        state[name] = torch.from_numpy(values.copy()).reshape(state[name].shape)
        loaded.add(name)
    missing = set(state) - loaded
    if missing:
        raise ValueError(f"{model_dir} lacks {sorted(missing)}")
    net.load_state_dict(state)
    return net


def save_blob(net, model_dir, extra=None):
    """Writes net as weights.f32 + manifest.json (the teacher's format)."""
    tensors, chunks, offset = [], [], 0
    for name, value in net.state_dict().items():
        array = value.detach().float().cpu().numpy().astype("<f4").ravel()
        tensors.append({"name": _blob_name(name), "shape": list(value.shape),
                        "offset_floats": offset, "count": int(array.size)})
        chunks.append(array)
        offset += array.size
    np.concatenate(chunks).tofile(f"{model_dir}/weights.f32")
    manifest = {
        "arch": "transformer",
        "hparams": net.hparams,
        "dtype": "float32 little-endian, row-major (C order), one blob",
        "total_floats": int(offset),
        "inputs": {"board": [PLANES, 15, 15], "scalars": [SCALARS]},
        "served_outputs": {
            "value": "P(win) - P(loss) from softmax(heads.wdl) [loss, draw, win]",
            "spread": "tanh(heads.spread)"},
        "tensors": tensors,
    }
    if extra:
        manifest.update(extra)
    json.dump(manifest, open(f"{model_dir}/manifest.json", "w"), indent=1)


if __name__ == "__main__":
    # Parity: the teacher in this class against its reference outputs.
    import sys
    model_dir = sys.argv[1]
    net = load_blob(model_dir).eval()
    rows = 64
    board = torch.from_numpy(np.fromfile(f"{model_dir}/parity/board.f32", "<f4")
                             .reshape(rows, PLANES, SQUARES))
    scalars = torch.from_numpy(np.fromfile(f"{model_dir}/parity/scalars.f32",
                                           "<f4").reshape(rows, SCALARS))
    ref_value = np.fromfile(f"{model_dir}/parity/value.f32", "<f4")
    ref_spread = np.fromfile(f"{model_dir}/parity/spread.f32", "<f4")
    for device in ["cpu", "mps"]:
        with torch.no_grad():
            value, spread, _ = net.to(device)(board.to(device),
                                              scalars.to(device))
        print(device, "max |value diff|",
              np.abs(value.cpu().numpy() - ref_value).max(),
              "max |spread diff|", np.abs(spread.cpu().numpy() - ref_spread).max())
