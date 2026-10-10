#!/usr/bin/env python3
"""Build optional, versioned browser WMP downloads from the packaged KWGs."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--threads', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.threads <= 14:
        parser.error('Threads must be 1–14.')
    manifest = {'schema': 1, 'wmp_version': 3, 'board_dim': 15, 'lexica': {}}
    for lexicon in ('CSW24', 'NWL23'):
        scratch = tempfile.TemporaryDirectory(prefix='magpie-wmp-')
        scratch_path = Path(scratch.name)
        (scratch_path / 'lexica').mkdir()
        (scratch_path / 'lexica' / f'{lexicon}.kwg').symlink_to(ROOT / f'data/lexica/{lexicon}.kwg')
        for directory in ('letterdistributions', 'layouts', 'strategy'):
            (scratch_path / directory).symlink_to(ROOT / 'data' / directory)
        subprocess.run([str(ROOT / 'bin/magpie'), 'convert', 'dawg2wordmap',
                        lexicon, '-path', str(scratch_path), '-wmp', 'false', '-threads', str(args.threads)],
                       cwd=ROOT, check=True)
        source = scratch_path / f'lexica/{lexicon}.wmp'
        checksum = digest(source)
        files = []
        # Small static assets work on hosts with per-file upload limits too.
        # Reassembly is checked against the hash of the complete WMP.
        with source.open('rb') as stream:
            while chunk := stream.read(16 * 1024 * 1024):
                relative = f'wmp/{lexicon}-{checksum}-{len(files):02d}-gzip.bin'
                target = ROOT / 'data' / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                compressed = gzip.compress(chunk, compresslevel=6, mtime=0)
                target.write_bytes(compressed)
                files.append({'file': relative, 'bytes': len(compressed),
                              'unpacked_bytes': len(chunk),
                              'sha256': hashlib.sha256(compressed).hexdigest()})
        manifest['lexica'][lexicon] = {
            'files': files, 'compression': 'gzip', 'bytes': source.stat().st_size,
            'sha256': checksum,
            'kwg_sha256': digest(ROOT / f'data/lexica/{lexicon}.kwg'),
        }
        scratch.cleanup()
    (ROOT / 'data/wmp-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
