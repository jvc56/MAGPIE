"""Adds an opponent-leave head (rackjoint.py's head.npz, an MLP with side
inputs) to a copy of a student model directory, in the tensors and hparams
MAGPIE's value_net.c reads: rack_head.fc.weight [units, hidden + 31],
rack_head.fc.bias, rack_head.out.weight [27, units], rack_head.out.bias, and
hparams rack_head_units. The trunk, its weights and any ane.mlpackage are
copied unchanged, so the head must have been trained on this trunk (a
frozen-trunk run, or the run's own saved student).

    python export_rack_head.py --student runs/t6-so --head rack/A_frozen_side \
        --out runs/t6-so-rack
"""

import argparse
import json
import os
import shutil

import numpy as np

SIDE = 31
LETTERS = 27


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--student", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    summary = json.load(open(f"{args.head}/summary.json"))["args"]
    if summary["kind"] != "mlp" or not summary["side"]:
        raise SystemExit("needs an MLP head with side inputs")
    head = dict(np.load(f"{args.head}/head.npz"))
    manifest = json.load(open(f"{args.student}/manifest.json"))
    hidden = manifest["hparams"].get("hidden", 128)
    units = head["fc/weight"].shape[0]
    assert head["fc/weight"].shape == (units, hidden + SIDE), \
        head["fc/weight"].shape
    assert head["out/weight"].shape == (LETTERS, units)
    blob = np.fromfile(f"{args.student}/weights.f32", dtype="<f4")
    assert blob.size == manifest["total_floats"]
    tensors = [t for t in manifest["tensors"]
               if not t["name"].startswith("rack_head.")]
    chunks = [blob]
    offset = blob.size
    for name, key in [("rack_head.fc.weight", "fc/weight"),
                      ("rack_head.fc.bias", "fc/bias"),
                      ("rack_head.out.weight", "out/weight"),
                      ("rack_head.out.bias", "out/bias")]:
        array = np.ascontiguousarray(head[key], dtype="<f4")
        tensors.append({"name": name, "shape": list(array.shape),
                        "offset_floats": int(offset),
                        "count": int(array.size)})
        chunks.append(array.ravel())
        offset += array.size
    os.makedirs(args.out, exist_ok=True)
    np.concatenate(chunks).astype("<f4").tofile(f"{args.out}/weights.f32")
    manifest["tensors"] = tensors
    manifest["total_floats"] = int(offset)
    manifest["hparams"]["rack_head_units"] = int(units)
    manifest["served_outputs"]["rack_odds"] = (
        "log odds per tile type (blank first) that a tile of that type is "
        "among those the opponent kept from their last move, against a "
        "uniform draw from the unseen tiles; from the head's hidden vector "
        "and 31 side inputs about that move")
    manifest["rack_head"] = {"source": os.path.abspath(args.head),
                             "trained": summary}
    json.dump(manifest, open(f"{args.out}/manifest.json", "w"), indent=1)
    for extra in ["ane.mlpackage", "parity"]:
        source = f"{args.student}/{extra}"
        if os.path.exists(source) and not os.path.exists(f"{args.out}/{extra}"):
            shutil.copytree(source, f"{args.out}/{extra}")
    print(f"wrote {args.out}: {units}-unit rack head, {offset} floats")


if __name__ == "__main__":
    main()
