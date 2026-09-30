#!/usr/bin/env python3
"""Verify preserved archive bytes without extracting or running experiments."""
import hashlib
import json
from pathlib import Path
import tarfile


def digest_stream(stream):
    digest = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(chunk)
    return digest.hexdigest()


def main():
    root = Path(__file__).resolve().parent
    manifest = json.loads((root / "ARCHIVE_MANIFEST.json").read_text())
    total = 0
    for name, record in manifest["archives"].items():
        path = root / record["file"]
        with path.open("rb") as stream:
            assert digest_stream(stream) == record["sha256"], name
        seen = set()
        with tarfile.open(path, "r:gz") as archive:
            for entry in archive:
                assert entry.isfile() and entry.name not in seen, entry.name
                seen.add(entry.name)
                expected = record["members"][entry.name]
                assert entry.size == expected["bytes"], entry.name
                with archive.extractfile(entry) as stream:
                    assert digest_stream(stream) == expected["sha256"], entry.name
        assert seen == set(record["members"]), name
        total += len(seen)
        print(f"{name}: verified {len(seen)} files")
    print(f"Verified {total} preserved files")


if __name__ == "__main__":
    main()
