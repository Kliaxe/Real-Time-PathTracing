#!/usr/bin/env python3
"""Compare one renderer capture prefix against a matching reference prefix."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any, BinaryIO

import numpy as np
from PIL import Image


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="Capture prefix for the accepted reference")
    parser.add_argument("candidate", type=Path, help="Capture prefix for the candidate")
    parser.add_argument("--output", type=Path, help="Write the JSON report to this path")
    parser.add_argument("--limits", type=Path, help="Optional accepted maximums keyed by image and metric")
    return parser.parse_args()


def read_required_line(source: BinaryIO) -> bytes:
    line = source.readline()
    if not line:
        raise ValueError("unexpected end of Radiance HDR header")
    return line.rstrip(b"\r\n")


def decode_hdr(path: Path) -> np.ndarray:
    with path.open("rb") as source:
        if read_required_line(source) != b"#?RADIANCE":
            raise ValueError(f"unsupported HDR signature: {path}")

        format_seen = False
        while True:
            line = read_required_line(source)
            if not line:
                break
            if line == b"FORMAT=32-bit_rle_rgbe":
                format_seen = True
        if not format_seen:
            raise ValueError(f"unsupported HDR encoding: {path}")

        resolution = read_required_line(source).split()
        if len(resolution) != 4 or resolution[0] != b"-Y" or resolution[2] != b"+X":
            raise ValueError(f"unsupported HDR orientation: {path}")
        height = int(resolution[1])
        width = int(resolution[3])
        if width < 8 or width > 0x7FFF or height <= 0:
            raise ValueError(f"unsupported HDR dimensions: {width}x{height}")

        rgbe = np.empty((height, width, 4), dtype=np.uint8)
        for y in range(height):
            marker = source.read(4)
            if len(marker) != 4 or marker[0:2] != b"\x02\x02":
                raise ValueError(f"invalid HDR scanline marker at row {y}: {path}")
            scanline_width = (marker[2] << 8) | marker[3]
            if scanline_width != width:
                raise ValueError(f"HDR scanline width mismatch at row {y}: {path}")

            for channel in range(4):
                x = 0
                while x < width:
                    packet = source.read(1)
                    if not packet:
                        raise ValueError(f"truncated HDR scanline at row {y}: {path}")
                    count = packet[0]
                    if count > 128:
                        run_length = count - 128
                        value = source.read(1)
                        if not value or run_length == 0 or x + run_length > width:
                            raise ValueError(f"invalid HDR run at row {y}: {path}")
                        rgbe[y, x : x + run_length, channel] = value[0]
                        x += run_length
                    else:
                        literal_length = count
                        values = source.read(literal_length)
                        if literal_length == 0 or len(values) != literal_length or x + literal_length > width:
                            raise ValueError(f"invalid HDR literal at row {y}: {path}")
                        rgbe[y, x : x + literal_length, channel] = np.frombuffer(values, dtype=np.uint8)
                        x += literal_length

    exponent = rgbe[..., 3].astype(np.int32)
    scale = np.zeros(exponent.shape, dtype=np.float32)
    nonzero = exponent != 0
    scale[nonzero] = np.ldexp(np.ones(np.count_nonzero(nonzero), dtype=np.float32), exponent[nonzero] - 136)
    return rgbe[..., :3].astype(np.float32) * scale[..., None]


def decode_png(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGBA"), dtype=np.float32) / 255.0


def load_metadata(prefix: Path) -> dict[str, Any]:
    return json.loads(Path(f"{prefix}.json").read_text(encoding="utf-8"))


def validate_metadata(reference: dict[str, Any], candidate: dict[str, Any]) -> None:
    compared_fields = ("renderer", "scene", "viewport", "frame_count", "camera", "tonemapper")
    mismatches = [field for field in compared_fields if reference.get(field) != candidate.get(field)]
    if mismatches:
        raise ValueError("capture metadata differs in: " + ", ".join(mismatches))


def calculate_metrics(reference: np.ndarray, candidate: np.ndarray) -> dict[str, Any]:
    if reference.shape != candidate.shape:
        raise ValueError(f"image shape mismatch: {reference.shape} != {candidate.shape}")
    if not np.isfinite(reference).all() or not np.isfinite(candidate).all():
        raise ValueError("image contains NaN or Inf values")

    difference = candidate.astype(np.float64) - reference.astype(np.float64)
    absolute = np.abs(difference)
    mean_squared_error = float(np.mean(np.square(difference)))
    reference_energy = float(np.sum(np.abs(reference), dtype=np.float64))
    return {
        "mean_absolute_error": float(np.mean(absolute)),
        "root_mean_squared_error": math.sqrt(mean_squared_error),
        "maximum_absolute_error": float(np.max(absolute)),
        "relative_l1_error": float(np.sum(absolute, dtype=np.float64) / max(reference_energy, 1.0e-20)),
        "mean_bias": float(np.mean(difference)),
        "reference_minimum": float(np.min(reference)),
        "reference_maximum": float(np.max(reference)),
        "candidate_minimum": float(np.min(candidate)),
        "candidate_maximum": float(np.max(candidate)),
        "channel_mean_absolute_error": [float(value) for value in np.mean(absolute, axis=(0, 1))],
    }


def check_limits(metrics: dict[str, dict[str, Any]], limits_path: Path) -> list[str]:
    limits = json.loads(limits_path.read_text(encoding="utf-8"))
    failures: list[str] = []
    for image_name, image_limits in limits.items():
        if image_name not in metrics or not isinstance(image_limits, dict):
            raise ValueError(f"invalid limits image key: {image_name}")
        for metric_name, maximum in image_limits.items():
            actual = metrics[image_name].get(metric_name)
            if not isinstance(maximum, (int, float)) or not isinstance(actual, (int, float)):
                raise ValueError(f"invalid scalar limit: {image_name}.{metric_name}")
            if abs(actual) > maximum:
                failures.append(f"{image_name}.{metric_name}: {actual:.9g} > {maximum:.9g}")
    return failures


def main() -> int:
    args = parse_args()
    reference_metadata = load_metadata(args.reference)
    candidate_metadata = load_metadata(args.candidate)
    validate_metadata(reference_metadata, candidate_metadata)

    metrics = {
        "linear_hdr": calculate_metrics(
            decode_hdr(Path(f"{args.reference}.linear.hdr")),
            decode_hdr(Path(f"{args.candidate}.linear.hdr")),
        ),
        "final_png": calculate_metrics(
            decode_png(Path(f"{args.reference}.final.png")),
            decode_png(Path(f"{args.candidate}.final.png")),
        ),
    }
    failures = check_limits(metrics, args.limits) if args.limits else []
    report = {
        "schema_version": 1,
        "reference": str(args.reference.resolve()),
        "candidate": str(args.candidate.resolve()),
        "metrics": metrics,
        "limits": str(args.limits.resolve()) if args.limits else None,
        "passed": not failures,
        "failures": failures,
    }
    report_text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report_text, encoding="utf-8")
    print(report_text, end="")
    return 0 if not failures else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, ValueError, json.JSONDecodeError) as error:
        print(f"compare_captures.py: error: {error}", file=sys.stderr)
        raise SystemExit(2)
