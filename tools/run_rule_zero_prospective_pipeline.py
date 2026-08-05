#!/usr/bin/env python3
"""Sequentially prepare, run, and analyze the Rule-of-Zero panel."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys


def _run(command: list[str], repository: Path) -> None:
    result = subprocess.run(command, cwd=repository, check=False)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--shadow-model", required=True, type=Path)
    parser.add_argument("--preregistration", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--panel-wall-seconds", type=float, default=8 * 60 * 60)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parent.parent
    binary = args.binary.resolve()
    shadow_model = args.shadow_model.resolve()
    preregistration = args.preregistration.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    _run(
        [
            sys.executable,
            str(repository / "tools" / "prepare_rule_zero_panel.py"),
            "--binary",
            str(binary),
            "--preregistration",
            str(preregistration),
            "--output-dir",
            str(output_dir),
        ],
        repository,
    )
    frozen = json.loads(
        (output_dir / "frozen-inputs.json").read_text(encoding="utf-8")
    )
    run_dir = output_dir / "validation-run"
    _run(
        [
            sys.executable,
            str(repository / "tools" / "run_rule_zero_panel.py"),
            "--corpus",
            str(frozen["panel"]),
            "--manifest",
            str(frozen["selection_manifest"]),
            "--binary",
            str(binary),
            "--shadow-model",
            str(shadow_model),
            "--preregistration",
            str(preregistration),
            "--run-dir",
            str(run_dir),
            "--wall-seconds",
            str(args.panel_wall_seconds),
        ],
        repository,
    )
    state = json.loads((run_dir / "state.json").read_text(encoding="utf-8"))
    if state.get("status") != "complete":
        raise SystemExit(75)
    _run(
        [
            sys.executable,
            str(repository / "tools" / "analyze_rule_zero_panel.py"),
            str(run_dir / "thinking-curves.log"),
            "--manifest",
            str(frozen["selection_manifest"]),
            "--output",
            str(run_dir / "analysis.json"),
        ],
        repository,
    )


if __name__ == "__main__":
    main()
