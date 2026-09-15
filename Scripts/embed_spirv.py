#!/usr/bin/env python3
"""Embed a SPIR-V module as a named C++ uint32_t array."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("symbol")
    args = parser.parse_args()

    binary = args.input.read_bytes()
    if len(binary) % 4 != 0:
        raise ValueError(f"SPIR-V byte count is not divisible by four: {args.input}")
    words = struct.unpack(f"<{len(binary) // 4}I", binary)
    if not words or words[0] != 0x07230203:
        raise ValueError(f"SPIR-V magic number is missing: {args.input}")

    lines = ["#pragma once", "", "#include <cstdint>", "", f"inline constexpr uint32_t {args.symbol}[] = {{"]
    for offset in range(0, len(words), 8):
        lines.append("    " + ", ".join(f"0x{word:08x}" for word in words[offset : offset + 8]) + ",")
    lines.extend(["};", ""])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
