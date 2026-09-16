#include "PathTracing/ReSTIR/PT/ReSTIRPTUi.h"

#include <imgui.h>

#include "Framework/Presentation/UiControls.h"
#include "PathTracing/Common/RendererUi.h"
#include "ReSTIR/PTParameters.h"

namespace rtpt
{

// ReSTIR PT Enhanced controls
// The panel is organized by paper section so each Enhanced technique can be toggled independently and its effect observed in isolation, mirroring the ablation table in the paper.
// Section numbers in the labels are deliberate: they tie a slider directly to the text that justifies it.
// The reasoning behind each control is a hover explanation rather than text under it, so the panel stays short enough to see every section at once.

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

  DrawTooltip("Where a pixel is allowed to look for paths other than its own.\n\nTemporal reuses the path this pixel held last frame, which is what makes a still image converge.\n\nSpatial reuses paths from neighbouring pixels in this frame, which is what removes noise while the camera moves.\n\nTogether they are the whole point of ReSTIR; None leaves plain one-sample-per-pixel path tracing.");

  if(settings.resamplingMode == rtpt::ReSTIRPTResamplingMode::eNone)
  {
    ImGui::TextDisabled("Reuse disabled: this is plain 1spp path tracing through the ReSTIR plumbing, and must converge to the same image as Path Tracing mode.");
  }

  return changed;
}

bool DrawReSTIRPTInitialSamplingSection(ReSTIRPTInitialSamplingParameters& settings, uint32_t bounceLimit)
{
  const bool open = ImGui::TreeNodeEx("Initial Sampling", ImGuiTreeNodeFlags_DefaultOpen);

  DrawTooltip("The path each pixel traces for itself before any reuse happens. Everything the reuse passes redistribute comes from here, so this is both the quality floor and the single most expensive pass.");

  if(!open)
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

  DrawTooltip("Aims environment rays at the bright parts of the HDRI instead of spreading them uniformly over the sphere. The difference is largest with a small bright sun in an otherwise dim sky, and negligible under a uniform environment.");

  // Russian roulette
  // Section 6.2.4. The single largest performance win in the paper's table (initial sampling 10.6 -> 5.2 ms), because initial sampling is dominated by long divergent paths.

  bool enableRussianRoulette = (settings.enableRussianRoulette != 0);

  if(ImGui::Checkbox("Russian Roulette (6.2.4)", &enableRussianRoulette))
  {
    settings.enableRussianRoulette = enableRussianRoulette ? 1u : 0u;
    changed                        = true;
  }

  DrawTooltip("Kills paths at random once they carry little energy, and scales the survivors up to keep the estimate unbiased. It is the largest single performance win in the paper - initial sampling 10.6 to 5.2 ms - because that pass is dominated by long paths that contribute almost nothing.\n\nRoulette runs during initial sampling only: applying it while replaying a path would kill vertices the original path survived.");

  ImGui::BeginDisabled(!enableRussianRoulette);

  int russianRouletteStartBounce = static_cast<int>(settings.russianRouletteStartBounce);

  if(ImGui::SliderInt("Roulette Start Bounce", &russianRouletteStartBounce, 1, 8))
  {
    settings.russianRouletteStartBounce = static_cast<uint32_t>(russianRouletteStartBounce);
    changed                             = true;
  }

  ImGui::EndDisabled();

  DrawTooltip("First bounce roulette may terminate. Bounces before it always survive, which keeps the direct and first-bounce contributions - where most of the energy is - free of the extra variance roulette introduces.");

  ImGui::TreePop();

  return changed;
}

