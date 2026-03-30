#include <ReSTIR/GI/ReSTIRGI.h>

#include <cassert>

namespace restir
{

ReSTIRGIBufferIndices GetDefaultReSTIRGIBufferIndices()
{
  ReSTIRGIBufferIndices bufferIndices = {};
  bufferIndices.initialSamplingOutputBufferIndex    = 0;
  bufferIndices.temporalResamplingInputBufferIndex  = 0;
  bufferIndices.temporalResamplingOutputBufferIndex = 0;
  bufferIndices.spatialResamplingInputBufferIndex   = 0;
  bufferIndices.spatialResamplingOutputBufferIndex  = 0;
  bufferIndices.shadingInputBufferIndex             = 0;
  return bufferIndices;
}

ReSTIRGITemporalResamplingParameters GetDefaultReSTIRGITemporalResamplingParams()
{
  ReSTIRGITemporalResamplingParameters params = {};
  params.depthThreshold            = 0.1f;
  params.normalThreshold           = 0.6f;
  params.maxHistoryLength          = 8u;
  params.enableFallbackSampling    = 1u;
  params.biasCorrectionMode        = ReSTIRGIBiasCorrectionMode::Basic;
  params.maxReservoirAge           = 30u;
  params.enablePermutationSampling = 0u;
  params.uniformRandomNumber       = 0u;
  return params;
}

ReSTIRGISpatialResamplingParameters GetDefaultReSTIRGISpatialResamplingParams()
{
  ReSTIRGISpatialResamplingParameters params = {};
  params.depthThreshold     = 0.1f;
  params.normalThreshold    = 0.6f;
  params.numSamples         = 2u;
  params.samplingRadius     = 32.0f;
  params.biasCorrectionMode = ReSTIRGIBiasCorrectionMode::Basic;
  return params;
}

ReSTIRGIShadingParameters GetDefaultReSTIRGIShadingParams()
{
  ReSTIRGIShadingParameters params = {};
  params.enableFinalVisibility = 1u;
  return params;
}

static void DebugCheckParameters(const ReSTIRGIStaticParameters& params)
{
  assert(params.RenderWidth > 0);
  assert(params.RenderHeight > 0);
}

ReSTIRGIContext::ReSTIRGIContext(const ReSTIRGIStaticParameters& params)
    : m_StaticParameters(params)
    , m_ReservoirBufferParameters(CalculateReservoirBufferParameters(params.RenderWidth, params.RenderHeight, params.CheckerboardSamplingMode))
    , m_BufferIndices(GetDefaultReSTIRGIBufferIndices())
    , m_TemporalResamplingParameters(GetDefaultReSTIRGITemporalResamplingParams())
    , m_SpatialResamplingParameters(GetDefaultReSTIRGISpatialResamplingParams())
    , m_ShadingParameters(GetDefaultReSTIRGIShadingParams())
{
  DebugCheckParameters(params);
  m_RuntimeParameters.neighborOffsetMask = m_StaticParameters.NeighborOffsetCount - 1u;
  UpdateCheckerboardField();
  UpdateBufferIndices();
}

const ReSTIRGIStaticParameters& ReSTIRGIContext::GetStaticParameters() const
{
  return m_StaticParameters;
}

uint32_t ReSTIRGIContext::GetFrameIndex() const
{
  return m_FrameIndex;
}

ReSTIRReservoirBufferParameters ReSTIRGIContext::GetReservoirBufferParameters() const
{
  return m_ReservoirBufferParameters;
}

ReSTIRRuntimeParameters ReSTIRGIContext::GetRuntimeParameters() const
{
  return m_RuntimeParameters;
}

ReSTIRGI_ResamplingMode ReSTIRGIContext::GetResamplingMode() const
{
  return m_ResamplingMode;
}

ReSTIRGIBufferIndices ReSTIRGIContext::GetBufferIndices() const
{
  return m_BufferIndices;
}

ReSTIRGITemporalResamplingParameters ReSTIRGIContext::GetTemporalResamplingParameters() const
{
  return m_TemporalResamplingParameters;
}

ReSTIRGISpatialResamplingParameters ReSTIRGIContext::GetSpatialResamplingParameters() const
{
  return m_SpatialResamplingParameters;
}

ReSTIRGIShadingParameters ReSTIRGIContext::GetShadingParameters() const
{
  return m_ShadingParameters;
}

void ReSTIRGIContext::SetFrameIndex(uint32_t frameIndex)
{
  m_FrameIndex = frameIndex;
  m_RuntimeParameters.frameIndex = frameIndex;
  m_TemporalResamplingParameters.uniformRandomNumber = JenkinsHash(m_FrameIndex);
  UpdateBufferIndices();
  UpdateCheckerboardField();
}

void ReSTIRGIContext::SetResamplingMode(ReSTIRGI_ResamplingMode resamplingMode)
{
  m_ResamplingMode = resamplingMode;
  UpdateBufferIndices();
}

void ReSTIRGIContext::SetTemporalResamplingParameters(const ReSTIRGITemporalResamplingParameters& temporalResamplingParameters)
{
  m_TemporalResamplingParameters = temporalResamplingParameters;
  m_TemporalResamplingParameters.uniformRandomNumber = JenkinsHash(m_FrameIndex);
}

void ReSTIRGIContext::SetSpatialResamplingParameters(const ReSTIRGISpatialResamplingParameters& spatialResamplingParameters)
{
  m_SpatialResamplingParameters = spatialResamplingParameters;
}

void ReSTIRGIContext::SetShadingParameters(const ReSTIRGIShadingParameters& shadingParameters)
{
  m_ShadingParameters = shadingParameters;
}

void ReSTIRGIContext::UpdateBufferIndices()
{
  switch(m_ResamplingMode)
  {
    case ReSTIRGI_ResamplingMode::Temporal:
      m_BufferIndices.initialSamplingOutputBufferIndex    = m_FrameIndex & 1u;
      m_BufferIndices.temporalResamplingInputBufferIndex  = m_BufferIndices.initialSamplingOutputBufferIndex ^ 1u;
      m_BufferIndices.temporalResamplingOutputBufferIndex = m_BufferIndices.initialSamplingOutputBufferIndex;
      m_BufferIndices.spatialResamplingInputBufferIndex   = m_BufferIndices.temporalResamplingOutputBufferIndex;
      m_BufferIndices.spatialResamplingOutputBufferIndex  = m_BufferIndices.temporalResamplingOutputBufferIndex;
      m_BufferIndices.shadingInputBufferIndex             = m_BufferIndices.temporalResamplingOutputBufferIndex;
      break;
    case ReSTIRGI_ResamplingMode::Spatial:
      m_BufferIndices.initialSamplingOutputBufferIndex    = 0u;
      m_BufferIndices.temporalResamplingInputBufferIndex  = 0u;
      m_BufferIndices.temporalResamplingOutputBufferIndex = 0u;
      m_BufferIndices.spatialResamplingInputBufferIndex   = 0u;
      m_BufferIndices.spatialResamplingOutputBufferIndex  = 1u;
      m_BufferIndices.shadingInputBufferIndex             = 1u;
      break;
    case ReSTIRGI_ResamplingMode::TemporalAndSpatial:
      m_BufferIndices.initialSamplingOutputBufferIndex    = 0u;
      m_BufferIndices.temporalResamplingInputBufferIndex  = 1u;
      m_BufferIndices.temporalResamplingOutputBufferIndex = 0u;
      m_BufferIndices.spatialResamplingInputBufferIndex   = 0u;
      m_BufferIndices.spatialResamplingOutputBufferIndex  = 1u;
      m_BufferIndices.shadingInputBufferIndex             = 1u;
      break;
    case ReSTIRGI_ResamplingMode::None:
    default:
      m_BufferIndices.initialSamplingOutputBufferIndex    = 0u;
      m_BufferIndices.temporalResamplingInputBufferIndex  = 0u;
      m_BufferIndices.temporalResamplingOutputBufferIndex = 0u;
      m_BufferIndices.spatialResamplingInputBufferIndex   = 0u;
      m_BufferIndices.spatialResamplingOutputBufferIndex  = 0u;
      m_BufferIndices.shadingInputBufferIndex             = 0u;
      break;
  }
}

void ReSTIRGIContext::UpdateCheckerboardField()
{
  switch(m_StaticParameters.CheckerboardSamplingMode)
  {
    case CheckerboardMode::Black:
      m_RuntimeParameters.activeCheckerboardField = (m_FrameIndex & 1u) ? 1u : 2u;
      break;
    case CheckerboardMode::White:
      m_RuntimeParameters.activeCheckerboardField = (m_FrameIndex & 1u) ? 2u : 1u;
      break;
    case CheckerboardMode::Off:
    default:
      m_RuntimeParameters.activeCheckerboardField = 0u;
      break;
  }
}

}  // namespace restir
