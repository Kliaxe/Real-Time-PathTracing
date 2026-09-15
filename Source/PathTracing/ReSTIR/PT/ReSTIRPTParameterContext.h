#pragma once

#include <cstdint>

#include "ReSTIR/PTParameters.h"

namespace rtpt
{

// Number of reservoir arrays the parameter context rotates between; see RESTIR_PT_RESERVOIR_BUFFER_COUNT for why two are enough.
inline constexpr uint32_t kReSTIRPTReservoirBufferCount = RESTIR_PT_RESERVOIR_BUFFER_COUNT;

// ReSTIRPTResamplingMode
// ReSTIR PT can run without reuse, with temporal reuse, with spatial reuse, or with both. The mode decides which reservoir arrays become pass inputs and outputs.
// "None" is the correctness baseline: with reuse disabled the renderer degenerates to 1spp path tracing through the ReSTIR plumbing, so it must converge to the same image as the standalone path tracer.

enum class ReSTIRPTResamplingMode : uint32_t
{
  eNone = 0,
  eTemporal,
  eSpatial,
  eTemporalAndSpatial,
};

// ReSTIRPTStaticParameters
// Dimensions that decide the reservoir buffer layout. Changing them recreates the parameter context.

struct ReSTIRPTStaticParameters
{
  // Render resolution in pixels. Must be non-zero; the constructor asserts it.
  uint32_t renderWidth  = 0;
  uint32_t renderHeight = 0;
};

// Reservoir storage pitches for a given render resolution. Reservoirs are stored block-linear, so the pitches are measured in whole blocks rather than pixels.
ReSTIRPTReservoirBufferParameters CalculateReSTIRPTReservoirBufferParameters(uint32_t renderWidth, uint32_t renderHeight);

// Default parameter blocks
// The paper's configuration, or the locally measured replacement where one exists. The measurements behind each value are recorded next to it in ReSTIRPTParameterContext.cpp.

ReSTIRPTBufferIndices                GetDefaultReSTIRPTBufferIndices();
ReSTIRPTInitialSamplingParameters    GetDefaultReSTIRPTInitialSamplingParams();
ReSTIRPTShiftParameters              GetDefaultReSTIRPTShiftParams();
ReSTIRPTTemporalResamplingParameters GetDefaultReSTIRPTTemporalResamplingParams();
ReSTIRPTSpatialResamplingParameters  GetDefaultReSTIRPTSpatialResamplingParams();
ReSTIRPTDecorrelationParameters      GetDefaultReSTIRPTDecorrelationParams();
ReSTIRPTShadingParameters            GetDefaultReSTIRPTShadingParams();
ReSTIRPTNeeParameters                GetDefaultReSTIRPTNeeParams();

// Pairing sigma
// Converts the screen-space reuse radius into the pairing texture's standard deviation (Section 3).
// Both sampling schemes are matched on *mean sample distance* so paired and unpaired spatial reuse stay comparable: a uniform disk of radius r has mean distance 2r/3, an isotropic Gaussian with std dev s has mean distance s*sqrt(pi/2), and equating them gives s = sqrt(8 / (9*pi)) * r.
// The paper's default r = 30 therefore yields sigma = 16.0.

float CalculateReSTIRPTPairingSigma(float samplingRadius);

// ReSTIRPTParameterContext
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

  // Call-order contract, because the reservoir rotation depends on it.
  // SetFrameIndex advances the history rotation and must be called EXACTLY ONCE per frame that is actually recorded, before the setters below. Calling it twice, or calling it for a frame that is then skipped, promotes an array that was never written to history and silently produces garbage reuse.
  void SetFrameIndex(uint32_t frameIndex);

  // Safe to call at any point after SetFrameIndex within a frame: it recomputes the indices for the current frame only and never advances the rotation.
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

  // Reservoir array that shaded the previous frame. It becomes this frame's temporal history input.
  uint32_t m_LastFrameOutputReservoir    = 0;
  // Reservoir array that will shade this frame. SetFrameIndex promotes it to m_LastFrameOutputReservoir.
  uint32_t m_CurrentFrameOutputReservoir = 0;
  // Distinguishes "frame 0 has not happened yet" from "frame 0 is current", so the once-per-frame assert does not fire on the very first call.
  bool     m_HasAdvancedOnce             = false;

  // Resolution the reservoir layout was computed for.
  ReSTIRPTStaticParameters m_StaticParameters {};
  // Decides which passes read and write which reservoir arrays.
  ReSTIRPTResamplingMode   m_ResamplingMode = ReSTIRPTResamplingMode::eTemporalAndSpatial;

  // Shader parameter blocks
  // These structs are copied almost directly into shaderio::ReSTIRPTParameters.
  // The user-facing blocks are stored as last set and returned unchanged, except for the derived pairing sigma.

  // Block-linear addressing pitches derived from m_StaticParameters.
  ReSTIRPTReservoirBufferParameters m_ReservoirBufferParameters {};
  // Frame index and its per-frame hash.
  ReSTIRPTRuntimeParameters         m_RuntimeParameters {};
  // Which reservoir array each pass reads and writes this frame.
  ReSTIRPTBufferIndices             m_BufferIndices {};

  // Path length, roulette, and environment importance sampling.
  ReSTIRPTInitialSamplingParameters    m_InitialSamplingParameters {};
  // Shift mapping and reconnection criteria (Section 4).
  ReSTIRPTShiftParameters              m_ShiftParameters {};
  // Temporal reuse cap and reprojection gates.
  ReSTIRPTTemporalResamplingParameters m_TemporalResamplingParameters {};
  // Spatial reuse radius, pairing, and similarity gates (Section 3).
  ReSTIRPTSpatialResamplingParameters  m_SpatialResamplingParameters {};
  // Duplication-map cap reduction (Section 5).
  ReSTIRPTDecorrelationParameters      m_DecorrelationParameters {};
  // Vector-valued shading weights (Section 6.3).
  ReSTIRPTShadingParameters            m_ShadingParameters {};
  // Light-tile NEE candidates (Section 6.1).
  ReSTIRPTNeeParameters                m_NeeParameters {};
};

}  // namespace rtpt
