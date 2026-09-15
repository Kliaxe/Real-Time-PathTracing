#!/usr/bin/env python3
"""Generate and inspect the renderer's project-owned spatiotemporal blue-noise volume."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


DEFAULT_WIDTH = 64
DEFAULT_HEIGHT = 64
DEFAULT_LAYERS = 32
DEFAULT_SIGMA = 1.6
DEFAULT_SEED = 0x52545054

# These operations were selected by minimizing temporal FFT energy in bins 1-4.
# Reordering layers and complementing ranks preserve every layer's spatial
# spectrum exactly, so temporal tuning cannot trade away the spatial result.
LAYER_ORDER = (
    31, 10, 5, 11, 14, 30, 21, 19, 1, 6, 8, 20, 27, 25, 13, 17,
    4, 2, 0, 3, 9, 7, 24, 22, 16, 26, 18, 23, 15, 12, 28, 29,
)
INVERTED_LAYERS = (
    False, True, True, False, True, False, True, False,
    True, True, False, True, True, False, True, False,
    True, False, True, True, False, True, False, True,
    True, False, False, True, False, True, False, True,
)


def _numpy():
    try:
        import numpy as np
    except ImportError as error:
        raise RuntimeError("generation requires NumPy; the renderer build does not") from error
    return np


def _periodic_kernel(width: int, height: int, sigma: float):
    np = _numpy()
    x = np.minimum(np.arange(width), width - np.arange(width))
    y = np.minimum(np.arange(height), height - np.arange(height))
    distance_squared = y[:, None] ** 2 + x[None, :] ** 2
    return np.exp(-distance_squared / (2.0 * sigma * sigma)).astype(np.float32)


def _translated_kernels(kernel):
    np = _numpy()
    height, width = kernel.shape
    translated = np.empty((width * height, width * height), dtype=np.float32)
    for y in range(height):
        for x in range(width):
            translated[y * width + x] = np.roll(kernel, (y, x), axis=(0, 1)).reshape(-1)
    return translated


def _energy(translated, occupied):
    np = _numpy()
    indices = np.flatnonzero(occupied)
    return translated[indices].sum(axis=0, dtype=np.float32)


def _relax(occupied, translated, maximum_iterations: int):
    np = _numpy()
    occupied = occupied.copy()
    energy = _energy(translated, occupied)
    for _ in range(maximum_iterations):
        cluster_scores = np.where(occupied, energy, -np.inf)
        cluster = int(np.argmax(cluster_scores))
        occupied[cluster] = False
        energy -= translated[cluster]

        void_scores = np.where(occupied, np.inf, energy)
        void = int(np.argmin(void_scores))
        if void == cluster:
            occupied[cluster] = True
            energy += translated[cluster]
            return occupied

        occupied[void] = True
        energy += translated[void]
    raise RuntimeError("void-and-cluster relaxation did not converge")


def _rank_layer(seed_pattern, translated):
    np = _numpy()
    texel_count = seed_pattern.size
    midpoint = int(seed_pattern.sum())
    ranks = np.empty(texel_count, dtype=np.uint32)

    occupied = seed_pattern.copy()
    energy = _energy(translated, occupied)
    for rank in range(midpoint - 1, -1, -1):
        cluster = int(np.argmax(np.where(occupied, energy, -np.inf)))
        ranks[cluster] = rank
        occupied[cluster] = False
        energy -= translated[cluster]

    occupied = seed_pattern.copy()
    energy = _energy(translated, occupied)
    for rank in range(midpoint, texel_count):
        void = int(np.argmin(np.where(occupied, np.inf, energy)))
        ranks[void] = rank
        occupied[void] = True
        energy += translated[void]

    return ranks


def generate(width: int, height: int, layers: int, sigma: float, seed: int):
    np = _numpy()
    if width <= 0 or height <= 0 or layers <= 0 or width * height % 256 != 0:
        raise ValueError("dimensions must be positive and each layer must contain a multiple of 256 texels")

    texel_count = width * height
    rng = np.random.default_rng(seed)
    translated = _translated_kernels(_periodic_kernel(width, height, sigma))
    occupied = np.zeros(texel_count, dtype=np.bool_)
    occupied[rng.choice(texel_count, texel_count // 2, replace=False)] = True

    volume = np.empty((layers, height, width), dtype=np.uint8)
    previous_ranks = None
    for layer in range(layers):
        if previous_ranks is not None:
            occupied = np.flip(previous_ranks.reshape(height, width) < texel_count // 2, axis=(0, 1)).reshape(-1)
            # A small deterministic exchange prevents the periodic flip from settling
            # into a two-layer cycle while preserving the previous layer's structure.
            exchange_count = max(1, texel_count // 64)
            ones = rng.choice(np.flatnonzero(occupied), exchange_count, replace=False)
            zeros = rng.choice(np.flatnonzero(~occupied), exchange_count, replace=False)
            occupied[ones] = False
            occupied[zeros] = True

        occupied = _relax(occupied, translated, texel_count * 16)
        previous_ranks = _rank_layer(occupied, translated)
        volume[layer] = ((previous_ranks * 256) // texel_count).astype(np.uint8).reshape(height, width)
        print(f"generated layer {layer + 1}/{layers}", flush=True)

    if (width, height, layers, sigma, seed) == (
        DEFAULT_WIDTH,
        DEFAULT_HEIGHT,
        DEFAULT_LAYERS,
        DEFAULT_SIGMA,
        DEFAULT_SEED,
    ):
        volume = volume[list(LAYER_ORDER)]
        for layer, inverted in enumerate(INVERTED_LAYERS):
            if inverted:
                volume[layer] = 255 - volume[layer]
    return volume


def analyze(data: bytes, width: int, height: int, layers: int) -> tuple[float, float]:
    np = _numpy()
    expected_size = width * height * layers
    if len(data) != expected_size:
        raise ValueError(f"expected {expected_size} bytes, found {len(data)}")
    volume = np.frombuffer(data, dtype=np.uint8).reshape(layers, height, width).astype(np.float64) / 255.0

    spatial = np.fft.fft2(volume - volume.mean(axis=(1, 2), keepdims=True), axes=(1, 2))
    spatial_power = np.abs(spatial) ** 2
    fy = np.fft.fftfreq(height)[:, None]
    fx = np.fft.fftfreq(width)[None, :]
    radius = np.sqrt(fx * fx + fy * fy)
    spatial_low = spatial_power[:, (radius > 0.0) & (radius <= 2.0 / min(width, height))].mean()
    spatial_mid = spatial_power[:, (radius >= 0.20) & (radius <= 0.35)].mean()

    temporal = np.fft.fft(volume - volume.mean(axis=0, keepdims=True), axis=0)
    temporal_power = np.abs(temporal) ** 2
    temporal_low = temporal_power[1:5].mean()
    temporal_mid = temporal_power[layers // 4 : layers // 2].mean()
    return float(spatial_low / spatial_mid), float(temporal_low / temporal_mid)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--layers", type=int, default=DEFAULT_LAYERS)
    parser.add_argument("--sigma", type=float, default=DEFAULT_SIGMA)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=DEFAULT_SEED)
    parser.add_argument("--analyze-only", action="store_true")
    args = parser.parse_args()

    if args.analyze_only:
        data = args.output.read_bytes()
    else:
        volume = generate(args.width, args.height, args.layers, args.sigma, args.seed)
        data = volume.tobytes(order="C")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)

    spatial_ratio, temporal_ratio = analyze(data, args.width, args.height, args.layers)
    print(f"sha256={hashlib.sha256(data).hexdigest()}")
    print(f"spatial_low_to_mid={spatial_ratio:.6f}")
    print(f"temporal_low_to_mid={temporal_ratio:.6f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
