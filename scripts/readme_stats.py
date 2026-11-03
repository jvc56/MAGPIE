#!/usr/bin/env python3
"""Checks, or rewrites, the generated blocks in README.md and docs/building.md.

    python3 scripts/readme_stats.py          # fails if a block is out of date
    python3 scripts/readme_stats.py --write  # rewrites the blocks

Each block sits between <!-- NAME:start --> and <!-- NAME:end --> and holds
facts read from the source alone, so that CI's lint job can check them without
building MAGPIE: the version, the data version, the commands `help` lists, the
builder versions, and the size of the code and its tests.

The sizes are rounded down, to the 10 for the test suites and to the 5,000 for
the lines of C, so that a block changes only when a boundary is crossed, not
with every PR that adds a test. The version, the data version, the command
count and the builder versions are exact: a PR that changes one of them
should say so on the front page. scripts/release.py rewrites the blocks when
it bumps MAGPIE_VERSION.
"""

import argparse
import os
import re
import sys
from typing import Callable, Dict, List, Tuple

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WRITE_COMMAND = "python3 scripts/readme_stats.py --write"

VERSION_FILE = "src/impl/config.c"
DATA_VERSION_FILE = "src/impl/data_fetch.h"
HELP_TABLE_FILE = "src/impl/help_table.c"
BUILDER_FILE = "src/def/builder_defs.h"
TEST_TABLE_FILE = "test/test.c"
SOURCE_DIRS = ("src", "cmd")
TEST_DIRS = ("test",)

TEST_SUITE_ROUNDING = 10
LINE_ROUNDING = 5000


def read(path: str) -> str:
    with open(os.path.join(REPO_ROOT, path), encoding="utf-8") as source:
        return source.read()


def define(path: str, name: str) -> str:
    match = re.search(rf"^#define {name} (\S+)$", read(path), re.MULTILINE)
    if match is None:
        sys.exit(f"readme_stats: no #define {name} in {path}")
    return match.group(1).strip('"')


def command_count() -> int:
    """The commands `help` lists by task, from help_table.c's sections."""
    text = read(HELP_TABLE_FILE)
    sections = re.findall(
        r"static const arg_token_t \w+_commands\[\] = \{(.*?)\};", text, re.DOTALL
    )
    if not sections:
        sys.exit(f"readme_stats: no command sections in {HELP_TABLE_FILE}")
    return sum(len(re.findall(r"\bARG_TOKEN_\w+", section)) for section in sections)


def test_suite_count() -> int:
    """The suites `magpie_test` runs with no arguments, as CI does."""
    match = re.search(
        r"static TestEntry test_table\[\] = \{(.*?)\n\};", read(TEST_TABLE_FILE), re.DOTALL
    )
    if match is None:
        sys.exit(f"readme_stats: no test_table in {TEST_TABLE_FILE}")
    return len(re.findall(r'^\s*\{"', match.group(1), re.MULTILINE))


def line_count(directories: Tuple[str, ...]) -> int:
    total = 0
    for directory in directories:
        for parent, _, filenames in os.walk(os.path.join(REPO_ROOT, directory)):
            for filename in filenames:
                if filename.endswith((".c", ".h")):
                    with open(os.path.join(parent, filename), "rb") as source:
                        total += source.read().count(b"\n")
    return total


def at_least(count: int, rounding: int) -> str:
    return f"{count // rounding * rounding:,}+"


def builder_versions() -> Dict[str, int]:
    return {
        name: int(define(BUILDER_FILE, f"{name.upper()}_BUILDER_VERSION"))
        for name in ("wmp", "rit", "klv", "wit")
    }


def stats_block() -> List[str]:
    version = define(VERSION_FILE, "MAGPIE_VERSION")
    data_version = define(DATA_VERSION_FILE, "DATA_FETCH_VERSION")
    builders = builder_versions()
    columns = (
        ("Version", version),
        ("Data set", f"`data-{data_version}`"),
        ("Commands", str(command_count())),
        ("Test suites in CI", at_least(test_suite_count(), TEST_SUITE_ROUNDING)),
        ("Lines of C", at_least(line_count(SOURCE_DIRS), LINE_ROUNDING)),
        ("Lines of tests", at_least(line_count(TEST_DIRS), LINE_ROUNDING)),
        (
            "Builder versions",
            " · ".join(f"{name.upper()} {value}" for name, value in builders.items()),
        ),
    )
    return [
        "| " + " | ".join(label for label, _ in columns) + " |",
        "|" + "---|" * len(columns),
        "| " + " | ".join(value for _, value in columns) + " |",
    ]


def builders_block() -> List[str]:
    builders = builder_versions()
    json = (
        f'{{"magpie_version":"{define(VERSION_FILE, "MAGPIE_VERSION")}",'
        '"build_target":"nehalem",'
        + ",".join(f'"{name}_builder_version":{value}' for name, value in builders.items())
        + "}"
    )
    return ["```", "magpie> builders", json, "```"]


BLOCKS: Tuple[Tuple[str, str, Callable[[], List[str]]], ...] = (
    ("README.md", "stats", stats_block),
    ("docs/building.md", "builders", builders_block),
)


def render(path: str, name: str, lines: List[str]) -> str:
    text = read(path)
    pattern = re.compile(
        rf"(<!-- {name}:start -->\n).*?(<!-- {name}:end -->)", re.DOTALL
    )
    note = f"<!-- Generated by scripts/readme_stats.py: run `{WRITE_COMMAND}`. -->"
    replacement = "\n".join([note, *lines]) + "\n"
    rendered, replacements = pattern.subn(
        lambda match: match.group(1) + replacement + match.group(2), text
    )
    if replacements != 1:
        sys.exit(f"readme_stats: expected one {name} block in {path}")
    return rendered


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write", action="store_true", help="rewrite the blocks")
    arguments = parser.parse_args()
    stale = []
    for path, name, block in BLOCKS:
        rendered = render(path, name, block())
        if rendered == read(path):
            continue
        if arguments.write:
            with open(os.path.join(REPO_ROOT, path), "w", encoding="utf-8") as target:
                target.write(rendered)
            print(f"Rewrote the {name} block in {path}")
        else:
            stale.append(f"{path} ({name})")
    if stale:
        print(f"Out of date: {', '.join(stale)}. Run `{WRITE_COMMAND}`.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
