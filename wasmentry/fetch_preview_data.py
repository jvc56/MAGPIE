#!/usr/bin/env python3
"""Fetch only the preview's data from the release's pinned MAGPIE-DATA commit."""
import hashlib
import json
from urllib.request import urlopen

from package_preview import ASSETS, ROOT


if __name__ == '__main__':
    build = json.loads((ROOT / 'wasmentry/release-build.json').read_text())
    revision = build['data_revision']
    for asset in ASSETS:
        if not asset.startswith('data/'):
            continue
        target = ROOT / asset
        target.parent.mkdir(parents=True, exist_ok=True)
        source = build.get('additional_data', {}).get(asset)
        url = source['url'] if source else f'https://raw.githubusercontent.com/jvc56/MAGPIE-DATA/{revision}/{asset}'
        with urlopen(url, timeout=120) as response:
            data = response.read()
        if source and hashlib.sha256(data).hexdigest() != source['sha256']:
            raise ValueError(f'Checksum mismatch for {asset}')
        target.write_bytes(data)
        print(asset)