bool DrawReSTIRPTShiftSection(ReSTIRPTShiftParameters& settings)
{
  const bool open = ImGui::TreeNodeEx("Shift Mapping (4)", ImGuiTreeNodeFlags_DefaultOpen);

  DrawTooltip("How a path belonging to one pixel is rewritten to start at another. Reuse is only possible because of this mapping, and how well it succeeds is what decides whether reuse removes noise or adds it.");

  if(!open)
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

  DrawTooltip("What to do with a path that has no vertex rough enough to reconnect through - a path down a chain of mirrors or glass, for instance. On, it is retraced from the new pixel all the way to its endpoint; off, it is refused.\n\nRefusing is not the conservative choice it looks like. With temporal and spatial reuse chained together the refusals compound, and the result gains energy visibly.");

  // Reconnection criteria
  // Only the threshold that belongs to the selected criteria is editable, and its explanation is shown beneath it.

  int reconnectionCriteria = static_cast<int>(settings.reconnectionCriteria);
  const char* reconnectionCriteriaNames[] = { "Legacy (distance + roughness)", "Footprint (Section 4)" };

  if(ImGui::Combo("Reconnection Criteria", &reconnectionCriteria, reconnectionCriteriaNames, IM_ARRAYSIZE(reconnectionCriteriaNames)))
  {
    settings.reconnectionCriteria = static_cast<uint32_t>(reconnectionCriteria);
    changed                       = true;
  }

  DrawTooltip("The test that decides where along a path it is safe to reconnect.\n\nLegacy asks whether the vertex is far enough away and rough enough, using thresholds that have to be retuned for every scene scale.\n\nFootprint instead compares how much the path has spread against the size of the reconnection, which is scale-free - the contribution of Section 4 and the reason one constant works everywhere.");

  const bool usingFootprint = (settings.reconnectionCriteria == RESTIR_PT_RECONNECTION_CRITERIA_FOOTPRINT);

  ImGui::BeginDisabled(!usingFootprint);

  // Logarithmic because the useful range spans more than an order of magnitude and the interesting region sits at the bottom of it.
  changed |= ImGui::SliderFloat("Footprint Threshold (kappa)", &settings.footprintThreshold, 0.001f, 0.64f, "%.4f", ImGuiSliderFlags_Logarithmic);

  ImGui::EndDisabled();

  DrawTooltip("How much path spread a reconnection may cost. Lower reconnects more cautiously, keeping more of the path replayed and more expensive; higher reconnects sooner and risks reusing paths that do not match.\n\nScene-independent: per-scene optima span 0.005 to 0.04, and 0.02 is the recommended constant.");

  ImGui::BeginDisabled(usingFootprint);
  changed |= ImGui::SliderFloat("Legacy Min Distance", &settings.legacyMinDistance, 0.0f, 1.0f, "%.3f");
  ImGui::EndDisabled();

  DrawTooltip("Shortest segment the legacy criteria will reconnect across, in world units. Scene-scale dependent, and this is exactly the per-scene tuning burden the footprint threshold removes.");

  // Retained under both criteria (Section 4.2).
  changed |= ImGui::SliderFloat("Min Roughness", &settings.minRoughness, 0.0f, 1.0f, "%.3f");

  DrawTooltip("Roughness a vertex must reach before it may be reconnected through, under either criteria. A near-specular vertex reflects a narrow set of directions, so a reconnection made there lands outside the lobe and contributes nothing but variance.");

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

  DrawTooltip("How many past frames one reservoir may claim to represent. It is the temporal counterpart of a history length: higher converges further on a still image, and holds on to stale lighting longer once something moves.");

  changed |= ImGui::SliderFloat("Temporal Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  DrawTooltip("How far last frame's depth may disagree with this one before the reprojected reservoir is rejected. It is what stops a background pixel from inheriting a foreground pixel's path across a silhouette.");

  changed |= ImGui::SliderFloat("Temporal Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");
  DrawTooltip("The same test on the surface normal. Two surfaces at the same depth but facing differently receive different light, so reusing between them leaks lighting around corners.");

  bool enableDualMotionVectors = (settings.enableDualMotionVectors != 0);

  if(ImGui::Checkbox("Dual Motion Vectors (6.4)", &enableDualMotionVectors))
  {
    settings.enableDualMotionVectors = enableDualMotionVectors ? 1u : 0u;
    changed                          = true;
  }

  DrawTooltip("Reprojects with a second motion vector - the occluder's - alongside the surface's own. Where a moving object uncovers the background, the surface motion vector points at pixels that were hidden and has no history to offer; the occluder's finds the history that was there.");

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

  DrawTooltip("How many neighbouring pixels each pixel tries to reuse a path from. Every sample costs a shift attempt, so this is the main spatial quality-for-time dial.");

  // Sigma is derived, so the settings object must be re-derived here as well as in the parameter context, or the value reported below keeps showing the sigma of whatever radius was set at construction.
  if(ImGui::SliderFloat("Spatial Radius", &settings.samplingRadius, 1.0f, maxRadius, "%.1f"))
  {
    settings.pairingSigma = rtpt::CalculateReSTIRPTPairingSigma(settings.samplingRadius);
    changed               = true;
  }

  DrawTooltip("How far away, in pixels, a neighbour may be drawn from. A wide radius finds genuinely different paths and so removes more noise, but its neighbours are less likely to share this pixel's surface, and the ones that fail the tests below are wasted work.");

  changed |= ImGui::SliderFloat("Spatial Depth Threshold", &settings.depthThreshold, 0.0f, 1.0f, "%.3f");
  DrawTooltip("How far a neighbour's depth may differ before its reservoir is refused. Same purpose as the temporal test, applied across the screen rather than across time.");

  changed |= ImGui::SliderFloat("Spatial Normal Threshold", &settings.normalThreshold, 0.0f, 1.0f, "%.2f");
  DrawTooltip("The same test on the surface normal, which is what keeps light from bleeding across a crease between two faces.");

  bool enableMaterialSimilarityTest = (settings.enableMaterialSimilarityTest != 0);

  if(ImGui::Checkbox("Material Similarity Test", &enableMaterialSimilarityTest))
  {
    settings.enableMaterialSimilarityTest = enableMaterialSimilarityTest ? 1u : 0u;
    changed                               = true;
  }

  DrawTooltip("Also refuses a neighbour whose material differs too much. Depth and normal agreeing is not enough: a rough and a polished surface side by side reflect completely different directions, and swapping paths between them adds noise instead of removing it.");

  bool enablePairedSpatialReuse = (settings.enablePairedSpatialReuse != 0);

  if(ImGui::Checkbox("Paired Spatial Reuse (3)", &enablePairedSpatialReuse))
  {
    settings.enablePairedSpatialReuse = enablePairedSpatialReuse ? 1u : 0u;
    changed                           = true;
  }

  DrawTooltip("Pairs pixels up so one shift attempt serves both ends of the pair, instead of each pixel shifting to its neighbours independently. It buys the noise reduction of a larger sample count at roughly half the shifts.");

  // Sigma is reported rather than edited, which makes the paired/unpaired comparison legible: both schemes are matched on mean sample distance, not on radius.
  ImGui::TextDisabled("Pairing sigma: %.1f px (derived from radius; matches mean sample distance)", settings.pairingSigma);

  DrawTooltip("Spread of the paired scheme's sample distribution, derived from the radius above rather than set here. It is chosen so paired and unpaired reuse draw samples the same mean distance away, which is what makes comparing them fair.");

  return changed;
}

bool DrawReSTIRPTResamplingSection(ReSTIRPTTemporalResamplingParameters& temporalSettings, ReSTIRPTSpatialResamplingParameters& spatialSettings, float maxRadius)
{
  const bool open = ImGui::TreeNodeEx("Resampling##ReSTIRPT_Settings", ImGuiTreeNodeFlags_DefaultOpen);

  DrawTooltip("The reuse passes themselves, and the tests that decide which reservoirs a pixel is allowed to accept. A test that is too strict throws away good samples; one that is too loose reuses paths from surfaces that are lit differently, which shows up as blotches and leaking light.");

  if(!open)
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
  const bool open = ImGui::TreeNodeEx("Decorrelation (5)");

  DrawTooltip("Counters the price of reuse: once a path has spread across many pixels, those pixels no longer have independent estimates, and their shared error appears as slowly drifting blobs rather than as noise.");

  if(!open)
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

  DrawTooltip("Counts how many reservoirs in a 17x17 neighbourhood descend from the same original path, then lowers the history cap where that count is high, forcing those pixels to start sampling for themselves again. It trades a small local bias for far fewer correlation blobs.");

  ImGui::BeginDisabled(!enable);

  changed |= ImGui::SliderFloat("Cap Min", &settings.capMin, 1.0f, 20.0f, "%.1f");
  DrawTooltip("Floor the history cap is lowered to in the most duplicated regions. Lower reacts more aggressively to correlation and keeps less history.");

  changed |= ImGui::SliderFloat("Gamma", &settings.gamma, 0.01f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
  DrawTooltip("How sharply the cap responds to the measured duplication. Small values leave the cap alone until duplication is severe; large values react to any of it.");

  ImGui::EndDisabled();

  ImGui::TreePop();

  return changed;
}

bool DrawReSTIRPTShadingSection(ReSTIRPTShadingParameters& settings)
{
  const bool open = ImGui::TreeNodeEx("Shading (6.3)");

  DrawTooltip("The final pass, which turns each surviving reservoir into the pixel's color.");

  if(!open)
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

  DrawTooltip("Weights the shaded result per color channel instead of by a single luminance. Resampling has to select on one number, so it selects on brightness and leaves color noisier than brightness; carrying the weight as a color fixes that, and costs nothing.");

  ImGui::TreePop();

  return changed;
}

bool DrawReSTIRPTNeeSection(ReSTIRPTNeeParameters& settings)
{
  const bool open = ImGui::TreeNodeEx("Next Event Estimation (6.1)");

  DrawTooltip("How each path vertex picks a light to connect to directly, rather than hoping a bounce finds one. This is what makes small bright lights usable at all.");

  if(!open)
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

  DrawTooltip("Draws several candidate lights and keeps the one that actually reaches this surface, instead of taking the first light the power distribution returns. Only the winner costs a shadow ray; the rest cost a light read and a BSDF evaluation.\n\nIt helps in proportion to how badly light power alone predicts which lights reach a surface, and costs a few percent where it cannot help.");

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

    DrawTooltip("Candidates drawn at the primary hit. Deeper bounces decay by an inverse square in the bounce index, because their contribution falls off while their cost does not. The cap is the size of one light tile, past which the window wraps and repeats lights already drawn.");
  }

  ImGui::TreePop();

  return changed;
}

}  // namespace rtpt
