#!/usr/bin/env python3
"""Run versioned headless capture scenarios and record reproducibility metadata."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Any


RENDERERS = {"raster", "path-tracer", "restir-pt"}
# Command-line spellings mapped to the RenderResolveMode value the renderer records as resolve_mode.
RESOLVE_MODES = {"off": 0, "accumulate": 1, "denoise": 2}


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    default_executable = repo_root / "Binaries" / "Debug" / (
        "RealTimePathTracing.exe" if os.name == "nt" else "RealTimePathTracing"
    )
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scenario-file",
        type=Path,
        default=repo_root / "Tests" / "Scenarios" / "baseline.json",
    )
    parser.add_argument("--executable", type=Path, default=default_executable)
    parser.add_argument("--output-dir", type=Path, default=repo_root / "Artifacts" / "Validation")
    parser.add_argument("--group", help="Run scenarios containing this group")
    parser.add_argument("--scenario", action="append", help="Run only this scenario id; repeatable")
    parser.add_argument("--repeats", type=int, help="Override every scenario repeat count")
    return parser.parse_args()


def require_integer(value: Any, name: str, minimum: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise ValueError(f"{name} must be an integer >= {minimum}")
    return value


def load_scenarios(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1:
        raise ValueError("scenario file schema_version must be 1")

    defaults = document.get("defaults", {})
    scenarios = document.get("scenarios")
    if not isinstance(scenarios, list) or not scenarios:
        raise ValueError("scenario file must contain a non-empty scenarios array")

    seen_ids: set[str] = set()
    for scenario in scenarios:
        scenario_id = scenario.get("id")
        if not isinstance(scenario_id, str) or not scenario_id or scenario_id in seen_ids:
            raise ValueError(f"scenario id must be non-empty and unique: {scenario_id!r}")
        seen_ids.add(scenario_id)
        require_integer(scenario.get("scene_index"), f"{scenario_id}.scene_index", 0)
        require_integer(scenario.get("frames"), f"{scenario_id}.frames", 1)
        if scenario.get("renderer") not in RENDERERS:
            raise ValueError(f"{scenario_id}.renderer must be one of {sorted(RENDERERS)}")
        if "resolve_mode" in scenario and scenario["resolve_mode"] not in RESOLVE_MODES:
            raise ValueError(f"{scenario_id}.resolve_mode must be one of {sorted(RESOLVE_MODES)}")
        if not isinstance(scenario.get("scene_label"), str) or not scenario["scene_label"]:
            raise ValueError(f"{scenario_id}.scene_label must be a non-empty string")
        if "repeats" in scenario:
            require_integer(scenario["repeats"], f"{scenario_id}.repeats", 1)

    return defaults, scenarios


def run_git(repo_root: Path, *arguments: str) -> str | None:
    result = subprocess.run(
        ["git", *arguments], cwd=repo_root, text=True, capture_output=True, check=False
    )
    return result.stdout.strip() if result.returncode == 0 else None


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]
    scenario_path = args.scenario_file.resolve()
    executable = args.executable.resolve()
    if not executable.is_file():
        raise FileNotFoundError(f"renderer executable does not exist: {executable}")
    if args.repeats is not None:
        require_integer(args.repeats, "--repeats", 1)

    defaults, scenarios = load_scenarios(scenario_path)
    requested_ids = set(args.scenario or [])
    known_ids = {scenario["id"] for scenario in scenarios}
    unknown_ids = requested_ids - known_ids
    if unknown_ids:
        raise ValueError(f"unknown scenario ids: {', '.join(sorted(unknown_ids))}")

    selected = [
        scenario
        for scenario in scenarios
        if (not requested_ids or scenario["id"] in requested_ids)
        and (args.group is None or args.group in scenario.get("groups", []))
    ]
    if not selected:
        raise ValueError("scenario selection is empty")

    started = dt.datetime.now(dt.timezone.utc)
    run_id = started.strftime("%Y%m%dT%H%M%SZ")
    run_dir = args.output_dir.resolve() / run_id
    run_dir.mkdir(parents=True, exist_ok=False)

    git_status = run_git(repo_root, "status", "--short")
    manifest: dict[str, Any] = {
        "schema_version": 1,
        "run_id": run_id,
        "started_utc": started.isoformat(),
        "scenario_file": str(scenario_path),
        "scenario_file_sha256": hash_file(scenario_path),
        "executable": str(executable),
        "executable_sha256": hash_file(executable),
        "source": {
            "revision": run_git(repo_root, "rev-parse", "HEAD"),
            "dirty": bool(git_status),
            "status": git_status.splitlines() if git_status else [],
        },
        "host": {
            "platform": platform.platform(),
            "python": platform.python_version(),
        },
        "captures": [],
    }

    for scenario in selected:
        width = require_integer(scenario.get("width", defaults.get("width")), "width", 1)
        height = require_integer(scenario.get("height", defaults.get("height")), "height", 1)
        repeats = args.repeats or scenario.get("repeats", defaults.get("repeats", 1))
        repeats = require_integer(repeats, "repeats", 1)

        for repeat_index in range(repeats):
            capture_name = f"{scenario['id']}.repeat-{repeat_index:02d}"
            capture_prefix = run_dir / capture_name
            resolve_arguments = (
                ["--resolve-mode", scenario["resolve_mode"]] if "resolve_mode" in scenario else []
            )
            command = [
                str(executable),
                "--headless",
                "--frames",
                str(scenario["frames"]),
                "--width",
                str(width),
                "--height",
                str(height),
                "--scene-index",
                str(scenario["scene_index"]),
                "--renderer",
                scenario["renderer"],
                *resolve_arguments,
                "--capture-prefix",
                str(capture_prefix),
                "--validation-sync",
            ]
            log_path = Path(f"{capture_prefix}.log")
            print(f"[{scenario['id']} {repeat_index + 1}/{repeats}]", flush=True)
            completed = subprocess.run(command, cwd=repo_root, text=True, capture_output=True, check=False)
            log_text = completed.stdout + completed.stderr
            log_path.write_text(log_text, encoding="utf-8")
            if completed.returncode != 0:
                raise RuntimeError(
                    f"capture failed with exit code {completed.returncode}; see {log_path}"
                )
            validation_errors = log_text.count("Validation Error")
            synchronization_hazards = log_text.count("SYNC-HAZARD")
            synchronization_validation_confirmed = (
                "Validation layer enabled with synchronization validation" in log_text
            )
            if not synchronization_validation_confirmed:
                raise RuntimeError(f"synchronization validation was not confirmed; see {log_path}")
            if validation_errors or synchronization_hazards:
                raise RuntimeError(
                    f"validation reported {validation_errors} errors and "
                    f"{synchronization_hazards} synchronization hazards; see {log_path}"
                )

            output_paths = {
                "linear_hdr": Path(f"{capture_prefix}.linear.hdr"),
                "final_png": Path(f"{capture_prefix}.final.png"),
                "metadata": Path(f"{capture_prefix}.json"),
                "log": log_path,
            }
            for kind, output_path in output_paths.items():
                if not output_path.is_file() or output_path.stat().st_size == 0:
                    raise RuntimeError(f"missing or empty {kind} output: {output_path}")

            capture_metadata = json.loads(output_paths["metadata"].read_text(encoding="utf-8"))
            if capture_metadata.get("scene", {}).get("label") != scenario["scene_label"]:
                raise RuntimeError(
                    f"scene catalog mismatch for index {scenario['scene_index']}: "
                    f"expected {scenario['scene_label']!r}, got "
                    f"{capture_metadata.get('scene', {}).get('label')!r}"
                )
            if (
                "resolve_mode" in scenario
                and capture_metadata.get("resolve_mode") != RESOLVE_MODES[scenario["resolve_mode"]]
            ):
                raise RuntimeError(
                    f"resolve mode mismatch: expected {scenario['resolve_mode']!r} "
                    f"({RESOLVE_MODES[scenario['resolve_mode']]}), got "
                    f"{capture_metadata.get('resolve_mode')!r}"
                )

            manifest["captures"].append(
                {
                    "scenario": scenario,
                    "repeat_index": repeat_index,
                    "command": command,
                    "exit_code": completed.returncode,
                    "validation": {
                        "synchronization_validation_confirmed": synchronization_validation_confirmed,
                        "validation_error_count": validation_errors,
                        "synchronization_hazard_count": synchronization_hazards,
                    },
                    "outputs": {
                        kind: {
                            "path": str(path.relative_to(run_dir)),
                            "size": path.stat().st_size,
                            "sha256": hash_file(path),
                        }
                        for kind, path in output_paths.items()
                    },
                    "renderer_metadata": capture_metadata,
                }
            )
            (run_dir / "manifest.json").write_text(
                json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
            )

    manifest["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
    (run_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(run_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"capture_baseline.py: error: {error}", file=sys.stderr)
        raise SystemExit(1)
