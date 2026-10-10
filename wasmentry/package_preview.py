#!/usr/bin/env python3
"""Package the Magpie beta; generated engine/data stay out of Git."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
ASSETS = [
    'wasmentry/index.html', 'wasmentry/analysis.css', 'wasmentry/theme.js',
    'wasmentry/peg-review.mjs', 'wasmentry/analysis.mjs', 'wasmentry/analysis-model.mjs', 'wasmentry/move-entry.mjs',
    'wasmentry/device-budget.mjs', 'wasmentry/wmp-assets.mjs', 'wasmentry/wmp-cache.mjs',
    'wasmentry/engine-client.mjs', 'wasmentry/wasm-worker.js',
    'wasmentry/gcg-loader.mjs', 'wasmentry/gcg-loader-worker.js', 'wasmentry/gcg-diagnostics.mjs',
    'wasmentry/isolation.mjs', 'wasmentry/isolation-worker.js',
    'wasmentry/magpie_wasm.mjs', 'wasmentry/magpie_wasm.wasm',
    'data/letterdistributions/english.csv', 'data/layouts/standard15.txt',
    'data/strategy/winpct_english.csv',
]
ASSETS += [f'data/lexica/{lexicon}.{extension}'
           for lexicon in ('CSW24', 'NWL23') for extension in ('kwg', 'klv2')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path, help='New, empty output directory')
    parser.add_argument('--version', help='Release tag, e.g. wasm-preview-v0.1.0')
    parser.add_argument('--with-wmp', action='store_true', help='Include optional WMP downloads built by build_wmp_assets.py')
    args = parser.parse_args()
    assets = list(ASSETS)
    if args.with_wmp:
        wmp_manifest = json.loads((ROOT / 'data/wmp-manifest.json').read_text())
        assets.append('data/wmp-manifest.json')
        for lexicon, entry in wmp_manifest['lexica'].items():
            kwg = ROOT / f'data/lexica/{lexicon}.kwg'
            if lexicon not in ('CSW24', 'NWL23') or hashlib.sha256(kwg.read_bytes()).hexdigest() != entry['kwg_sha256']:
                parser.error('WMP lexicon mismatch; rebuild assets.')
            combined = hashlib.sha256()
            size = 0
            for part in entry['files']:
                asset = 'data/' + part['file']
                if not re.fullmatch(r'data/wmp/(CSW24|NWL23)-[a-f0-9]{64}-\d{2}-gzip\.bin', asset):
                    parser.error('Invalid WMP asset path.')
                data = (ROOT / asset).read_bytes()
                if len(data) != part['bytes'] or hashlib.sha256(data).hexdigest() != part['sha256']:
                    parser.error('WMP checksum mismatch; rebuild assets.')
                unpacked = gzip.decompress(data)
                if len(unpacked) != part['unpacked_bytes']:
                    parser.error('WMP part size mismatch; rebuild assets.')
                combined.update(unpacked)
                size += len(unpacked)
                assets.append(asset)
            if combined.hexdigest() != entry['sha256'] or size != entry['bytes']:
                parser.error('Incomplete WMP assets; rebuild assets.')
    if args.version and not re.fullmatch(r'wasm-preview-v\d+\.\d+\.\d+(?:-[a-z0-9.]+)?', args.version):
        parser.error('Version must be a wasm-preview-vMAJOR.MINOR.PATCH tag.')
    output = args.destination.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('Destination must be empty; existing files will not be removed.')
    missing = [asset for asset in assets if not (ROOT / asset).is_file()]
    if missing:
        parser.error('Build WASM and download data first. Missing: ' + ', '.join(missing))
    for asset in assets:
        target = output / asset
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / asset, target)
    (output / 'index.html').write_text(
        '<!doctype html><html lang="en"><meta charset="utf-8">'
        '<meta http-equiv="refresh" content="0;url=wasmentry/">'
        '<title>Magpie</title><a href="wasmentry/">Open Magpie</a></html>\n')
    shutil.copy2(ROOT / 'wasmentry' / '_headers', output / '_headers')
    shutil.copy2(ROOT / 'wasmentry' / 'serve_preview.py', output / 'serve_preview.py')
    shutil.copy2(ROOT / 'wasmentry' / 'README.md', output / 'README.md')
    if args.version:
        build = json.loads((ROOT / 'wasmentry' / 'release-build.json').read_text())
        manifest = {
            'version': args.version,
            'source_revision': subprocess.check_output(
                ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
            **build,
            'files': {asset: hashlib.sha256((output / asset).read_bytes()).hexdigest()
                      for asset in assets},
        }
        (output / 'release.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Preview packaged in {output}')


if __name__ == '__main__':
    main()
