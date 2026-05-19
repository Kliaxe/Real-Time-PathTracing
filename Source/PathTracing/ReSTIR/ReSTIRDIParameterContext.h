#pragma once

#include <cstdint>

#include "PathTracing/ReSTIR/ReSTIRUtils.h"
#include "ReSTIR/Parameters.h"

namespace nvsamples
{

inline constexpr uint32_t kReSTIRDIReservoirBufferCount = 3;

// ReSTIR DI can run without reuse, with temporal reuse, with spatial reuse, or with both.
enum class ReSTIRDIResamplingMode : uint32_t
{
  eNone = 0,
  eTemporal,
  eSpatial,
  eTemporalAndSpatial,
};

// Static dimensions decide buffer layout, so changing these recreates the context.
struct ReSTIRDIStaticParameters
{
  uint32_t neighborOffsetCount = 16;
  uint32_t renderWidth         = 0;
  uint32_t renderHeight        = 0;
};

ReSTIRDIBufferIndices GetDefaultReSTIRDIBufferIndices();
ReSTIRDIInitialSamplingParameters GetDefaultReSTIRDIInitialSamplingParams();
ReSTIRDITemporalResamplingParameters GetDefaultReSTIRDITemporalResamplingParams();
ReSTIRDISpatialResamplingParameters GetDefaultReSTIRDISpatialResamplingParams();
ReSTIRDIShadingParameters GetDefaultReSTIRDIShadingParams();

// Builds the uniform-buffer parameters consumed by the ReSTIR DI shaders.
class ReSTIRDIParameterContext
{
public:
  explicit ReSTIRDIParameterContext(const ReSTIRDIStaticParameters& parameters);

  ReSTIRReservoirBufferParameters GetReservoirBufferParameters() const;
  ReSTIRDIResamplingMode GetResamplingMode() const;
  ReSTIRRuntimeParameters GetRuntimeParameters() const;
  ReSTIRDIBufferIndices GetBufferIndices() const;
  ReSTIRDIInitialSamplingParameters GetInitialSamplingParameters() const;
  ReSTIRDITemporalResamplingParameters GetTemporalResamplingParameters() const;
  ReSTIRDISpatialResamplingParameters GetSpatialResamplingParameters() const;
  ReSTIRDIShadingParameters GetShadingParameters() const;

  uint32_t GetFrameIndex() const;
  const ReSTIRDIStaticParameters& GetStaticParameters() const;

  void SetFrameIndex(uint32_t frameIndex);
  void SetResamplingMode(ReSTIRDIResamplingMode resamplingMode);
  void SetInitialSamplingParameters(const ReSTIRDIInitialSamplingParameters& initialSamplingParameters);
  void SetTemporalResamplingParameters(const ReSTIRDITemporalResamplingParameters& temporalResamplingParameters);
  void SetSpatialResamplingParameters(const ReSTIRDISpatialResamplingParameters& spatialResamplingParameters);
  void SetShadingParameters(const ReSTIRDIShadingParameters& shadingParameters);

private:
  void UpdateBufferIndices();

  uint32_t m_LastFrameOutputReservoir    = 0;
  uint32_t m_CurrentFrameOutputReservoir = 0;

  ReSTIRDIStaticParameters m_StaticParameters{};
  ReSTIRDIResamplingMode   m_ResamplingMode = ReSTIRDIResamplingMode::eTemporalAndSpatial;
  ReSTIRReservoirBufferParameters m_ReservoirBufferParameters{};
  ReSTIRRuntimeParameters         m_RuntimeParameters{};
  ReSTIRDIBufferIndices           m_BufferIndices{};

  ReSTIRDIInitialSamplingParameters    m_InitialSamplingParameters{};
  ReSTIRDITemporalResamplingParameters m_TemporalResamplingParameters{};
  ReSTIRDISpatialResamplingParameters  m_SpatialResamplingParameters{};
  ReSTIRDIShadingParameters            m_ShadingParameters{};
};

}  // namespace nvsamples
