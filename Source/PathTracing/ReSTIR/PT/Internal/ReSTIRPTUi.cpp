#include "PathTracing/ReSTIR/PT/ReSTIRPTUi.h"

#include <imgui.h>

#include "PathTracing/Common/RendererUi.h"
#include "ReSTIR/PTParameters.h"

namespace rtpt
{

// ReSTIR PT Enhanced controls
// The panel is organized by paper section so each Enhanced technique can be toggled independently and its effect observed in isolation, mirroring the ablation table in the paper.
// Section numbers in the labels are deliberate: they tie a slider directly to the text that justifies it.

bool DrawReSTIRPTCommonControls(ReSTIRPTCommonSettings& settings)
{
  bool changed = false;

  changed |= DrawResolveModeControl(settings.resolveMode);

  int resamplingMode = static_cast<int>(settings.resamplingMode);
  const char* resamplingModes[] = { "None", "Temporal", "Spatial", "Temporal + Spatial" };

  if(ImGui::Combo("Resampling##ReSTIRPT_Mode", &resamplingMode, resamplingModes, IM_ARRAYSIZE(resamplingModes)))
  {
    settings.resamplingMode = static_cast<rtpt::ReSTIRPTResamplingMode>(resamplingMode);
    changed                 = true;
  }

  if(settings.resamplingMode == rtpt::ReSTIRPTResamplingMode::eNone)
  {
    ImGui::TextDisabled("Reuse disabled: this is plain 1spp path tracing through the ReSTIR plumbing, and must converge to the same image as Path Tracing mode.");
  }

  return changed;
}

bool DrawReSTIRPTInitialSamplingSection(ReSTIRPTInitialSamplingParameters& settings, uint32_t bounceLimit)
{
  if(!ImGui::TreeNodeEx("Initial Sampling", ImGuiTreeNodeFlags_DefaultOpen))
  {
    return false;
  }

  bool changed = false;

  changed |= DrawBounceLimitControl("Max Bounces", settings.maxBounces, bounceLimit);

  bool environmentImportanceSampling = (settings.environmentMapImportanceSampling != 0);

  if(ImGui::Checkbox("Environment Importance Sampling", &environmentImportanceSampling))
  {
    settings.environmentMapImportanceSampling = environmentImportanceSampling ? 1u : 0u;
    changed                                   = true;
  }

  // Russian roulette
  // Section 6.2.4. The single largest performance win in the paper's table (initial sampling 10.6 -> 5.2 ms), because initial sampling is dominated by long divergent paths.

  bool enableRussianRoulette = (settings.enableRussianRoulette != 0);

  if(ImGui::Checkbox("Russian Roulette (6.2.4)", &enableRussianRoulette))
  {
    settings.enableRussianRoulette = enableRussianRoulette ? 1u : 0u;
    changed                        = true;
  }

  ImGui::BeginDisabled(!enableRussianRoulette);

  int russianRouletteStartBounce = static_cast<int>(settings.russianRouletteStartBounce);

  if(ImGui::SliderInt("Roulette Start Bounce", &russianRouletteStartBounce, 1, 8))
  {
    settings.russianRouletteStartBounce = static_cast<uint32_t>(russianRouletteStartBounce);
    changed                             = true;
  }

  ImGui::EndDisabled();
  ImGui::TextDisabled("Roulette runs at initial sampling only. Applying it during replay would kill paths the base path survived.");

  ImGui::TreePop();

  return changed;
}

bool DrawReSTIRPTShiftSection(ReSTIRPTShiftParameters& settings)
{
  if(!ImGui::TreeNodeEx("Shift Mapping (4)", ImGuiTreeNodeFlags_DefaultOpen))
  {
    return false;
  }

  bool changed = false;

  // Replay fallback
  // The Reconnection / Random Replay / Hybrid selector that used to live here was never wired to anything: no shader reads shiftMapping, so all three settings produced the hybrid shift.
  // The control below is the choice that actually exists - whether a path with no reconnection anchor is replayed to its end or refused.

  bool enableReplayFallback = (settings.replayEndpointMask != 0u);

  if(ImGui::Checkbox("Random Replay Fallback", &enableReplayFallback))
  {
    settings.replayEndpointMask = enableReplayFallback ? uint32_t(RESTIR_PT_REPLAY_MASK_ALL) : 0u;
    changed                     = true;
  }

  ImGui::TextDisabled("Paths with no reconnection anchor are replayed to their endpoint rather than refused. Turning it off is not the " "conservative choice it looks like: with temporal and spatial reuse chained, the refusals compound into visible " "energy gain.");

  // Reconnection criteria
  // Only the threshold that belongs to the selected criteria is editable, and its explanation is shown beneath it.

  int reconnectionCriteria = static_cast<int>(settings.reconnectionCriteria);
  const char* reconnectionCriteriaNames[] = { "Legacy (distance + roughness)", "Footprint (Section 4)" };

  if(ImGui::Combo("Reconnection Criteria", &reconnectionCriteria, reconnectionCriteriaNames, IM_ARRAYSIZE(reconnectionCriteriaNames)))
  {
    settings.reconnectionCriteria = static_cast<uint32_t>(reconnectionCriteria);
    changed                       = true;
  }

  const bool usingFootprint = (settings.reconnectionCriteria == RESTIR_PT_RECONNECTION_CRITERIA_FOOTPRINT);

  ImGui::BeginDisabled(!usingFootprint);

  // Logarithmic because the useful range spans more than an order of magnitude and the interesting region sits at the bottom of it.
  changed |= ImGui::SliderFloat("Footprint Threshold (kappa)", &settings.footprintThreshold, 0.001f, 0.64f, "%.4f", ImGuiSliderFlags_Logarithmic);

  ImGui::EndDisabled();

  if(usingFootprint)
  {
    ImGui::TextDisabled("Scene-independent. Per-scene optima span 0.005-0.04; 0.02 is the recommended constant.");
  }

  ImGui::BeginDisabled(usingFootprint);
  changed |= ImGui::SliderFloat("Legacy Min Distance", &settings.legacyMinDistance, 0.0f, 1.0f, "%.3f");
  ImGui::EndDisabled();

  if(!usingFootprint)
  {
    ImGui::TextDisabled("Scene-scale dependent. This is the per-scene tuning burden the footprint threshold removes.");
  }

  // Retained under both criteria (Section 4.2).
  changed |= ImGui::SliderFloat("Min Roughness", &settings.minRoughness, 0.0f, 1.0f, "%.3f");

  ImGui::TreePop();

  return changed;
}

bool DrawReSTIRPTTemporalControls(ReSTIRPTTemporalResamplingParameters& settings)
{
  bool changed = false;

  int maxHistoryLength = static_cast<int>(settings.maxHistoryLength);

  if(ImGui::SliderInt("M Cap", &maxHistoryLength, 1, 64))
  {
    settings.maxHistoryLength = static_cast<uint32_t>(maxHistoryLength);
    changed                   = true;
  }

  changed |= ImGui::SliderFloat("Temporal Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  changed |= ImGui::SliderFloat("Temporal Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");

  bool enableDualMotionVectors = (settings.enableDualMotionVectors != 0);

  if(ImGui::Checkbox("Dual Motion Vectors (6.4)", &enableDualMotionVectors))
  {
    settings.enableDualMotionVectors = enableDualMotionVectors ? 1u : 0u;
    changed                          = true;
  }

  ImGui::TextDisabled("Recovers temporal history across disocclusions by reprojecting with the occluder's motion.");

  return changed;
}

bool DrawReSTIRPTSpatialControls(ReSTIRPTSpatialResamplingParameters& settings, float maxRadius)
{
  bool changed = false;

  int sampleCount = static_cast<int>(settings.numSamples);

  // Capped at the pairing texture count because each neighbour slot has its own pairing texture.
  if(ImGui::SliderInt("Spatial Sample Count", &sampleCount, 1, RESTIR_PT_MAX_PAIRING_TEXTURES))
  {
    settings.numSamples = static_cast<uint32_t>(sampleCount);
    changed             = true;
  }

  // Sigma is derived, so the settings object must be re-derived here as well as in the parameter context, or the value reported below keeps showing the sigma of whatever radius was set at construction.
  if(ImGui::SliderFloat("Spatial Radius", &settings.samplingRadius, 1.0f, maxRadius, "%.1f"))
  {
    settings.pairingSigma = rtpt::CalculateReSTIRPTPairingSigma(settings.samplingRadius);
    changed               = true;
  }

  changed |= ImGui::SliderFloat("Spatial Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  changed |= ImGui::SliderFloat("Spatial Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");

  bool enableMaterialSimilarityTest = (settings.enableMaterialSimilarityTest != 0);

  if(ImGui::Checkbox("Material Similarity Test", &enableMaterialSimilarityTest))
  {
    settings.enableMaterialSimilarityTest = enableMaterialSimilarityTest ? 1u : 0u;
    changed                               = true;
  }

  bool enablePairedSpatialReuse = (settings.enablePairedSpatialReuse != 0);

  if(ImGui::Checkbox("Paired Spatial Reuse (3)", &enablePairedSpatialReuse))
  {
    settings.enablePairedSpatialReuse = enablePairedSpatialReuse ? 1u : 0u;
    changed                           = true;
  }

  // Sigma is reported rather than edited, which makes the paired/unpaired comparison legible: both schemes are matched on mean sample distance, not on radius.
  ImGui::TextDisabled("Pairing sigma: %.1f px (derived from radius; matches mean sample distance)", settings.pairingSigma);

  return changed;
}

bool DrawReSTIRPTResamplingSection(ReSTIRPTTemporalResamplingParameters& temporalSettings, ReSTIRPTSpatialResamplingParameters& spatialSettings, float maxRadius)
{
  if(!ImGui::TreeNodeEx("Resampling##ReSTIRPT_Settings", ImGuiTreeNodeFlags_DefaultOpen))
  {
    return false;
  }

  bool changed = false;

  changed |= DrawReSTIRPTTemporalControls(temporalSettings);
  changed |= DrawReSTIRPTSpatialControls(spatialSettings, maxRadius);

  ImGui::TreePop();

  return changed;
}

bool DrawReSTIRPTDecorrelationSection(ReSTIRPTDecorrelationParameters& settings)
{
  if(!ImGui::TreeNodeEx("Decorrelation (5)"))
  {
    return false;
  }

  bool changed = false;

  bool enable = (settings.enable != 0);

  if(ImGui::Checkbox("Duplication Maps", &enable))
  {
    settings.enable = enable ? 1u : 0u;
    changed         = true;
  }

  ImGui::BeginDisabled(!enable);
  changed |= ImGui::SliderFloat("Cap Min", &settings.capMin, 1.0f, 20.0f, "%.1f");
  changed |= ImGui::SliderFloat("Gamma", &settings.gamma, 0.01f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
  ImGui::EndDisabled();

  ImGui::TextDisabled("Counts reservoirs in a 17x17 neighborhood sharing a random seed, then lowers the M cap where duplication is high. Trades a small local bias for far fewer correlation blobs.");

  ImGui::TreePop();

  return changed;
}

bool DrawReSTIRPTShadingSection(ReSTIRPTShadingParameters& settings)
{
  if(!ImGui::TreeNodeEx("Shading (6.3)"))
  {
    return false;
  }

  bool changed = false;

  bool enableVectorWeights = (settings.enableVectorWeights != 0);

  if(ImGui::Checkbox("Vector Resampling Weights", &enableVectorWeights))
  {
    settings.enableVectorWeights = enableVectorWeights ? 1u : 0u;
    changed                      = true;
  }

  ImGui::TextDisabled("Resampling selects on luminance, which leaves chroma noisy. Shading with vector-valued weights fixes that at no extra cost.");

  ImGui::TreePop();

  return changed;
}

bool DrawReSTIRPTNeeSection(ReSTIRPTNeeParameters& settings)
{
  if(!ImGui::TreeNodeEx("Next Event Estimation (6.1)"))
  {
    return false;
  }

  bool changed = false;

  bool enableLightTiles = (settings.enableLightTiles != 0);

  if(ImGui::Checkbox("RIS from Light Tiles", &enableLightTiles))
  {
    settings.enableLightTiles = enableLightTiles ? 1u : 0u;
    changed                   = true;
  }

  ImGui::TextDisabled("Resamples several presampled lights against the receiving surface instead of taking the first the power CDF " "returns. Helps in proportion to how badly power alone predicts which lights reach a surface; costs a few percent " "where it cannot help.");

  // The candidate count only has an effect while light tiles are on, so the slider is hidden otherwise.
  if(enableLightTiles)
  {
    int candidates = int(settings.primaryCandidates);

    // Capped at the light tile size: candidates are drawn consecutively from one tile, so past it the window wraps around and repeats lights already drawn.
    if(ImGui::SliderInt("NEE Candidates", &candidates, 1, int(RESTIR_PT_LIGHT_TILE_SIZE)))
    {
      settings.primaryCandidates = uint32_t(candidates);
      changed                    = true;
    }

    ImGui::TextDisabled("Count at the primary hit; deeper bounces decay by an inverse square in the bounce index.");
  }

  ImGui::TreePop();

  return changed;
}

}  // namespace rtpt
