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

#include <cstdint>

#include "ReSTIR/Common/ReSTIRUtils.h"
#include "ReSTIR/GI/ReSTIRGIParameters.h"

namespace restir
{

static constexpr uint32_t c_NumReSTIRGIReservoirBuffers = 2;

enum class ReSTIRGI_ResamplingMode : uint32_t
{
  None,
  Temporal,
  Spatial,
  TemporalAndSpatial,
};

struct ReSTIRGIStaticParameters
{
  uint32_t NeighborOffsetCount = 8192;
  uint32_t RenderWidth         = 0;
  uint32_t RenderHeight        = 0;

  CheckerboardMode CheckerboardSamplingMode = CheckerboardMode::Off;
};

ReSTIRGIBufferIndices                 GetDefaultReSTIRGIBufferIndices();
ReSTIRGITemporalResamplingParameters  GetDefaultReSTIRGITemporalResamplingParams();
ReSTIRGISpatialResamplingParameters   GetDefaultReSTIRGISpatialResamplingParams();
ReSTIRGIShadingParameters             GetDefaultReSTIRGIShadingParams();

class ReSTIRGIContext
{
public:
  explicit ReSTIRGIContext(const ReSTIRGIStaticParameters& params);

  const ReSTIRGIStaticParameters& GetStaticParameters() const;
  uint32_t                        GetFrameIndex() const;
  ReSTIRReservoirBufferParameters GetReservoirBufferParameters() const;
  ReSTIRRuntimeParameters         GetRuntimeParameters() const;
  ReSTIRGI_ResamplingMode         GetResamplingMode() const;
  ReSTIRGIBufferIndices           GetBufferIndices() const;
  ReSTIRGITemporalResamplingParameters GetTemporalResamplingParameters() const;
  ReSTIRGISpatialResamplingParameters  GetSpatialResamplingParameters() const;
  ReSTIRGIShadingParameters            GetShadingParameters() const;

  void SetFrameIndex(uint32_t frameIndex);
  void SetResamplingMode(ReSTIRGI_ResamplingMode resamplingMode);
  void SetTemporalResamplingParameters(const ReSTIRGITemporalResamplingParameters& temporalResamplingParameters);
  void SetSpatialResamplingParameters(const ReSTIRGISpatialResamplingParameters& spatialResamplingParameters);
  void SetShadingParameters(const ReSTIRGIShadingParameters& shadingParameters);

private:
  void UpdateBufferIndices();
  void UpdateCheckerboardField();

  ReSTIRGIStaticParameters         m_StaticParameters{};
  uint32_t                         m_FrameIndex = 0;
  ReSTIRReservoirBufferParameters  m_ReservoirBufferParameters{};
  ReSTIRRuntimeParameters          m_RuntimeParameters{};
  ReSTIRGI_ResamplingMode          m_ResamplingMode = ReSTIRGI_ResamplingMode::None;
  ReSTIRGIBufferIndices            m_BufferIndices{};
  ReSTIRGITemporalResamplingParameters m_TemporalResamplingParameters{};
  ReSTIRGISpatialResamplingParameters  m_SpatialResamplingParameters{};
  ReSTIRGIShadingParameters            m_ShadingParameters{};
};

}  // namespace restir
