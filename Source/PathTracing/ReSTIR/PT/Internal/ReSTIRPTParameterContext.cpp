#include "PathTracing/ReSTIR/PT/ReSTIRPTParameterContext.h"

#include <cassert>
#include <cmath>

namespace rtpt
{

namespace
{

// JenkinsHash
// Small integer hash, used to decorrelate the per-frame sampling stream.
// http://burtleburtle.net/bob/hash/integer.html

uint32_t JenkinsHash(uint32_t a)
{
  a = (a + 0x7ed55d16) + (a << 12);
  a = (a ^ 0xc761c23c) ^ (a >> 19);
  a = (a + 0x165667b1) + (a << 5);
  a = (a + 0xd3a2646c) ^ (a << 9);
  a = (a + 0xfd7046c5) + (a << 3);
  a = (a ^ 0xb55a4f09) ^ (a >> 16);

  return a;
}

void CheckStaticParameters(const ReSTIRPTStaticParameters& parameters)
{
  // Shader indexing assumes positive dimensions.
  assert(parameters.renderWidth > 0);
  assert(parameters.renderHeight > 0);
}

}  // namespace

ReSTIRPTReservoirBufferParameters CalculateReSTIRPTReservoirBufferParameters(uint32_t renderWidth, uint32_t renderHeight)
{
  // Block-linear layout
  // Reservoir storage is block-linear so nearby pixels stay close in memory during spatial reuse.
  // This matters more for PT than for DI: a PT reservoir is 64 bytes against DI's 24, and spatial reuse reads a random neighborhood of them.

  const uint32_t renderWidthBlocks  = (renderWidth + RESTIR_PT_RESERVOIR_BLOCK_SIZE - 1) / RESTIR_PT_RESERVOIR_BLOCK_SIZE;
  const uint32_t renderHeightBlocks = (renderHeight + RESTIR_PT_RESERVOIR_BLOCK_SIZE - 1) / RESTIR_PT_RESERVOIR_BLOCK_SIZE;

  ReSTIRPTReservoirBufferParameters parameters {};

  // Pitches are measured in reservoir entries, not bytes.
  parameters.reservoirBlockRowPitch = renderWidthBlocks * (RESTIR_PT_RESERVOIR_BLOCK_SIZE * RESTIR_PT_RESERVOIR_BLOCK_SIZE);
  parameters.reservoirArrayPitch    = parameters.reservoirBlockRowPitch * renderHeightBlocks;

  return parameters;
}

float CalculateReSTIRPTPairingSigma(float samplingRadius)
{
  // sqrt(8 / (9*pi)) ~= 0.531923. See the header for the mean-distance derivation.
  constexpr float kRadiusToSigma = 0.5319230405352436f;

  return samplingRadius * kRadiusToSigma;
}

ReSTIRPTBufferIndices GetDefaultReSTIRPTBufferIndices()
{
  // Every index starts at array 0; UpdateBufferIndices derives the real rotation as soon as a context exists.

  ReSTIRPTBufferIndices bufferIndices {};

  bufferIndices.initialSamplingOutputBufferIndex    = 0;
  bufferIndices.temporalResamplingInputBufferIndex  = 0;
  bufferIndices.temporalResamplingOutputBufferIndex = 0;
  bufferIndices.spatialResamplingInputBufferIndex   = 0;
  bufferIndices.spatialResamplingOutputBufferIndex  = 0;
  bufferIndices.shadingInputBufferIndex             = 0;

  return bufferIndices;
}

ReSTIRPTInitialSamplingParameters GetDefaultReSTIRPTInitialSamplingParams()
{
  // Paper configuration
  // The paper produces initial samples with 1spp path tracing (one path tree per pixel) and draws 32 NEE light candidates at the primary hit.
  // The bounce limit is the one departure: three keeps initial sampling interactive and matches the reference path tracer's default, so the two renderers are compared at the same path length.

  ReSTIRPTInitialSamplingParameters parameters {};

  parameters.maxBounces                       = 3;
  parameters.environmentMapImportanceSampling = 1;
  parameters.enableRussianRoulette            = 1;

  // Roulette from the third vertex onward keeps the direct and first-bounce contributions - which carry most of the energy - always intact.
  parameters.russianRouletteStartBounce = 2;

  return parameters;
}

ReSTIRPTShiftParameters GetDefaultReSTIRPTShiftParams()
{
  // Shift mapping
  // Hybrid shift plus the Section 4 footprint criteria is the paper's configuration.

  ReSTIRPTShiftParameters parameters {};

  parameters.shiftMapping         = RESTIR_PT_SHIFT_HYBRID;
  parameters.reconnectionCriteria = RESTIR_PT_RECONNECTION_CRITERIA_FOOTPRINT;

  // Footprint threshold
  // Equation 5's kappa: how far the shifted vertex may drift, as a fraction of the primary ray footprint, before reconnection is refused and replay takes over.
  // The paper reports per-scene optima spanning 0.005-0.04, averaging 0.0175, and takes 0.02 as the near-optimal constant. That was the only justification here for a long time - the value was inherited, never measured locally - so it was swept against the plain path-traced reference in linear radiance:
  //   kappa                0.005    0.01    0.02    0.04    0.08
  //   Cornell Box
  //     bias vs reference  0.391%  0.415%  0.424%  0.459%  0.520%
  //     relative RMSE      0.4757  0.4747  0.4764  0.4805  0.4821
  //     max |J-1|          2.2e02  2.2e02  2.2e02  1.6e03  1.6e03
  //   Bunny Metallic
  //     bias vs reference  0.120%  0.121%  0.121%  0.122%  0.122%
  //     relative RMSE      0.2645  0.2645  0.2648  0.2648  0.2648
  //     max |J-1|          1.2e01  1.2e01  1.2e01  1.9e01  1.9e01
  // Bunny Metallic cannot discriminate and is here to show that: 99.8% of its paths never find a reconnection anchor at all, so the criterion this constant tunes almost never runs, and every column is the same number. A sweep on that scene alone would have "confirmed" any value at all.
  // Cornell Box does discriminate, and says two things. Cost rises monotonically with kappa - bias 0.391% to 0.520% across the range - because a looser threshold admits reconnections whose Jacobians are worse. And there is a cliff rather than a slope: max |J-1| holds at 2.2e02 through 0.02 and jumps 7x to 1.6e03 at 0.04. That cliff, not the average of the paper's optima, is the real reason to stay at or below 0.02.
  // 0.01 measured marginally better than 0.02 on the only scene that can tell them apart (0.415% vs 0.424% bias, 0.4747 vs 0.4764 relative RMSE). That margin is far too small to justify diverging from the paper on one scene, so the value stays - but it now stays on measured ground, on the safe side of a measured cliff, rather than on an average borrowed from six scenes that are not these.

  parameters.footprintThreshold = 0.02f;

  // Minimum roughness
  // Section 4.2 safeguard at the vertex preceding the reconnection vertex. The paper only says "we use ReSTIR PT's default alpha = 0.2" and never states the units, which matters because a perceptual 0.2 is a GGX alpha of 0.04.
  // Resolved against ReSTIR PT's own source: its specularRoughnessThreshold defaults to 0.2 and is compared against Falcor's ShadingData::linearRoughness, the perceptual artist-facing value that Falcor squares to get GGX alpha. SurfaceData::roughness here is likewise perceptual glTF roughness, so 0.2 transfers directly with no conversion.
  // Measurement could not settle the UNITS: on the glossy scenes the two readings are byte-identical (those materials are bimodal, so nothing sits between the thresholds). What measurement does settle is whether the guard earns its place and how precisely the value needs choosing.
  // Re-derived with random replay enabled and in linear radiance. The earlier note here cited shift success of 49.4% and a tonemapped RMSE; success is now 55.9% and that metric was later shown to understate errors by 6x to 135x, so those numbers described a renderer that no longer exists.
  //   Bunny Metallic      0.045    0.1     0.2    0.447    0.7
  //     bias vs reference 0.114%  0.121%  0.121%  0.121%  0.121%
  //     relative RMSE     0.3151  0.2648  0.2648  0.2648  0.2648
  //     max |J-1|         4.9e01  1.2e01  1.2e01  1.2e01  1.2e01
  //     shift success     56.21%  55.89%  55.89%  55.89%  55.89%
  //   Cornell Box: flat across the whole range on every column - bias within 0.432-0.440%, relative RMSE within 0.4514-0.4564, max |J-1| identical.
  // Two conclusions. The guard is worth having: disabling it (0.045) costs 16% more noise and roughly quadruples the worst Jacobian on the glossy scene, which is the same direction the original note claimed even though its figures no longer hold.
  // And the exact value does not matter - every setting from 0.1 upward is identical, because the threshold sorts materials into two groups rather than interpolating. 0.2 is kept because it is the paper's, not because it measured better than 0.1 or 0.7.

  parameters.minRoughness = 0.2f;

  // Replay endpoint mask
  // Every replayable kind is on, because refusing a shift is not the safe default it looks like. A refusal still lets the other technique's coverage down-weight the canonical sample while contributing nothing itself, and once temporal and spatial reuse are chained those refusals compound into enormous energy gain.
  // Mean LINEAR radiance against the plain path-traced reference (--debugview 1), 800 accumulated frames, decorrelation off. Linear, not the tonemapped image: tonemapping clips exactly the bright regions the error lives in, which understated these by 6x on Cornell Box and by 135x on Area Light.
  //                          Cornell  Disoccl    Area           Bunny  Scattered
  //                              Box  Pillars   Light  Firepl  Metallic   Lights
  //   no reuse                -0.01%   -0.02%  +0.00%  -0.00%    +0.00%   -0.44%
  //   t+s, no replay         +86.40%  +74.96% +233.4%  +0.98%    +0.01%   +5.23%
  //     + bsdf emissive only +84.51%  +73.69% +222.8%  +0.98%    +0.01%   -0.42%
  //     + environment NEE    +60.27%  +49.95%  +59.6%  +2.74%  +109.39%   +5.22%
  //     + environment miss   +29.69%  +28.70% +212.0% +45.39%   +35.48%   +5.21%
  //     + all kinds           +0.44%   +0.35%  +0.03%  +0.77%    +0.12%   -0.43%
  // Three things in that table matter beyond the totals. Either pass ALONE is unbiased with or without replay - only the chain is not - so comparing two reuse configurations against each other cannot find this and only the reference can.
  // The partial masks are catastrophic, not merely unhelpful: enabling one kind changes which candidates survive without restoring the balance, and Bunny Metallic goes from +0.01% to +109% on environment NEE alone. They exist to attribute a regression to one branch, never to ship.
  // And the -0.44% on Scattered Lights is the firefly noise floor of the metric at this frame count, not a measured deficit - it appears in the no-reuse row too.
  // The residual with all kinds on is the refusals replay still cannot cover. Measured on the spatial pass, as a share of all pixels (Cornell Box / Disocclusion Pillars):
  //   replay escaped            0.54% / 0.59%   replayed path left the scene where
  //                                             the base path found geometry
  //   occluded / wrong geometry 0.68% / 0.62%   reconnection ray hit something else
  //   prev lobe unsupported     0.16% / 0.07%
  //   bad denominator           0.12% / 0.09%
  //   replay sample failed      0.02% / 0.04%
  //                             -----   -----
  //                             1.52% / 1.41%   against a +0.44% / +0.35% residual
  // Counts only, deliberately. Image error is not additively attributable to a refusal class: a refusal changes which candidate is selected and therefore how every later frame evolves, so "this class costs X%" cannot be read off these numbers, and isolating one by ablation perturbs the others.
  // The two escapee classes dominate and are geometric - they are what a shift with a wider valid domain would recover - which is the direction to look, not this constant.
  // On the most geometrically complex scene measured, replay is not an improvement to reuse but the entire reason reuse works at all. Sponza Reduced at 0.45 scale, stepping the ablation one technique at a time:
  //   1spp path traced      relative RMSE 0.2077
  //   + temporal reuse                    0.2077
  //   + spatial reuse                     0.2077   (and 19.6 ms, from 1.2)
  //   + paired spatial                    0.2077
  //   + random replay                     0.1243
  // The first four rows are the same image to four decimals: reconnection-only reuse finds no admissible anchor anywhere in that scene, so every candidate is refused and the passes cost 16x the frame time to contribute nothing.
  // Replay is what converts that into a 40% noise reduction. The small closed-box scenes cannot show this - they have anchors everywhere - which is why a scene of this kind belongs in the ablation.

  parameters.replayEndpointMask = RESTIR_PT_REPLAY_MASK_ALL;

  // Legacy criteria only. Scene-scale dependent, which is precisely the tuning burden the footprint threshold removes.
  parameters.legacyMinDistance = 0.03f;

  return parameters;
}

ReSTIRPTTemporalResamplingParameters GetDefaultReSTIRPTTemporalResamplingParams()
{
  ReSTIRPTTemporalResamplingParameters parameters {};

  // Confidence cap
  // "Cap = 20" is ReSTIR PT's default confidence cap, retained by the paper - and this implementation is only marginally stable there. That is worth stating plainly, because the cap turns out to be the contraction knob for the whole temporal <-> paired-spatial loop.
  // Paired reuse plus temporal reuse amplifies an arbitrarily small perturbation. Perturbing with the sorted pre-pass (which computes the same shifts to within a last bit - see ReSTIRPTSettings.h) and measuring growth per frame on Bunny Metallic:
  //   cap  2   stable            cap 20   1.1227x per frame -> 1e33 by 800 frames
  //   cap  5   stable            cap 40   1.1571x per frame
  //   cap 10   diverges
  // Verified at 1200 and 2400 accumulated frames, not at 300: cap 10 reads a flat 0.8381 at 300 frames and 4.4e21 at 1200. A short window says nothing here, so a short frame count says nothing about stability here.
  // What a lower cap costs, unperturbed, at 800 frames (bias / relative RMSE):
  //                     cap 5             cap 10            cap 20
  //   Cornell Box     0.166% / 0.4863   0.272% / 0.4664   0.424% / 0.4764
  //   Disoccl.        0.115% / 0.5090   0.200% / 0.4818   0.334% / 0.4880
  //   Bunny Metallic  0.095% / 0.2188   0.116% / 0.2408   0.121% / 0.2648
  //   Scattered      -0.443% / 0.6599  -0.437% / 0.6482  -0.436% / 0.6684
  // So a shorter history is consistently LESS biased and, on two of four scenes, also less noisy - which fits the correlation story: paired reuse makes neighbours share shifts, and a long history compounds that correlation instead of averaging it away.
  // The paper's 20 is kept anyway, for two reasons. The default configuration is stable at 20 (2400 frames, no divergence) because nothing currently perturbs it, and lowering a constant the paper specifies would bury an unexplained result rather than fix it: a correct estimator should not have a feedback gain above one at the author's own setting.
  // Section 5 decorrelation is the paper's damping for exactly this correlation, and at its gamma of 0.1 it does hold the loop stable - but at a bias cost (about 5.7% on Cornell Box) that was once judged far outside tolerance here, which is why GetDefaultReSTIRPTDecorrelationParams used 0.5 for a long time. 0.5 only slowed the growth rather than stopping it.
  // The default is now the paper's 0.1 again (see "Section 5 alpha" in GetDefaultReSTIRPTDecorrelationParams), which accepts that bias, as the paper intends, in exchange for a stable loop.
  // Anyone changing this should re-measure the boundary rather than assume it: the failure is silent until several hundred accumulated frames have passed.

  parameters.maxHistoryLength = 20;
  parameters.depthThreshold   = 0.1f;
  parameters.normalThreshold  = 0.5f;

  // Dual motion vectors
  // Section 6.4. A second reprojection candidate from the occluder's motion, tried only when the pixel's own motion vector lands on a surface that fails the similarity gate, and held to that same gate so a wrong guess is rejected.
  // Validating this needs a moving camera, which no other measurement here uses, and it needs the right KIND of motion. Under orbit on Bunny Metallic and Cornell Box the temporal success rate moves by at most 0.01 points at any speed - not because the mechanism is idle but because there is nothing to rescue: those scenes lose only 0.07-1.56% of pixels to reprojection failure in the first place, so the technique's entire addressable population is under 2%.
  // Lateral pan on Disocclusion Pillars is the case it exists for - foreground geometry sweeping across background geometry - and there it works (temporal pass outcomes on the last of 64 frames with the camera moving):
  //   pan/frame   surface mismatch      temporal SUCCESS
  //     0.002      0.10% -> 0.09%       56.96% -> 56.98%
  //     0.005      0.16% -> 0.06%       42.53% -> 42.63%
  //     0.010      0.17% -> 0.05%       24.68% -> 24.79%
  //     0.020      0.00% -> 0.00%        7.75% -> 7.75%
  // It removes about two thirds of the surface-mismatch rejections, which is exactly the failure class it targets, and converts them into reuse. The gain in total success is only ~0.1 points because that class is only ~0.17% of pixels.
  // At 0.02 per frame history has already collapsed to 7.75% and there is nothing left to recover - the technique rescues reprojection, not motion this violent.
  // Judged on image error it looks like noise: on the pixels it changes, median error moves under 1% and not consistently in one direction. That is expected at this population size and is not evidence either way; the outcome histogram above is where the claim is actually testable.

  parameters.enableDualMotionVectors = 1;

  return parameters;
}

ReSTIRPTSpatialResamplingParameters GetDefaultReSTIRPTSpatialResamplingParams()
{
  // Three random spatial neighbors in a 30-pixel radius, borrowed from ReSTIR PT.

  ReSTIRPTSpatialResamplingParameters parameters {};

  parameters.numSamples = 3;

  // Sampling radius
  // The paper uses 30. Measured here, 6 is better on every axis, and this has now been rechecked under the conditions the earlier note said to recheck it under.
  // The first sweep was taken while the shift still refused anchorless paths, and it concluded "revisit once the shift covers the endpoint cases it rejects; the paper's 30 should become usable again then".
  // Full random replay now covers those cases - there are no such refusals left - so the sweep was rerun with it on, in linear radiance rather than through a tonemapped image. The answer did not change:
  //   Cornell Box            r=3     r=6     r=12    r=20    r=30
  //     bias vs reference  0.126%  0.432%  0.732%  0.904%  1.068%
  //     relative RMSE      0.7781  0.4764  0.5033  0.6575  0.7125
  //     mean Jacobian      1.0011  1.0041  1.0211  1.0180  1.6613
  //     max |J-1|          1.8e02  2.2e02  3.8e03  1.0e03  1.6e05
  //     shift success      57.58%  56.39%  55.05%  52.86%  50.35%
  //   Scattered Lights       r=3     r=6     r=12    r=20    r=30
  //     bias vs reference -0.418% -0.427% -0.345% -0.364% -0.352%   (noise floor)
  //     relative RMSE      1.1333  0.6684  0.7993  0.9790  1.1188
  //     mean Jacobian      1.0001  1.0006  1.0041  1.0039  1.0101
  //     shift success      96.32%  94.19%  91.92%  88.65%  84.71%
  // Two scenes chosen to separate the explanations. On Cornell Box a 30-pixel radius spans a large fraction of the room, and the Jacobian duly explodes.
  // On Scattered Lights it does not - mean J stays at 1.01 out to r=30 - and 30 is STILL the worst radius for noise. So conditioning is not the whole story: shift success falls monotonically with radius on both scenes, and a neighbour whose shift is refused contributes nothing while still diluting the estimate.
  // Note r=3 has the lowest bias on Cornell Box and the worst noise. 6 is the noise optimum on both scenes, which is what spatial reuse is for.
  // What would make 30 pay is a shift that keeps succeeding at distance - Section 4 sets the reconnection footprint bound, so that is where to look, not here.

  parameters.samplingRadius               = 6.0f;
  parameters.enablePairedSpatialReuse     = 1;
  parameters.pairingSigma                 = CalculateReSTIRPTPairingSigma(parameters.samplingRadius);
  parameters.depthThreshold               = 0.1f;
  parameters.normalThreshold              = 0.5f;
  parameters.enableMaterialSimilarityTest = 1;

  return parameters;
}

ReSTIRPTDecorrelationParameters GetDefaultReSTIRPTDecorrelationParams()
{
  ReSTIRPTDecorrelationParameters parameters {};

  parameters.enable = 1;
  parameters.capMin = 1.0f;

  // Section 5 alpha
  // Verified against the paper: "we found c_Cap^min = 1 and alpha = 0.1 yield good results". Both now match.
  // This was 0.5 for a long time, tuned to remove a bias that the paper puts there ON PURPOSE. Section 5 states it plainly - "the partition of unity of MIS weights m_i is violated, introducing a small bias... our approach trades correlation for bias" - and reports 3.25% average absolute relative bias in a hard scene. The measured cost here is the same order (about 5.7% on Cornell Box).
  // Raising alpha bought that bias back and gave away what it paid for. Section 5 exists to stop high-energy samples spreading through reuse, and with alpha = 0.5 it engages too late to do that: the compacted pre-pass then diverged on five of six camera poses on Bunny Metallic at 1200 frames.
  // At the paper's 0.1, zero of six. That was the whole "instability" - a mistuned constant, not a defect in the estimator.

  parameters.gamma = 0.1f;

  return parameters;
}

ReSTIRPTShadingParameters GetDefaultReSTIRPTShadingParams()
{
  // Vector resampling weights
  // Section 6.3. Shading with the summed RGB resampling weight instead of the one selected candidate's radiance. It costs nothing extra: every F it needs was already evaluated to build the scalar weights, and the producing pass writes the sum whether or not this is on.
  // What it buys, at 16 frames against a converged reference:
  //                        Cornell Box        Scattered Lights
  //     luminance RMSE   0.4764 -> 0.4764    0.6684 -> 0.6684
  //     chroma RMSE      0.0872 -> 0.0656    0.0795 -> 0.0579
  //     bias              0.424%, unchanged   -0.429%, unchanged
  // Chroma noise falls about a quarter on both scenes while luminance noise does not move at all, which is exactly the claim: resampling picks samples by a scalar target function, so luminance is already well importance-sampled and the error that remains is in the chroma.
  // Bias is unchanged because this is a variance reduction - it marginalizes over the index choice rather than changing what is being estimated.
  // The luminance column is not evidence of a no-op, and was read as one for a while: luminance is 71.5% green, green improves least (0.9% against 7.6% in red), so the projection cancels almost the whole benefit. Judging this feature needs a colour metric - the ablation table cannot see it.

  ReSTIRPTShadingParameters parameters {};

  parameters.enableVectorWeights = 1;

  return parameters;
}

ReSTIRPTNeeParameters GetDefaultReSTIRPTNeeParams()
{
  ReSTIRPTNeeParameters parameters {};

  // Light tiles
  // On by default, but the benefit depends entirely on whether power alone predicts which lights reach a surface, and it is NOT free where it does not.
  // Linear-radiance relative RMSE against a 2400-frame reference, temporal+spatial reuse. Measured as a curve rather than repeated runs because renders here are deterministic - the RNG is seeded from the frame index, so re-running a configuration reproduces the same image and averages nothing:
  //             Scattered Lights (132 varied)    Cornell Many Lights (24 similar)
  //   frames    single   RIS 32   change          single   RIS 32   change
  //      4      2.1065   1.7431   -17.3%          2.1888   2.2594    +3.2%
  //      8      1.2302   1.0197   -17.1%          1.2413   1.2969    +4.5%
  //     16      0.7126   0.5822   -18.3%          0.7671   0.7183    -6.4%
  //     32      0.4536   0.3497   -22.9%          0.4804   0.4188   -12.8%
  //     64      0.2929   0.2350   -19.8%          0.3339   0.2528   -24.3%
  //   initial sampling cost       +4.2%                             +22.4%
  // Scattered Lights is the case this exists for: 132 emitters spread over a corridor with a ~60x power range, where the brightest lights a power CDF prefers are usually far away or occluded. The gain is 17-23% at every sample count.
  // Cornell Many Lights is the honest counterexample. Its 24 ceiling panels have similar power and all reach the room, so the CDF is already close to the best proposal available - and at 4 and 8 frames RIS is slightly WORSE, because the resampling weight adds variance that the better light choice has not yet repaid.
  // It only turns profitable past ~16 frames. If this renderer is judged at 4spp on scenes like that, this default is the wrong one.
  // The cost is in initial sampling's candidate loop, NOT in building the tiles: the tile pass measures 0.01 ms, about 0.02% of a frame. Per-feature wall-clock differencing previously attributed the whole cost to "light tiles", which pointed at the wrong optimization.

  parameters.enableLightTiles = 1;

  // The paper's count at the primary hit; deeper bounces decay by an inverse square, see PTNeeCandidateCount. Not tuned below this: an earlier reading that 8 candidates matched 32 held on one scene and failed on the other, so there is no measured basis for lowering it.
  parameters.primaryCandidates = 32;

  return parameters;
}

ReSTIRPTParameterContext::ReSTIRPTParameterContext(const ReSTIRPTStaticParameters& parameters)
    : m_StaticParameters(parameters)
    , m_ReservoirBufferParameters(CalculateReSTIRPTReservoirBufferParameters(parameters.renderWidth, parameters.renderHeight))
    , m_BufferIndices(GetDefaultReSTIRPTBufferIndices())
    , m_InitialSamplingParameters(GetDefaultReSTIRPTInitialSamplingParams())
    , m_ShiftParameters(GetDefaultReSTIRPTShiftParams())
    , m_TemporalResamplingParameters(GetDefaultReSTIRPTTemporalResamplingParams())
    , m_SpatialResamplingParameters(GetDefaultReSTIRPTSpatialResamplingParams())
    , m_DecorrelationParameters(GetDefaultReSTIRPTDecorrelationParams())
    , m_ShadingParameters(GetDefaultReSTIRPTShadingParams())
    , m_NeeParameters(GetDefaultReSTIRPTNeeParams())
{
  CheckStaticParameters(parameters);

  // Derives the first frame's rotation so the indices are valid before SetFrameIndex runs.
  UpdateBufferIndices();
}

ReSTIRPTResamplingMode ReSTIRPTParameterContext::GetResamplingMode() const
{
  return m_ResamplingMode;
}

ReSTIRPTRuntimeParameters ReSTIRPTParameterContext::GetRuntimeParameters() const
{
  return m_RuntimeParameters;
}

ReSTIRPTReservoirBufferParameters ReSTIRPTParameterContext::GetReservoirBufferParameters() const
{
  return m_ReservoirBufferParameters;
}

ReSTIRPTBufferIndices ReSTIRPTParameterContext::GetBufferIndices() const
{
  return m_BufferIndices;
}

ReSTIRPTInitialSamplingParameters ReSTIRPTParameterContext::GetInitialSamplingParameters() const
{
  return m_InitialSamplingParameters;
}

ReSTIRPTShiftParameters ReSTIRPTParameterContext::GetShiftParameters() const
{
  return m_ShiftParameters;
}

ReSTIRPTTemporalResamplingParameters ReSTIRPTParameterContext::GetTemporalResamplingParameters() const
{
  return m_TemporalResamplingParameters;
}

ReSTIRPTSpatialResamplingParameters ReSTIRPTParameterContext::GetSpatialResamplingParameters() const
{
  return m_SpatialResamplingParameters;
}

ReSTIRPTDecorrelationParameters ReSTIRPTParameterContext::GetDecorrelationParameters() const
{
  return m_DecorrelationParameters;
}

ReSTIRPTShadingParameters ReSTIRPTParameterContext::GetShadingParameters() const
{
  return m_ShadingParameters;
}

ReSTIRPTNeeParameters ReSTIRPTParameterContext::GetNeeParameters() const
{
  return m_NeeParameters;
}

const ReSTIRPTStaticParameters& ReSTIRPTParameterContext::GetStaticParameters() const
{
  return m_StaticParameters;
}

uint32_t ReSTIRPTParameterContext::GetFrameIndex() const
{
  return m_RuntimeParameters.frameIndex;
}

void ReSTIRPTParameterContext::SetFrameIndex(uint32_t frameIndex)
{
  // Once-per-frame guard
  // Guards the once-per-recorded-frame contract documented on the declaration.
  // Advancing the rotation twice for one frame promotes a reservoir array that no pass wrote, which shows up later as corrupt reuse rather than as a crash, so it is worth catching at the call site.

  assert((!m_HasAdvancedOnce || frameIndex != m_RuntimeParameters.frameIndex) && "ReSTIRPTParameterContext::SetFrameIndex must be called once per recorded frame.");

  m_HasAdvancedOnce = true;

  // Advance

  m_RuntimeParameters.frameIndex = frameIndex;

  // Paired spatial reuse re-randomizes its self-inverting pairing textures every frame from this value; without it the same pixels would pair forever and the reuse pattern would burn into the image.
  m_RuntimeParameters.uniformRandomNumber = JenkinsHash(frameIndex);

  // Last-frame output becomes the history input for this frame.
  m_LastFrameOutputReservoir = m_CurrentFrameOutputReservoir;

  UpdateBufferIndices();
}

void ReSTIRPTParameterContext::SetResamplingMode(ReSTIRPTResamplingMode resamplingMode)
{
  m_ResamplingMode = resamplingMode;

  UpdateBufferIndices();
}

void ReSTIRPTParameterContext::SetInitialSamplingParameters(const ReSTIRPTInitialSamplingParameters& initialSamplingParameters)
{
  m_InitialSamplingParameters = initialSamplingParameters;
}

void ReSTIRPTParameterContext::SetShiftParameters(const ReSTIRPTShiftParameters& shiftParameters)
{
  m_ShiftParameters = shiftParameters;
}

void ReSTIRPTParameterContext::SetTemporalResamplingParameters(const ReSTIRPTTemporalResamplingParameters& temporalResamplingParameters)
{
  m_TemporalResamplingParameters = temporalResamplingParameters;
}

void ReSTIRPTParameterContext::SetSpatialResamplingParameters(const ReSTIRPTSpatialResamplingParameters& spatialResamplingParameters)
{
  m_SpatialResamplingParameters = spatialResamplingParameters;

  // Sigma is a derived quantity, never authored directly. Recomputing it here guarantees the paired and unpaired reuse paths always sample at the same mean distance, which is what makes the Section 3 ablation an apples-to-apples one.
  m_SpatialResamplingParameters.pairingSigma = CalculateReSTIRPTPairingSigma(m_SpatialResamplingParameters.samplingRadius);
}

void ReSTIRPTParameterContext::SetDecorrelationParameters(const ReSTIRPTDecorrelationParameters& decorrelationParameters)
{
  m_DecorrelationParameters = decorrelationParameters;
}

void ReSTIRPTParameterContext::SetShadingParameters(const ReSTIRPTShadingParameters& shadingParameters)
{
  m_ShadingParameters = shadingParameters;
}

void ReSTIRPTParameterContext::SetNeeParameters(const ReSTIRPTNeeParameters& neeParameters)
{
  m_NeeParameters = neeParameters;
}

void ReSTIRPTParameterContext::UpdateBufferIndices()
{
  const bool useTemporalResampling = m_ResamplingMode == ReSTIRPTResamplingMode::eTemporal || m_ResamplingMode == ReSTIRPTResamplingMode::eTemporalAndSpatial;
  const bool useSpatialResampling  = m_ResamplingMode == ReSTIRPTResamplingMode::eSpatial || m_ResamplingMode == ReSTIRPTResamplingMode::eTemporalAndSpatial;

  // Initial and temporal arrays
  // Reservoir buffers rotate so this frame can read history and write a fresh candidate.
  // Temporal output is placed in a different array than the previous history input.

  m_BufferIndices.initialSamplingOutputBufferIndex    = (m_LastFrameOutputReservoir + 1) % kReSTIRPTReservoirBufferCount;
  m_BufferIndices.temporalResamplingInputBufferIndex  = m_LastFrameOutputReservoir;
  m_BufferIndices.temporalResamplingOutputBufferIndex = (m_BufferIndices.temporalResamplingInputBufferIndex + 1) % kReSTIRPTReservoirBufferCount;

  // Spatial arrays
  // Spatial reuse consumes either the temporal result or the initial candidate - and those are the same array. Temporal writes its output in place over the initial candidate (it must load that candidate before overwriting it), so both expressions are (last + 1) for any reservoir count.
  // Written as one assignment rather than a ternary that cannot choose, so nobody reads the branch as evidence that the two arrays differ.

  m_BufferIndices.spatialResamplingInputBufferIndex  = m_BufferIndices.initialSamplingOutputBufferIndex;
  m_BufferIndices.spatialResamplingOutputBufferIndex = (m_BufferIndices.spatialResamplingInputBufferIndex + 1) % kReSTIRPTReservoirBufferCount;

  // Shading input
  // The last pass that runs feeds shading. With reuse fully disabled the initial candidate is what gets shaded, which is the 1spp path tracing baseline the renderer must match.

  if(useSpatialResampling)
  {
    m_BufferIndices.shadingInputBufferIndex = m_BufferIndices.spatialResamplingOutputBufferIndex;
  }
  else if(useTemporalResampling)
  {
    m_BufferIndices.shadingInputBufferIndex = m_BufferIndices.temporalResamplingOutputBufferIndex;
  }
  else
  {
    m_BufferIndices.shadingInputBufferIndex = m_BufferIndices.initialSamplingOutputBufferIndex;
  }

  // The shading input becomes the previous-frame reservoir when the next SetFrameIndex call advances the rotation.
  m_CurrentFrameOutputReservoir = m_BufferIndices.shadingInputBufferIndex;
}

}  // namespace rtpt
