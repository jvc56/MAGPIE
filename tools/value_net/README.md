# Value net tools

`build_ane.py` builds the CoreML model that MAGPIE's Neural Engine backend
(`src/impl/value_net_coreml.m`, player backends `ane` and `anegpu`) loads
from `<model_dir>/ane.mlpackage`. It reads the same `weights.f32` and
`manifest.json` as the CPU and Metal backends and lays the net out the way
the Neural Engine prefers (Apple's ml-ane-transformers recipe):
activations as (batch, channels, 1, tokens), 1x1 convolutions for linear
layers, LayerNorm over channels, and per-head attention by einsum. Its
outputs are value and spread (the spread head, which MAGPIE's utility
uses). Before converting it checks the PyTorch model against the parity
rows in fp32.

```sh
python3.13 -m venv ~/sources/nn-venv   # coremltools has no 3.14 build yet
~/sources/nn-venv/bin/pip install coremltools torch numpy
~/sources/nn-venv/bin/python tools/value_net/build_ane.py <model_dir> 8 \
    <model_dir>/ane.mlpackage
```

The batch size (8) is fixed in the model; the backend splits larger
requests into chunks of it. On an M4, batches of 8 to 16 run fastest (about
2,250 rows/s from one caller; larger batches are slower), and the
converted model matches the reference values within 1.4e-3 and
spreads within 1.3e-3 (fp16).

Measured on an M4 (4-ply sims of 15 root plays, 10 threads, value net
replies on the first rollout ply among the top K static replies):

| backend | rows/s | K = 15 | K = 8 |
|---|---:|---:|---:|
| Metal fp16 (`fp16`) | ~1,000 | 68 it/s | 130 it/s |
| Neural Engine (`ane`) | ~2,200 | 142 it/s | 275 it/s |
| both (`anegpu`) | ~2,600 | 175 it/s | 363 it/s |

On the `claude/value-net` branch (not for main), the net César shared is
committed under `models/macondo-nn-tf-nwl23s-v1/`: his handoff files
(`weights.f32`, `manifest.json`, `README.md`, `MD5SUMS`, `parity/`) and the
Neural Engine build (`ane.mlpackage`, batch 8), so that directory can be
passed as `<model_dir>` directly, e.g.

```sh
./bin/magpie_test valuenet:parity:models/macondo-nn-tf-nwl23s-v1:models/macondo-nn-tf-nwl23s-v1/parity
```
