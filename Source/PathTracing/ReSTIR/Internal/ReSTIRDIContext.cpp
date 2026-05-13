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

#include "PathTracing/ReSTIR/ReSTIRDIContext.h"

#include <cassert>
#include <vector>
#include <memory>
#include <numeric>
#include <math.h>

using namespace restir;

namespace restir
{

ReSTIRDIBufferIndices GetDefaultReSTIRDIBufferIndices()
{
    ReSTIRDIBufferIndices bufferIndices = {};
    bufferIndices.initialSamplingOutputBufferIndex = 0;
    bufferIndices.temporalResamplingInputBufferIndex = 0;
    bufferIndices.temporalResamplingOutputBufferIndex = 0;
    bufferIndices.spatialResamplingInputBufferIndex = 0;
    bufferIndices.spatialResamplingOutputBufferIndex = 0;
    bufferIndices.shadingInputBufferIndex = 0;
    return bufferIndices;
}

ReSTIRDIInitialSamplingParameters GetDefaultReSTIRDIInitialSamplingParams()
{
    ReSTIRDIInitialSamplingParameters params = {};
    params.brdfCutoff = 0.0001f;
    params.brdfRayMinT = 0.001f;
    params.enableInitialVisibility = true;
    params.environmentMapImportanceSampling = 1;
    params.numBrdfSamples = 0;
    params.numEnvironmentSamples = 8;
    params.numLocalLightSamples = 24;
    return params;
}

ReSTIRDITemporalResamplingParameters GetDefaultReSTIRDITemporalResamplingParams()
{
    ReSTIRDITemporalResamplingParameters params = {};
    params.maxHistoryLength = 20;
    params.biasCorrectionMode = ReSTIRDI_TemporalBiasCorrectionMode::Basic;
    params.depthThreshold = 0.1f;
    params.normalThreshold = 0.5f;
    params.enableVisibilityShortcut = false;
    params.enablePermutationSampling = true;
    params.uniformRandomNumber = 0;
    return params;
}

ReSTIRDISpatialResamplingParameters GetDefaultReSTIRDISpatialResamplingParams()
{
    ReSTIRDISpatialResamplingParameters params = {};
    params.numDisocclusionBoostSamples = 8;
    params.numSamples = 5;
    params.biasCorrectionMode = ReSTIRDI_SpatialBiasCorrectionMode::Basic;
    params.depthThreshold = 0.1f;
    params.normalThreshold = 0.5f;
    params.samplingRadius = 30.0f;
    params.enableMaterialSimilarityTest = true;
    params.discountNaiveSamples = true;
    params.targetHistoryLength = 0;
    return params;
}

ReSTIRDIShadingParameters GetDefaultReSTIRDIShadingParams()
{
    ReSTIRDIShadingParameters params = {};
    params.enableFinalVisibility = true;
    params.finalVisibilityMaxAge = 4;
    params.finalVisibilityMaxDistance = 16.f;
    params.reuseFinalVisibility = true;
    return params;
}

void debugCheckParameters(const ReSTIRDIStaticParameters& params)
{
    assert(params.RenderWidth > 0);
    assert(params.RenderHeight > 0);
    assert(params.NeighborOffsetCount > 0);
    assert((params.NeighborOffsetCount & (params.NeighborOffsetCount - 1)) == 0);
}

ReSTIRDIContext::ReSTIRDIContext(const ReSTIRDIStaticParameters& params) :
    m_lastFrameOutputReservoir(0),
    m_currentFrameOutputReservoir(0),
    m_staticParams(params),
    m_resamplingMode(ReSTIRDI_ResamplingMode::TemporalAndSpatial),
    m_reservoirBufferParams(CalculateReservoirBufferParameters(params.RenderWidth, params.RenderHeight)),
    m_bufferIndices(GetDefaultReSTIRDIBufferIndices()),
    m_initialSamplingParams(GetDefaultReSTIRDIInitialSamplingParams()),
    m_temporalResamplingParams(GetDefaultReSTIRDITemporalResamplingParams()),
    m_spatialResamplingParams(GetDefaultReSTIRDISpatialResamplingParams()),
    m_shadingParams(GetDefaultReSTIRDIShadingParams())
{
    debugCheckParameters(params);
    m_runtimeParams.neighborOffsetMask = m_staticParams.NeighborOffsetCount - 1;
    UpdateBufferIndices();
}

ReSTIRDI_ResamplingMode ReSTIRDIContext::GetResamplingMode() const
{
    return m_resamplingMode;
}

ReSTIRRuntimeParameters ReSTIRDIContext::GetRuntimeParams() const
{
    return m_runtimeParams;
}

ReSTIRReservoirBufferParameters ReSTIRDIContext::GetReservoirBufferParameters() const
{
    return m_reservoirBufferParams;
}

ReSTIRDIBufferIndices ReSTIRDIContext::GetBufferIndices() const
{
    return m_bufferIndices;
}

ReSTIRDIInitialSamplingParameters ReSTIRDIContext::GetInitialSamplingParameters() const
{
    return m_initialSamplingParams;
}

ReSTIRDITemporalResamplingParameters ReSTIRDIContext::GetTemporalResamplingParameters() const
{
    return m_temporalResamplingParams;
}

ReSTIRDISpatialResamplingParameters ReSTIRDIContext::GetSpatialResamplingParameters() const
{
    return m_spatialResamplingParams;
}

ReSTIRDIShadingParameters ReSTIRDIContext::GetShadingParameters() const
{
    return m_shadingParams;
}

const ReSTIRDIStaticParameters& ReSTIRDIContext::GetStaticParameters() const
{
    return m_staticParams;
}

void ReSTIRDIContext::SetFrameIndex(uint32_t frameIndex)
{
    m_runtimeParams.frameIndex = frameIndex;
    m_temporalResamplingParams.uniformRandomNumber = JenkinsHash(m_runtimeParams.frameIndex);
    m_lastFrameOutputReservoir = m_currentFrameOutputReservoir;
    UpdateBufferIndices();
}

uint32_t ReSTIRDIContext::GetFrameIndex() const
{
    return m_runtimeParams.frameIndex;
}

void ReSTIRDIContext::SetResamplingMode(ReSTIRDI_ResamplingMode resamplingMode)
{
    m_resamplingMode = resamplingMode;
    UpdateBufferIndices();
}

void ReSTIRDIContext::SetInitialSamplingParameters(const ReSTIRDIInitialSamplingParameters& initialSamplingParams)
{
    m_initialSamplingParams = initialSamplingParams;
}

void ReSTIRDIContext::SetTemporalResamplingParameters(const ReSTIRDITemporalResamplingParameters& temporalResamplingParams)
{
    m_temporalResamplingParams = temporalResamplingParams;
    m_temporalResamplingParams.uniformRandomNumber = JenkinsHash(m_runtimeParams.frameIndex);
}

void ReSTIRDIContext::SetSpatialResamplingParameters(const ReSTIRDISpatialResamplingParameters& spatialResamplingParams)
{
    m_spatialResamplingParams = spatialResamplingParams;
}

void ReSTIRDIContext::SetShadingParameters(const ReSTIRDIShadingParameters& shadingParams)
{
    m_shadingParams = shadingParams;
}

void ReSTIRDIContext::UpdateBufferIndices()
{
    const bool useTemporalResampling =
        m_resamplingMode == ReSTIRDI_ResamplingMode::Temporal ||
        m_resamplingMode == ReSTIRDI_ResamplingMode::TemporalAndSpatial;

    const bool useSpatialResampling =
        m_resamplingMode == ReSTIRDI_ResamplingMode::Spatial ||
        m_resamplingMode == ReSTIRDI_ResamplingMode::TemporalAndSpatial;

    m_bufferIndices.initialSamplingOutputBufferIndex = (m_lastFrameOutputReservoir + 1) % c_NumReSTIRDIReservoirBuffers;
    m_bufferIndices.temporalResamplingInputBufferIndex = m_lastFrameOutputReservoir;
    m_bufferIndices.temporalResamplingOutputBufferIndex = (m_bufferIndices.temporalResamplingInputBufferIndex + 1) % c_NumReSTIRDIReservoirBuffers;
    m_bufferIndices.spatialResamplingInputBufferIndex = useTemporalResampling
        ? m_bufferIndices.temporalResamplingOutputBufferIndex
        : m_bufferIndices.initialSamplingOutputBufferIndex;
    m_bufferIndices.spatialResamplingOutputBufferIndex = (m_bufferIndices.spatialResamplingInputBufferIndex + 1) % c_NumReSTIRDIReservoirBuffers;
    m_bufferIndices.shadingInputBufferIndex = useSpatialResampling
        ? m_bufferIndices.spatialResamplingOutputBufferIndex
        : m_bufferIndices.temporalResamplingOutputBufferIndex;
    m_currentFrameOutputReservoir = m_bufferIndices.shadingInputBufferIndex;
}

}
