#pragma once

#include <cstdint>

#include "ReSTIR/PTParameters.h"

namespace nvsamples
{

inline constexpr uint32_t kReSTIRPTReservoirBufferCount = RESTIR_PT_RESERVOIR_BUFFER_COUNT;

// ReSTIR PT can run without reuse, with temporal reuse, with spatial reuse, or
// with both. The mode decides which reservoir arrays become pass inputs and
// outputs. "None" is the correctness baseline: with reuse disabled the renderer
// degenerates to 1spp path tracing through the ReSTIR plumbing, so it must
// converge to the same image as the standalone path tracer.
enum class ReSTIRPTResamplingMode : uint32_t
{
  eNone = 0,
  eTemporal,
  eSpatial,
  eTemporalAndSpatial,
};

// Static dimensions decide buffer layout, so changing these recreates the context.
struct ReSTIRPTStaticParameters
{
  uint32_t renderWidth  = 0;
  uint32_t renderHeight = 0;
};

// Reservoir storage pitches for a given render resolution.
//
// Intentionally duplicates the block-linear math used by ReSTIR DI rather than
// sharing its return type: the two renderers own independent shader ABIs and are
// expected to diverge (PT additionally carries pairing textures and a duplication
// map). Sharing the struct would couple the ABIs for four lines of arithmetic.
ReSTIRPTReservoirBufferParameters CalculateReSTIRPTReservoirBufferParameters(uint32_t renderWidth, uint32_t renderHeight);

ReSTIRPTBufferIndices                GetDefaultReSTIRPTBufferIndices();
ReSTIRPTInitialSamplingParameters    GetDefaultReSTIRPTInitialSamplingParams();
ReSTIRPTShiftParameters              GetDefaultReSTIRPTShiftParams();
ReSTIRPTTemporalResamplingParameters GetDefaultReSTIRPTTemporalResamplingParams();
ReSTIRPTSpatialResamplingParameters  GetDefaultReSTIRPTSpatialResamplingParams();
ReSTIRPTDecorrelationParameters      GetDefaultReSTIRPTDecorrelationParams();
ReSTIRPTShadingParameters            GetDefaultReSTIRPTShadingParams();
ReSTIRPTNeeParameters                GetDefaultReSTIRPTNeeParams();

// Converts the screen-space reuse radius into the pairing texture's standard
// deviation (Section 3). Both sampling schemes are matched on *mean sample
// distance* so paired and unpaired spatial reuse stay comparable:
//   uniform disk of radius r has mean distance 2r/3,
//   isotropic Gaussian with std dev s has mean distance s*sqrt(pi/2),
//   equating them gives s = sqrt(8 / (9*pi)) * r.
// The paper's default r = 30 therefore yields sigma = 16.0.
float CalculateReSTIRPTPairingSigma(float samplingRadius);

// Builds the uniform-buffer parameters consumed by the ReSTIR PT shaders.
// It also owns the reservoir-array rotation, which is why it depends on frame index.
class ReSTIRPTParameterContext
{
public:
  explicit ReSTIRPTParameterContext(const ReSTIRPTStaticParameters& parameters);

  ReSTIRPTReservoirBufferParameters    GetReservoirBufferParameters() const;
  ReSTIRPTResamplingMode               GetResamplingMode() const;
  ReSTIRPTRuntimeParameters            GetRuntimeParameters() const;
  ReSTIRPTBufferIndices                GetBufferIndices() const;
  ReSTIRPTInitialSamplingParameters    GetInitialSamplingParameters() const;
  ReSTIRPTShiftParameters              GetShiftParameters() const;
  ReSTIRPTTemporalResamplingParameters GetTemporalResamplingParameters() const;
  ReSTIRPTSpatialResamplingParameters  GetSpatialResamplingParameters() const;
  ReSTIRPTDecorrelationParameters      GetDecorrelationParameters() const;
  ReSTIRPTShadingParameters            GetShadingParameters() const;
  ReSTIRPTNeeParameters                GetNeeParameters() const;

  uint32_t                        GetFrameIndex() const;
  const ReSTIRPTStaticParameters& GetStaticParameters() const;

  // Call-order contract, because the reservoir rotation depends on it:
  // SetFrameIndex advances the history rotation and must be called EXACTLY ONCE
  // per frame that is actually recorded, before the setters below. Calling it
  // twice, or calling it for a frame that is then skipped, promotes an array that
  // was never written to history and silently produces garbage reuse.
  void SetFrameIndex(uint32_t frameIndex);
  // Safe to call at any point after SetFrameIndex within a frame: it recomputes
  // the indices for the current frame only and never advances the rotation.
  void SetResamplingMode(ReSTIRPTResamplingMode resamplingMode);
  void SetInitialSamplingParameters(const ReSTIRPTInitialSamplingParameters& initialSamplingParameters);
  void SetShiftParameters(const ReSTIRPTShiftParameters& shiftParameters);
  void SetTemporalResamplingParameters(const ReSTIRPTTemporalResamplingParameters& temporalResamplingParameters);
  // Recomputes pairingSigma from samplingRadius; callers set the radius only.
  void SetSpatialResamplingParameters(const ReSTIRPTSpatialResamplingParameters& spatialResamplingParameters);
  void SetDecorrelationParameters(const ReSTIRPTDecorrelationParameters& decorrelationParameters);
  void SetShadingParameters(const ReSTIRPTShadingParameters& shadingParameters);
  void SetNeeParameters(const ReSTIRPTNeeParameters& neeParameters);

private:
  void UpdateBufferIndices();

  // Tracks which reservoir array shaded the previous frame and which will shade this frame.
  uint32_t m_LastFrameOutputReservoir    = 0;
  uint32_t m_CurrentFrameOutputReservoir = 0;
  // Distinguishes "frame 0 has not happened yet" from "frame 0 is current", so the
  // once-per-frame assert does not fire on the very first call.
  bool     m_HasAdvancedOnce             = false;

  ReSTIRPTStaticParameters m_StaticParameters{};
  ReSTIRPTResamplingMode   m_ResamplingMode = ReSTIRPTResamplingMode::eTemporalAndSpatial;
  // These structs are copied almost directly into shaderio::ReSTIRPTParameters.
  ReSTIRPTReservoirBufferParameters m_ReservoirBufferParameters{};
  ReSTIRPTRuntimeParameters         m_RuntimeParameters{};
  ReSTIRPTBufferIndices             m_BufferIndices{};

  ReSTIRPTInitialSamplingParameters    m_InitialSamplingParameters{};
  ReSTIRPTShiftParameters              m_ShiftParameters{};
  ReSTIRPTTemporalResamplingParameters m_TemporalResamplingParameters{};
  ReSTIRPTSpatialResamplingParameters  m_SpatialResamplingParameters{};
  ReSTIRPTDecorrelationParameters      m_DecorrelationParameters{};
  ReSTIRPTShadingParameters            m_ShadingParameters{};
  ReSTIRPTNeeParameters                m_NeeParameters{};
};

}  // namespace nvsamples
