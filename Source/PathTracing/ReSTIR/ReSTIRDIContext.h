/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#pragma once

#include <stdint.h>

#include "PathTracing/ReSTIR/ReSTIRUtils.h"
#include "ReSTIR/Parameters.h"

namespace restir
{
    static constexpr uint32_t c_NumReSTIRDIReservoirBuffers = 3;

    enum class ReSTIRDI_ResamplingMode : uint32_t
    {
        None,
        Temporal,
        Spatial,
        TemporalAndSpatial
    };

    // Parameters used to initialize the ReSTIRDIContext
    // Changing any of these requires recreating the context.
    struct ReSTIRDIStaticParameters
    {
        uint32_t NeighborOffsetCount = 8192;
        uint32_t RenderWidth = 0;
        uint32_t RenderHeight = 0;
    };

    ReSTIRDIBufferIndices GetDefaultReSTIRDIBufferIndices();
    ReSTIRDIInitialSamplingParameters GetDefaultReSTIRDIInitialSamplingParams();
    ReSTIRDITemporalResamplingParameters GetDefaultReSTIRDITemporalResamplingParams();
    ReSTIRDISpatialResamplingParameters GetDefaultReSTIRDISpatialResamplingParams();
    ReSTIRDIShadingParameters GetDefaultReSTIRDIShadingParams();

    // This context owns the static DI setup and updates the dynamic parameters per frame.
    class ReSTIRDIContext
    {
    public:
        ReSTIRDIContext(const ReSTIRDIStaticParameters& params);

        ReSTIRReservoirBufferParameters GetReservoirBufferParameters() const;
        ReSTIRDI_ResamplingMode GetResamplingMode() const;
        ReSTIRRuntimeParameters GetRuntimeParams() const;
        ReSTIRDIBufferIndices GetBufferIndices() const;
        ReSTIRDIInitialSamplingParameters GetInitialSamplingParameters() const;
        ReSTIRDITemporalResamplingParameters GetTemporalResamplingParameters() const;
        ReSTIRDISpatialResamplingParameters GetSpatialResamplingParameters() const;
        ReSTIRDIShadingParameters GetShadingParameters() const;

        uint32_t GetFrameIndex() const;
        const ReSTIRDIStaticParameters& GetStaticParameters() const;

        void SetFrameIndex(uint32_t frameIndex);
        void SetResamplingMode(ReSTIRDI_ResamplingMode resamplingMode);
        void SetInitialSamplingParameters(const ReSTIRDIInitialSamplingParameters& initialSamplingParams);
        void SetTemporalResamplingParameters(const ReSTIRDITemporalResamplingParameters& temporalResamplingParams);
        void SetSpatialResamplingParameters(const ReSTIRDISpatialResamplingParameters& spatialResamplingParams);
        void SetShadingParameters(const ReSTIRDIShadingParameters& shadingParams);

    private:
        uint32_t m_lastFrameOutputReservoir;
        uint32_t m_currentFrameOutputReservoir;

        ReSTIRDIStaticParameters m_staticParams;

        ReSTIRDI_ResamplingMode m_resamplingMode;
        ReSTIRReservoirBufferParameters m_reservoirBufferParams;
        ReSTIRRuntimeParameters m_runtimeParams;
        ReSTIRDIBufferIndices m_bufferIndices;
        
        ReSTIRDIInitialSamplingParameters m_initialSamplingParams;
        ReSTIRDITemporalResamplingParameters m_temporalResamplingParams;
        ReSTIRDISpatialResamplingParameters m_spatialResamplingParams;
        ReSTIRDIShadingParameters m_shadingParams;

        void UpdateBufferIndices();
    };
}
