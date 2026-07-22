#!/usr/bin/env python3

import argparse
import hashlib
from pathlib import Path
import platform
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parent
SOURCE_DIRS = ("src", "test", "cmd")
SOURCE_SUFFIXES = (".c", ".h")
REQUIRED_KEYS = (
    "source_fingerprint",
    "compiler_major",
    "host_machine",
    "board_dim",
    "rack_size",
)


def source_files():
    files = [ROOT / "Makefile"]
    for directory in SOURCE_DIRS:
        files.extend(
            path
            for path in (ROOT / directory).rglob("*")
            if path.is_file() and path.suffix in SOURCE_SUFFIXES
        )
    return sorted(files, key=lambda path: path.relative_to(ROOT).as_posix())


def source_fingerprint():
    digest = hashlib.sha256()
    for path in source_files():
        relative_path = path.relative_to(ROOT).as_posix().encode()
        contents = path.read_bytes()
        digest.update(len(relative_path).to_bytes(8, "big"))
        digest.update(relative_path)
        digest.update(len(contents).to_bytes(8, "big"))
        digest.update(contents)
    return digest.hexdigest()


def compiler_major(compiler):
    result = subprocess.run(
        [compiler, "--version"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    version_line = result.stdout.splitlines()[0]
    match = re.search(r"clang version ([0-9]+)", version_line)
    if match is None:
        raise RuntimeError(f"could not determine Clang major version: {version_line}")
    return match.group(1)


def expected_metadata(args):
    return {
        "source_fingerprint": source_fingerprint(),
        "compiler_major": compiler_major(args.compiler),
        "host_machine": platform.machine(),
        "board_dim": str(args.board_dim),
        "rack_size": str(args.rack_size),
    }


def read_metadata(path):
    if not path.is_file():
        raise RuntimeError(f"PGO metadata not found: {path}")
    metadata = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator:
            raise RuntimeError(f"invalid PGO metadata line: {line}")
        metadata[key] = value
    return metadata


def record(args):
    metadata = expected_metadata(args)
    args.metadata.parent.mkdir(parents=True, exist_ok=True)
    contents = "".join(f"{key}={value}\n" for key, value in metadata.items())
    args.metadata.write_text(contents, encoding="utf-8")
    print(f"Recorded PGO provenance in {args.metadata}")


def verify(args):
    actual = read_metadata(args.metadata)
    expected = expected_metadata(args)
    missing = [key for key in REQUIRED_KEYS if key not in actual]
    mismatches = [
        key for key in REQUIRED_KEYS if key in actual and actual[key] != expected[key]
    ]
    if missing or mismatches:
        details = []
        if missing:
            details.append(f"missing fields: {', '.join(missing)}")
        for key in mismatches:
            details.append(
                f"{key}: profile={actual[key]!r}, current={expected[key]!r}"
            )
        raise RuntimeError(
            "refusing stale or incompatible PGO profile (" + "; ".join(details) + ")"
        )
    print(f"Verified current PGO provenance in {args.metadata}")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Record or verify the inputs associated with a Magpie PGO profile."
    )
    parser.add_argument("mode", choices=("record", "verify"))
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--board-dim", type=int, required=True)
    parser.add_argument("--rack-size", type=int, required=True)
    return parser.parse_args()


def main():
    args = parse_args()
    args.metadata = args.metadata.resolve()
    try:
        if args.mode == "record":
            record(args)
        else:
            verify(args)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
