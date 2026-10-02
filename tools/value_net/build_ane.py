"""Builds the Macondo transformer value net for the Neural Engine following
Apple's ml-ane-transformers layout: activations as (batch, channels, 1,
tokens), 1x1 convolutions for linear layers, LayerNorm over channels, and
per-head attention by einsum with no reshapes or transposes. Weights come
from the raw blob (weights.f32 + manifest.json). Outputs value (P(win) -
P(loss)) and spread (the spread head, tanh-scaled).
Usage: build_ane.py <handoff_dir> <batch> <out.mlpackage>
"""
import json
import sys

import numpy as np
import torch
import torch.nn as nn
import coremltools as ct

handoff, batch, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
manifest = json.load(open(f"{handoff}/manifest.json"))
blob = np.fromfile(f"{handoff}/weights.f32", dtype="<f4")
W = {t["name"]: torch.from_numpy(
    blob[t["offset_floats"]:t["offset_floats"] + t["count"]].copy()
    .reshape(t["shape"])) for t in manifest["tensors"]}
hparams = manifest["hparams"]
D, T, H = hparams["d_model"], 254, hparams["heads"]
HD, FF, LAYERS = D // H, hparams["ff_mult"] * D, hparams["layers"]
HIDDEN = hparams.get("hidden", 128)


def conv(name, in_dim, out_dim):
    layer = nn.Conv2d(in_dim, out_dim, 1)
    layer.weight.data = W[f"{name}.weight"].reshape(out_dim, in_dim, 1, 1)
    layer.bias.data = W[f"{name}.bias"]
    return layer


class LayerNormANE(nn.Module):
    """LayerNorm over the channel axis of (B, C, 1, S)."""

    def __init__(self, name):
        super().__init__()
        self.weight = nn.Parameter(W[f"{name}.weight"].reshape(1, D, 1, 1))
        self.bias = nn.Parameter(W[f"{name}.bias"].reshape(1, D, 1, 1))

    def forward(self, x):
        mean = x.mean(dim=1, keepdim=True)
        centered = x - mean
        variance = (centered * centered).mean(dim=1, keepdim=True)
        return centered * torch.rsqrt(variance + 1e-5) * self.weight + self.bias


class Block(nn.Module):
    def __init__(self, layer):
        super().__init__()
        p = f"blocks.{layer}"
        self.ln1 = LayerNormANE(f"{p}.ln1")
        qkv_w = W[f"{p}.qkv.weight"]
        qkv_b = W[f"{p}.qkv.bias"]
        self.q = nn.Conv2d(D, D, 1)
        self.k = nn.Conv2d(D, D, 1)
        self.v = nn.Conv2d(D, D, 1)
        for idx, layer_conv in enumerate([self.q, self.k, self.v]):
            layer_conv.weight.data = qkv_w[idx * D:(idx + 1) * D].reshape(D, D, 1, 1)
            layer_conv.bias.data = qkv_b[idx * D:(idx + 1) * D]
        self.proj = conv(f"{p}.proj", D, D)
        self.ln2 = LayerNormANE(f"{p}.ln2")
        self.fc1 = conv(f"{p}.fc1", D, FF)
        self.fc2 = conv(f"{p}.fc2", FF, D)

    def forward(self, x):
        h = self.ln1(x)
        q, k, v = self.q(h), self.k(h), self.v(h)
        mh_q = q.split(HD, dim=1)                   # (B, 32, 1, S) each
        mh_k = k.transpose(1, 3).split(HD, dim=3)   # (B, S, 1, 32) each
        mh_v = v.split(HD, dim=1)
        scale = 1.0 / np.sqrt(HD)
        weights = [torch.einsum("bchq,bkhc->bkhq", qi, ki) * scale
                   for qi, ki in zip(mh_q, mh_k)]   # (B, S_k, 1, S_q)
        weights = [w.softmax(dim=1) for w in weights]
        attended = [torch.einsum("bkhq,bchk->bchq", wi, vi)
                    for wi, vi in zip(weights, mh_v)]
        x = x + self.proj(torch.cat(attended, dim=1))
        h = self.fc1(self.ln2(x))
        h = torch.nn.functional.gelu(h)  # exact erf
        return x + self.fc2(h)


class ValueNetANE(nn.Module):
    def __init__(self):
        super().__init__()
        self.square_proj = conv("square_proj", 85, D)
        self.tile_proj = conv("tile_proj", 2, D)
        self.game_proj = conv("game_proj", 72, D)
        self.pos_emb = nn.Parameter(W["pos_emb"][0].T.reshape(1, D, 1, 225))
        self.tile_emb = nn.Parameter(W["tile_emb"][0].T.reshape(1, D, 1, 27))
        self.cls = nn.Parameter(W["cls"][0].T.reshape(1, D, 1, 1))
        self.blocks = nn.ModuleList([Block(layer) for layer in range(LAYERS)])
        self.ln_f = LayerNormANE("ln_f")
        self.fc1 = conv("fc1", D, HIDDEN)
        self.wdl = conv("heads.wdl", HIDDEN, 3)
        self.spread = conv("heads.spread", HIDDEN, 1)

    def forward(self, board, scalars):
        squares = self.square_proj(board.reshape(batch, 85, 1, 225)) + self.pos_emb
        tile_inputs = torch.stack([scalars[:, 0:27], scalars[:, 27:54]], dim=1)
        tiles = self.tile_proj(tile_inputs.reshape(batch, 2, 1, 27)) + self.tile_emb
        game = self.game_proj(scalars.reshape(batch, 72, 1, 1))
        cls = game * 0.0 + self.cls
        x = torch.cat([cls, squares, tiles, game], dim=3)
        for block in self.blocks:
            x = block(x)
        first = x[:, :, :, 0:1]
        hidden = torch.relu(self.fc1(self.ln_f(first)))
        probs = self.wdl(hidden).softmax(dim=1)
        value = (probs[:, 2] - probs[:, 0]).reshape(batch)
        spread = torch.tanh(self.spread(hidden)).reshape(batch)
        return value, spread


net = ValueNetANE().eval()
board = torch.zeros(batch, 85, 225)
scalars = torch.zeros(batch, 72)
# Check the torch model on the parity rows before converting.
pb = torch.from_numpy(np.fromfile(f"{handoff}/parity/board.f32", "<f4")
                      .reshape(64, 85, 225))
ps = torch.from_numpy(np.fromfile(f"{handoff}/parity/scalars.f32", "<f4")
                      .reshape(64, 72))
ref = np.fromfile(f"{handoff}/parity/value.f32", "<f4")
ref_spread = np.fromfile(f"{handoff}/parity/spread.f32", "<f4")
idx = np.arange(batch) % 64
with torch.no_grad():
    check, check_spread = (out.numpy() for out in net(pb[idx], ps[idx]))
print("torch fp32 max_abs_err value", np.abs(check - ref[idx]).max(),
      "spread", np.abs(check_spread - ref_spread[idx]).max())
traced = torch.jit.trace(net, (board, scalars))
model = ct.convert(
    traced,
    inputs=[ct.TensorType(name="board", shape=board.shape),
            ct.TensorType(name="scalars", shape=scalars.shape)],
    outputs=[ct.TensorType(name="value"), ct.TensorType(name="spread")],
    convert_to="mlprogram",
    compute_precision=ct.precision.FLOAT16,
    compute_units=ct.ComputeUnit.CPU_AND_NE,
    minimum_deployment_target=ct.target.macOS15)
model.save(out)
print("saved", out)
