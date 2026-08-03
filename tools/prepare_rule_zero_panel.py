#!/usr/bin/env python3
"""Resumably generate and freeze the fresh Rule-of-Zero source panel."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

try:
    from tools.extract_time_value_positions import fields, read_complete_games
    from tools.merge_time_value_positions import merge
    from tools.run_stratified_thinking_curve_panel import sha256
    from tools.select_matched_sim_tail_panel import select_tail_panel
except ModuleNotFoundError:  # Direct execution from tools/.
    from extract_time_value_positions import (  # type: ignore[no-redef]
        fields,
        read_complete_games,
    )
    from merge_time_value_positions import merge  # type: ignore[no-redef]
    from run_stratified_thinking_curve_panel import sha256  # type: ignore[no-redef]
    from select_matched_sim_tail_panel import (  # type: ignore[no-redef]
        select_tail_panel,
    )


def _position_count(path: Path, policy: str) -> int:
    count = 0
    with path.open(encoding="utf-8") as stream:
        for expected, line in enumerate(stream):
            if not line.startswith("TIME_VALUE_POSITION "):
                raise ValueError(f"{path} contains a non-position row")
            row = fields(line)
            if int(row["position"]) != expected or row["trajectory_policy"] != policy:
                raise ValueError(f"{path} is not a canonical {policy} corpus")
            count += 1
    if count == 0:
        raise ValueError(f"{path} has no positions")
    return count


def _validate_chunk(log: Path, corpus: Path, games: int, policy: str) -> dict[str, object]:
    reconciled = read_complete_games([log])
    if len(reconciled) != games:
        raise ValueError(f"{log} reconciles {len(reconciled)} games, expected {games}")
    return {
        "log": str(log),
        "log_sha256": sha256(log),
        "corpus": str(corpus),
        "corpus_sha256": sha256(corpus),
        "games": games,
        "positions": _position_count(corpus, policy),
        "trajectory_policy": policy,
    }


def _generate_chunk(
    *,
    repository: Path,
    binary: Path,
    output_dir: Path,
    policy: str,
    clock_ms: int,
    play_analyzed_moves: bool,
    games: int,
    seed: int,
    threads: int,
    chunk_index: int,
) -> dict[str, object]:
    stem = f"{policy}-chunk-{chunk_index:02d}-seed-{seed}"
    log = output_dir / f"{stem}.log"
    corpus = output_dir / f"{stem}.positions"
    if log.exists() and corpus.exists():
        return _validate_chunk(log, corpus, games, policy)
    if log.exists() != corpus.exists():
        raise ValueError(f"partial accepted generation chunk exists for {stem}")
    partial_log = output_dir / f"{stem}.partial.log"
    partial_corpus = output_dir / f"{stem}.partial.positions"
    partial_log.unlink(missing_ok=True)
    partial_corpus.unlink(missing_ok=True)
    command = [
        sys.executable,
        str(repository / "tools" / "collect_time_value_positions.py"),
        "--binary",
        str(binary),
        "--games",
        str(games),
        "--clock-ms",
        str(clock_ms),
        "--threads",
        str(threads),
        "--seed",
        str(seed),
        "--log",
        str(partial_log),
        "--corpus",
        str(partial_corpus),
    ]
    if play_analyzed_moves:
        command.append("--play-analyzed-moves")
    result = subprocess.run(command, cwd=repository, check=False)
    if result.returncode != 0:
        raise SystemExit(result.returncode)
    _validate_chunk(partial_log, partial_corpus, games, policy)
    partial_log.replace(log)
    partial_corpus.replace(corpus)
    return _validate_chunk(log, corpus, games, policy)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--preregistration", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    repository = Path(__file__).resolve().parent.parent
    binary = args.binary.resolve()
    preregistration_path = args.preregistration.resolve()
    output_dir = args.output_dir.resolve()
    if not binary.is_file() or not preregistration_path.is_file():
        parser.error("binary and preregistration must exist")
    preregistration = json.loads(preregistration_path.read_text(encoding="utf-8"))
    if (
        preregistration.get("artifact_kind")
        != "rule_zero_prospective_preregistration"
        or not preregistration.get("frozen_before_fresh_trajectory_generation")
    ):
        parser.error("invalid prospective preregistration")
    generation = preregistration["fresh_trajectory_generation"]
    games_per_policy = int(generation["games_per_policy"])
    games_per_chunk = int(generation["games_per_resumable_chunk"])
    threads = int(generation["threads"])
    if games_per_policy % games_per_chunk != 0:
        parser.error("generation chunks do not divide policy target")
    output_dir.mkdir(parents=True, exist_ok=True)
    generation_dir = output_dir / "generation-chunks"
    generation_dir.mkdir(exist_ok=True)
    chunks: list[dict[str, object]] = []
    corpora: list[Path] = []
    policies = (
        ("static", generation["static"]),
        ("playchooser-g3000ms", generation["playchooser"]),
    )
    for policy, settings in policies:
        for chunk_index in range(games_per_policy // games_per_chunk):
            seed = int(settings["base_seed"]) + 1000 * chunk_index
            record = _generate_chunk(
                repository=repository,
                binary=binary,
                output_dir=generation_dir,
                policy=policy,
                clock_ms=int(settings["clock_ms"]),
                play_analyzed_moves=bool(settings["play_analyzed_moves"]),
                games=games_per_chunk,
                seed=seed,
                threads=threads,
                chunk_index=chunk_index,
            )
            chunks.append(record)
            corpora.append(Path(str(record["corpus"])))

    merged = output_dir / "fresh-all-320.positions"
    merge_manifest = output_dir / "fresh-all-320.manifest.json"
    merge_document = merge(corpora, merged, merge_manifest)
    if int(merge_document["games"]) != 2 * games_per_policy:
        raise ValueError("merged fresh corpus has the wrong game count")

    exclusions: list[Path] = []
    for item in preregistration["exclusions"]:
        path = (repository / str(item["path"])).resolve()
        if sha256(path) != item["sha256"]:
            raise ValueError(f"exclusion manifest hash changed: {path}")
        exclusions.append(path)
    panel = output_dir / "rule-zero-320.positions"
    selection_manifest = output_dir / "selection-manifest.json"
    selection = select_tail_panel(
        merged,
        panel,
        selection_manifest,
        {"static": games_per_policy, "playchooser-g3000ms": games_per_policy},
        int(preregistration["panel"]["selection_seed"]),
        exclusions,
    )
    expected_roots = int(preregistration["panel"]["roots"])
    if int(selection["roots"]) != expected_roots:
        raise ValueError("selected prospective panel has the wrong root count")
    expected_per_stratum = int(
        preregistration["panel"]["roots_per_policy_bag_stratum"]
    )
    if set(selection["strata_selected"].values()) != {expected_per_stratum}:
        raise ValueError("selected prospective panel is not exactly balanced")

    frozen = {
        "artifact_kind": "rule_zero_prospective_frozen_inputs",
        "schema_version": 1,
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "preregistration": str(preregistration_path),
        "preregistration_sha256": sha256(preregistration_path),
        "generation_chunks": chunks,
        "merged_corpus": str(merged),
        "merged_corpus_sha256": sha256(merged),
        "merge_manifest": str(merge_manifest),
        "merge_manifest_sha256": sha256(merge_manifest),
        "panel": str(panel),
        "panel_sha256": sha256(panel),
        "selection_manifest": str(selection_manifest),
        "selection_manifest_sha256": sha256(selection_manifest),
        "roots": expected_roots,
        "source_games": expected_roots,
    }
    frozen_path = output_dir / "frozen-inputs.json"
    frozen_path.write_text(
        json.dumps(frozen, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(frozen, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
