/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef RESTIR_RANDOM_SAMPLER_PER_PASS_SEEDS_HLSLI
#define RESTIR_RANDOM_SAMPLER_PER_PASS_SEEDS_HLSLI

#define RESTIR_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED 1
#define RESTIR_DI_TEMPORAL_RESAMPLING_RANDOM_SEED 2
#define RESTIR_DI_SPATIAL_RESAMPLING_RANDOM_SEED 3

// Used for resampling from ReSTIR DI buffers on secondary hits
//  when they lie inside the original camera view.
#define RESTIR_SECONDARY_DI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED 5
#define RESTIR_SECONDARY_DI_SPATIAL_RESAMPLING_RANDOM_SEED 6

#define RESTIR_GI_GENERATE_INITIAL_SAMPLES_RANDOM_SEED 11
#define RESTIR_GI_TEMPORAL_RESAMPLING_RANDOM_SEED 12
#define RESTIR_GI_SPATIAL_RESAMPLING_RANDOM_SEED 13
#define RESTIR_GI_SPATIOTEMPORAL_RESAMPLING_RANDOM_SEED 14

#define RESTIR_PT_GENERATE_INITIAL_SAMPLES_RANDOM_SEED 21
#define RESTIR_PT_GENERATE_INITIAL_SAMPLES_REPLAY_RANDOM_SEED 22
#define RESTIR_PT_TEMPORAL_RESAMPLING_RANDOM_SEED 23
#define RESTIR_PT_SPATIAL_RESAMPLING_RANDOM_SEED 24

#define RESTIR_RANDOM_SAMPLER_PRIME_CONSTANT 31

#endif // RESTIR_RANDOM_SAMPLER_PER_PASS_SEEDS_HLSLI
