#include "PathTracing/ReSTIR/ReSTIRDIParameterContext.h"

#include <cassert>

namespace nvsamples
{

namespace
{

void CheckStaticParameters(const ReSTIRDIStaticParameters& parameters)
{
  // Shader indexing assumes positive dimensions and a power-of-two neighbor table.
  assert(parameters.renderWidth > 0);
  assert(parameters.renderHeight > 0);
  assert(parameters.neighborOffsetCount > 0);
  assert((parameters.neighborOffsetCount & (parameters.neighborOffsetCount - 1)) == 0);
}

}  // namespace

ReSTIRDIBufferIndices GetDefaultReSTIRDIBufferIndices()
{
  ReSTIRDIBufferIndices bufferIndices{};
  bufferIndices.initialSamplingOutputBufferIndex    = 0;
  bufferIndices.temporalResamplingInputBufferIndex  = 0;
  bufferIndices.temporalResamplingOutputBufferIndex = 0;
  bufferIndices.spatialResamplingInputBufferIndex   = 0;
  bufferIndices.spatialResamplingOutputBufferIndex  = 0;
  bufferIndices.shadingInputBufferIndex             = 0;
  return bufferIndices;
}

ReSTIRDIInitialSamplingParameters GetDefaultReSTIRDIInitialSamplingParams()
{
  // Defaults favor the thesis ReSTIR DI path: many direct-light candidates, no BRDF candidates.
  ReSTIRDIInitialSamplingParameters parameters{};
  parameters.brdfCutoff                      = 0.0001f;
  parameters.brdfRayMinT                     = 0.001f;
  parameters.enableInitialVisibility         = true;
  parameters.environmentMapImportanceSampling = 1;
  parameters.numBrdfSamples                  = 0;
  parameters.numEnvironmentSamples           = 8;
  parameters.numLocalLightSamples            = 24;
  return parameters;
}

ReSTIRDITemporalResamplingParameters GetDefaultReSTIRDITemporalResamplingParams()
{
  // Basic bias correction keeps the implementation focused while avoiding the fastest 1/M mode.
  ReSTIRDITemporalResamplingParameters parameters{};
  parameters.maxHistoryLength         = 20;
  parameters.biasCorrectionMode       = ReSTIRDI_TemporalBiasCorrectionMode::Basic;
  parameters.depthThreshold           = 0.1f;
  parameters.normalThreshold          = 0.5f;
  parameters.enableVisibilityShortcut = false;
  parameters.enablePermutationSampling = true;
  parameters.uniformRandomNumber      = 0;
  return parameters;
}

ReSTIRDISpatialResamplingParameters GetDefaultReSTIRDISpatialResamplingParams()
{
  // Spatial reuse uses a moderate neighbor count and radius suitable for the experiment scenes.
  ReSTIRDISpatialResamplingParameters parameters{};
  parameters.numDisocclusionBoostSamples = 8;
  parameters.numSamples                  = 5;
  parameters.biasCorrectionMode          = ReSTIRDI_SpatialBiasCorrectionMode::Basic;
  parameters.depthThreshold              = 0.1f;
  parameters.normalThreshold             = 0.5f;
  parameters.samplingRadius              = 30.0f;
  parameters.enableMaterialSimilarityTest = true;
  parameters.discountNaiveSamples        = true;
  parameters.targetHistoryLength         = 0;
  return parameters;
}

ReSTIRDIShadingParameters GetDefaultReSTIRDIShadingParams()
{
  // Final visibility reuse trades a few shadow rays for stable direct-light shading.
  ReSTIRDIShadingParameters parameters{};
  parameters.enableFinalVisibility      = true;
  parameters.finalVisibilityMaxAge      = 4;
  parameters.finalVisibilityMaxDistance = 16.0f;
  parameters.reuseFinalVisibility       = true;
  return parameters;
}

ReSTIRDIParameterContext::ReSTIRDIParameterContext(const ReSTIRDIStaticParameters& parameters)
    : m_StaticParameters(parameters)
    , m_ReservoirBufferParameters(CalculateReservoirBufferParameters(parameters.renderWidth, parameters.renderHeight))
    , m_BufferIndices(GetDefaultReSTIRDIBufferIndices())
    , m_InitialSamplingParameters(GetDefaultReSTIRDIInitialSamplingParams())
    , m_TemporalResamplingParameters(GetDefaultReSTIRDITemporalResamplingParams())
    , m_SpatialResamplingParameters(GetDefaultReSTIRDISpatialResamplingParams())
    , m_ShadingParameters(GetDefaultReSTIRDIShadingParams())
{
  CheckStaticParameters(parameters);
  // The shader masks random neighbor indices, so neighborOffsetCount must be a power of two.
  m_RuntimeParameters.neighborOffsetMask = m_StaticParameters.neighborOffsetCount - 1;
  UpdateBufferIndices();
}

ReSTIRDIResamplingMode ReSTIRDIParameterContext::GetResamplingMode() const
{
  return m_ResamplingMode;
}

ReSTIRRuntimeParameters ReSTIRDIParameterContext::GetRuntimeParameters() const
{
  return m_RuntimeParameters;
}

ReSTIRReservoirBufferParameters ReSTIRDIParameterContext::GetReservoirBufferParameters() const
{
  return m_ReservoirBufferParameters;
}

ReSTIRDIBufferIndices ReSTIRDIParameterContext::GetBufferIndices() const
{
  return m_BufferIndices;
}

ReSTIRDIInitialSamplingParameters ReSTIRDIParameterContext::GetInitialSamplingParameters() const
{
  return m_InitialSamplingParameters;
}

ReSTIRDITemporalResamplingParameters ReSTIRDIParameterContext::GetTemporalResamplingParameters() const
{
  return m_TemporalResamplingParameters;
}

ReSTIRDISpatialResamplingParameters ReSTIRDIParameterContext::GetSpatialResamplingParameters() const
{
  return m_SpatialResamplingParameters;
}

ReSTIRDIShadingParameters ReSTIRDIParameterContext::GetShadingParameters() const
{
  return m_ShadingParameters;
}

const ReSTIRDIStaticParameters& ReSTIRDIParameterContext::GetStaticParameters() const
{
  return m_StaticParameters;
}

void ReSTIRDIParameterContext::SetFrameIndex(uint32_t frameIndex)
{
  m_RuntimeParameters.frameIndex = frameIndex;
  // Temporal permutation sampling needs a deterministic per-frame random integer.
  m_TemporalResamplingParameters.uniformRandomNumber = JenkinsHash(m_RuntimeParameters.frameIndex);
  // Last-frame output becomes the history input for this frame.
  m_LastFrameOutputReservoir = m_CurrentFrameOutputReservoir;
  UpdateBufferIndices();
}

uint32_t ReSTIRDIParameterContext::GetFrameIndex() const
{
  return m_RuntimeParameters.frameIndex;
}

void ReSTIRDIParameterContext::SetResamplingMode(ReSTIRDIResamplingMode resamplingMode)
{
  m_ResamplingMode = resamplingMode;
  UpdateBufferIndices();
}

void ReSTIRDIParameterContext::SetInitialSamplingParameters(const ReSTIRDIInitialSamplingParameters& initialSamplingParameters)
{
  m_InitialSamplingParameters = initialSamplingParameters;
}

void ReSTIRDIParameterContext::SetTemporalResamplingParameters(const ReSTIRDITemporalResamplingParameters& temporalResamplingParameters)
{
  m_TemporalResamplingParameters = temporalResamplingParameters;
  // Preserve the frame-derived permutation value after copying UI settings.
  m_TemporalResamplingParameters.uniformRandomNumber = JenkinsHash(m_RuntimeParameters.frameIndex);
}

void ReSTIRDIParameterContext::SetSpatialResamplingParameters(const ReSTIRDISpatialResamplingParameters& spatialResamplingParameters)
{
  m_SpatialResamplingParameters = spatialResamplingParameters;
}

void ReSTIRDIParameterContext::SetShadingParameters(const ReSTIRDIShadingParameters& shadingParameters)
{
  m_ShadingParameters = shadingParameters;
}

void ReSTIRDIParameterContext::UpdateBufferIndices()
{
  const bool useTemporalResampling = m_ResamplingMode == ReSTIRDIResamplingMode::eTemporal
                                     || m_ResamplingMode == ReSTIRDIResamplingMode::eTemporalAndSpatial;
  const bool useSpatialResampling = m_ResamplingMode == ReSTIRDIResamplingMode::eSpatial
                                    || m_ResamplingMode == ReSTIRDIResamplingMode::eTemporalAndSpatial;

  // Reservoir buffers rotate so this frame can read history and write a fresh candidate.
  m_BufferIndices.initialSamplingOutputBufferIndex = (m_LastFrameOutputReservoir + 1) % kReSTIRDIReservoirBufferCount;
  m_BufferIndices.temporalResamplingInputBufferIndex = m_LastFrameOutputReservoir;
  // Temporal output is placed in a different array than the previous history input.
  m_BufferIndices.temporalResamplingOutputBufferIndex =
      (m_BufferIndices.temporalResamplingInputBufferIndex + 1) % kReSTIRDIReservoirBufferCount;

  // Spatial reuse consumes either the temporal result or the initial candidate.
  m_BufferIndices.spatialResamplingInputBufferIndex = useTemporalResampling ? m_BufferIndices.temporalResamplingOutputBufferIndex
                                                                            : m_BufferIndices.initialSamplingOutputBufferIndex;
  m_BufferIndices.spatialResamplingOutputBufferIndex =
      (m_BufferIndices.spatialResamplingInputBufferIndex + 1) % kReSTIRDIReservoirBufferCount;
  m_BufferIndices.shadingInputBufferIndex = useSpatialResampling ? m_BufferIndices.spatialResamplingOutputBufferIndex
                                                                 : m_BufferIndices.temporalResamplingOutputBufferIndex;
  // The shading input becomes the previous-frame reservoir after FinishFrame advances.
  m_CurrentFrameOutputReservoir = m_BufferIndices.shadingInputBufferIndex;
}

}  // namespace nvsamples
