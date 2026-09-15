#!/usr/bin/env python3
"""Embed a binary asset as a named C++ uint8_t array."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("symbol")
    parser.add_argument("--expected-size", type=int)
    args = parser.parse_args()

    data = args.input.read_bytes()
    if args.expected_size is not None and len(data) != args.expected_size:
        raise ValueError(f"expected {args.expected_size} bytes, found {len(data)}: {args.input}")

    lines = ["#pragma once", "", "#include <cstdint>", "", f"inline constexpr std::uint8_t {args.symbol}[] = {{"]
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in data[offset : offset + 16]) + ",")
    lines.extend(["};", ""])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
