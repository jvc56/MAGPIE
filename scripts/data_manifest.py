#!/usr/bin/env python3
"""Writes the manifest `magpie data fetch` downloads a data set from.

Usage:
  python3 scripts/data_manifest.py DATA_DIR BASE_URL [PATH ...] > manifest.json

Lists every file under DATA_DIR (or only the files and directories named, as
paths relative to it) with its URL (BASE_URL + its path), size and SHA-256.
A symlink is listed as a copy of the file it points to, which must be listed
too, so `data fetch` writes a copy (Windows cannot make links without
privilege). Hidden files are skipped.

MAGPIE-DATA publishes the files individually beside the manifest; see
src/impl/data_fetch.h for the format and docs/windows-plan.md, section 6.2.
"""

import hashlib
import json
import os
import sys


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def walk(data_dir, roots):
    for root in roots:
        top = os.path.join(data_dir, root)
        if os.path.isfile(top) or os.path.islink(top):
            yield root
            continue
        for dirpath, dirnames, filenames in os.walk(top):
            dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
            for filename in sorted(filenames):
                if not filename.startswith("."):
                    full = os.path.join(dirpath, filename)
                    yield os.path.relpath(full, data_dir)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data_dir = os.path.abspath(sys.argv[1])
    base_url = sys.argv[2].rstrip("/") + "/"
    roots = sys.argv[3:] or ["."]
    paths = sorted({os.path.normpath(path).replace(os.sep, "/")
                    for path in walk(data_dir, roots)})
    files = []
    for path in paths:
        full = os.path.join(data_dir, path)
        if os.path.islink(full):
            target = os.path.relpath(os.path.realpath(full), data_dir)
            target = target.replace(os.sep, "/")
            if target.startswith("../") or target not in paths:
                sys.exit(f"{path} links to {target}, which is not listed")
            files.append({"path": path, "copy_of": target})
        else:
            files.append({"path": path, "url": base_url + path,
                          "bytes": os.path.getsize(full),
                          "sha256": sha256_of(full)})
    json.dump({"files": files}, sys.stdout, indent=1)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
