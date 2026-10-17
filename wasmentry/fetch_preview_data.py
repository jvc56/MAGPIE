#!/usr/bin/env python3
"""Fetch only the preview's data from the release's pinned MAGPIE-DATA commit."""
import json
from urllib.request import urlopen

from package_preview import ASSETS, ROOT


if __name__ == '__main__':
    revision = json.loads((ROOT / 'wasmentry/release-build.json').read_text())['data_revision']
    for asset in ASSETS:
        if not asset.startswith('data/'):
            continue
        target = ROOT / asset
        target.parent.mkdir(parents=True, exist_ok=True)
        with urlopen(f'https://raw.githubusercontent.com/jvc56/MAGPIE-DATA/{revision}/{asset}', timeout=120) as response:
            target.write_bytes(response.read())
        print(asset)
