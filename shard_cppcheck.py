#!/usr/bin/env python3
"""Splits cppcheck.sh's first pass into shards for CI, and collects results.

CI checks a share of the files in each of several jobs, copies each job's
results into one --cppcheck-build-dir, and runs cppcheck.sh over everything
from it. That run takes every file's results from the merged directory (they
are keyed by a hash of the file as preprocessed, as in any incremental run)
and runs the whole-program checks, which only see files in the same run, over
all of them. A file a shard misses is analyzed there, so a shard can only
make the run slower, never miss a finding.

The files are the .c files under the given directories, in the order
cppcheck lists them: the directories in turn, each one's files sorted by
path. cppcheck names each file's results in the build dir after its
basename, numbering files that share one in that order, so those files go to
one shard in that order and get the names a run over everything gives them.
Files are weighted by their size plus that of the project headers they
include, directly or not, which on this tree splits within a few percent of
measured check times, and dealt heaviest first to the lightest shard.

Usage:
  python3 shard_cppcheck.py list INDEX COUNT DIR...
      Prints shard INDEX's (1 to COUNT) files, one per line, for --file-list.
  python3 shard_cppcheck.py collect BUILD_DIR OUT_DIR
      Copies the results of the files the last run in BUILD_DIR checked (the
      ones its files.txt names) into OUT_DIR.
"""

import os
import re
import shutil
import sys

INCLUDE_LINE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def source_files(dirs):
    files = []
    for top in dirs:
        found = []
        for dirpath, _, filenames in os.walk(top):
            for filename in filenames:
                if filename.endswith(".c"):
                    found.append(os.path.join(dirpath, filename))
        files.extend(sorted(found))
    return files


def included_headers(path, cache):
    """Every project header path includes, directly or not."""
    if path in cache:
        return cache[path]
    cache[path] = set()
    with open(path, errors="replace") as handle:
        text = handle.read()
    headers = set()
    for name in INCLUDE_LINE.findall(text):
        header = os.path.normpath(os.path.join(os.path.dirname(path), name))
        if os.path.isfile(header):
            headers.add(header)
            headers |= included_headers(header, cache)
    cache[path] = headers
    return headers


def basename_key(path):
    return os.path.splitext(os.path.basename(path))[0]


def shard_files(index, count, dirs):
    files = source_files(dirs)
    cache = {}
    group_weight = {}
    for path in files:
        weight = os.path.getsize(path) + sum(
            os.path.getsize(header) for header in included_headers(path, cache)
        )
        key = basename_key(path)
        group_weight[key] = group_weight.get(key, 0) + weight
    loads = [0] * count
    shard_of = {}
    for key in sorted(group_weight, key=lambda key: (-group_weight[key], key)):
        shard = loads.index(min(loads))
        loads[shard] += group_weight[key]
        shard_of[key] = shard
    return [path for path in files if shard_of[basename_key(path)] == index - 1]


def collect(build_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(build_dir, "files.txt")) as handle:
        for line in handle:
            result_name = line.split(":", 1)[0]
            # X.aN holds the file's results and findings, X.sN its function
            # summaries, when cppcheck wrote any.
            summary_name = re.sub(r"\.a([0-9]+)$", r".s\1", result_name)
            for name in (result_name, summary_name):
                path = os.path.join(build_dir, name)
                if os.path.isfile(path):
                    shutil.copy2(path, out_dir)


def main():
    args = sys.argv[1:]
    if len(args) >= 4 and args[0] == "list":
        index, count = int(args[1]), int(args[2])
        if not 1 <= index <= count:
            sys.exit(f"shard {index} is not between 1 and {count}")
        for path in shard_files(index, count, args[3:]):
            print(path)
    elif len(args) == 3 and args[0] == "collect":
        collect(args[1], args[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
