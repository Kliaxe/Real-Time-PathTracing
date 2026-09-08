#ifndef RESTIR_PT_PARAMETERS_H
#define RESTIR_PT_PARAMETERS_H

// CPU/Slang shared parameter layout for ReSTIR PT Enhanced.
//
// Implemented from "ReSTIR PT Enhanced: Algorithmic Advances for Faster and More
// Robust ReSTIR Path Tracing" (Lin, Kettunen, Wyman; I3D 2026). Section numbers in
// the comments below refer to that paper; equation numbers likewise.
//
// Keep this file plain C-like: it is included by C++ and shader code, so every
// field must stay ABI-stable and explicitly padded. This mirrors the contract of
// the sibling ReSTIR DI file, ReSTIR/Parameters.h.

#ifdef __cplusplus
#include <stdint.h>
#else
// Guarded because ShaderIo.h pulls in both this file and the ReSTIR DI parameter
// header, and Slang warns on redefining the alias. Either header must still work
// when included on its own by a shader that uses only one of the two renderers.
#ifndef uint32_t
#define uint32_t uint
#endif
#endif // __cplusplus

// Reservoirs use the same block-linear storage scheme as ReSTIR DI so both
// renderers can share the pitch math in ReSTIRUtils. Measured in pixels.
#define RESTIR_PT_RESERVOIR_BLOCK_SIZE 16

// ---------------------------------------------------------------------------
// Shift mapping
// ---------------------------------------------------------------------------
// A shift maps a path sampled at one pixel into the domain of another pixel.
// Unlike ReSTIR DI - where reuse is a re-evaluation of a stored light sample -
// PT reuse is a partial re-trace, which is why these modes exist at all.
//
// Reconnection-only: connect the current pixel's primary hit straight to the
//   neighbor's stored reconnection vertex. Cheap, but loses all specular chains.
// Replay-only: re-trace the neighbor's path from the current pixel using their
//   stored random numbers. Handles specular, but never reconnects, so it is
//   expensive and degrades once paths diverge.
// Hybrid: replay the specular prefix, then reconnect once the reconnection
//   criteria are met. This is the ReSTIR PT default (Section 2.3).
#define RESTIR_PT_SHIFT_RECONNECTION 0
#define RESTIR_PT_SHIFT_REPLAY       1
#define RESTIR_PT_SHIFT_HYBRID       2

// Criteria deciding whether a candidate vertex is safe to reconnect at.
// Legacy is the original ReSTIR PT [Lin et al. 2022] pair of thresholds
// (distance AND roughness); these are interdependent and need per-scene tuning.
// Footprint is Section 4's replacement: a scene-independent test on ray
// footprints controlled by the single parameter kappa. Kept as a switch so the
// paper's ablation can be reproduced from the UI.
#define RESTIR_PT_RECONNECTION_CRITERIA_LEGACY    0
#define RESTIR_PT_RECONNECTION_CRITERIA_FOOTPRINT 1

// ---------------------------------------------------------------------------
// Duplication maps (Section 5)
// ---------------------------------------------------------------------------
// Correlation is detected by counting how many reservoirs in a square
// neighborhood carry the same initial random seed - i.e. are shifted copies of
// one initial candidate. The count is normalized by the divisor to a score in
// [0, 1]. Both values are fixed by the paper and are not user-tunable: the
// divisor is the saturation point of the count, not a quality knob.
#define RESTIR_PT_DUPLICATION_NEIGHBORHOOD 17
#define RESTIR_PT_DUPLICATION_DIVISOR      288.0f

// Paired spatial reuse (Section 3) preloads one pairing texture per spatial
// neighbor. Sizes are deliberately coprime-ish so their tilings never align and
// produce visible structure; the paper uses 254/230/210 for three neighbors.
#define RESTIR_PT_MAX_PAIRING_TEXTURES 3

// Section 6.1 light tiles, following Wyman and Panteleev [2021]. Each frame
// presamples this many tiles of this many lights; an 8x8 screen tile draws one
// tile and takes all its NEE candidates from it, so the lights a warp touches come
// from a few kilobytes rather than from the whole scene's CDF. Compile-time
// because they size the buffer.
#define RESTIR_PT_LIGHT_TILE_COUNT 128
#define RESTIR_PT_LIGHT_TILE_SIZE  1024
// Screen-space tile that shares one light tile.
#define RESTIR_PT_LIGHT_TILE_SCREEN_EXTENT 8

// Section 6.2.2. The spatial pre-pass shifts one path per (pixel, pairing slot).
// The pass this replaces looped all three slots inside a single invocation, so a
// warp cost three times its slowest lane; compacting the pairs that actually need
// a shift into a flat list and launching one invocation each is what Section 6.2.2
// means by parallelizing over pixel-neighbour pairs.
//
// Counter layout, in uints: the running append count, then the three dimensions of
// VkTraceRaysIndirectCommandKHR.
#define RESTIR_PT_PREPASS_COUNT_OFFSET    0u
#define RESTIR_PT_PREPASS_INDIRECT_OFFSET 1u
#define RESTIR_PT_PREPASS_COUNTER_UINTS   (RESTIR_PT_PREPASS_INDIRECT_OFFSET + 3u)

// Number of reservoir arrays the parameter context rotates between, using the
// same rotation as ReSTIR DI. For a previous-frame output array L, with two
// arrays (so the other one is ~L):
//   initial sampling writes  ~L
//   temporal reads history L and the candidate ~L, and writes ~L IN PLACE
//   spatial reads ~L (own pixel AND neighbours) and writes L
//   final shading reads whichever array the last enabled pass wrote
//
// Temporal deliberately aliases its output onto the initial candidate: it must
// combine this frame's candidate with last frame's reservoir, so the pass is
// required to load the candidate before overwriting that element. Spatial cannot
// alias, because it reads NEIGHBOURS - but it can write the history array, which
// is dead the moment temporal has run. temporalResamplingInputBufferIndex is read
// in exactly one place and never again afterwards.
//
// A third array used to sit here, on the stated grounds that spatial reuse needed
// somewhere to write that would not clobber the temporal input. That hazard does
// not exist, for the reason above, and the array cost 134 MB at 1080p.
//
// TWO INVARIANTS make two arrays safe, and both are load-bearing now rather than
// merely true:
//   1. Every producing pass writes EVERY pixel of its output array. There is no
//      spare array left to absorb a pass that early-outs without storing, so a
//      partially written array would expose the previous frame's contents.
//   2. Frames must be ordered against each other. With two arrays, frame N+1's
//      initial sampling overwrites the array frame N's spatial pass read
//      neighbours from, with zero passes of slack - see the unconditional
//      frame-start barrier in ReSTIRPTRenderer::RecordPasses.
#define RESTIR_PT_RESERVOIR_BUFFER_COUNT 2

// pathFlags bit layout for ReSTIRPTPackedReservoir. M is confidence weight, not
// a count of anything physical, and is capped well below 255 in practice.
#define RESTIR_PT_PATH_FLAGS_M_MASK        0x000000ffu
#define RESTIR_PT_PATH_FLAGS_M_SHIFT       0u
// Set when the stored path terminates on an NEE-sampled light vertex. Path MIS
// weights differ between NEE and BSDF-sampled endpoints, so the shift must know.
#define RESTIR_PT_PATH_FLAGS_NEE_ENDPOINT  0x00000100u
// Set when a reconnection vertex was found and stored. Without it the reservoir
// describes a fully-replayed path with no reconnection anchor.
#define RESTIR_PT_PATH_FLAGS_HAS_RC_VERTEX 0x00000200u
// Length of the replayed prefix, i.e. the index k of the reconnection vertex.
// Five bits hold 0..31, which must stay >= the largest permitted maxBounces:
// overflow here would silently corrupt the flag bits packed alongside it rather
// than fail, so the range is deliberately wider than any usable path length.
#define RESTIR_PT_PATH_FLAGS_RC_LENGTH_MASK  0x00007c00u
#define RESTIR_PT_PATH_FLAGS_RC_LENGTH_SHIFT 10u
#define RESTIR_PT_MAX_RC_LENGTH              31u

// The BSDF proposal groups selected at the reconnection vertex and at its
// predecessor. The supplemental preserves the lobe index across reconnection, and
// it must be: the shift redirects the path through a geometrically fixed direction
// that may fall in any lobe, so evaluating the full mixture instead of the
// originally sampled group would not reproduce the base path's factorization.
// Two bits each, matching SampleSurfaceBsdf's four groups
// (0 broad, 1 reflection, 2 glass, 3 clearcoat).
#define RESTIR_PT_PATH_FLAGS_RC_LOBE_MASK       0x00018000u
#define RESTIR_PT_PATH_FLAGS_RC_LOBE_SHIFT      15u
#define RESTIR_PT_PATH_FLAGS_RC_PREV_LOBE_MASK  0x00060000u
#define RESTIR_PT_PATH_FLAGS_RC_PREV_LOBE_SHIFT 17u

// Depth at which the selected candidate's contribution occurred. A path shifted
// by full random replay has to know where to stop and what to evaluate there, and
// it is also the quantity that says WHY a path carries no reconnection anchor:
// a contribution at depth 0 is direct lighting at the primary hit, which is too
// short for any reconnection vertex to exist.
#define RESTIR_PT_PATH_FLAGS_ENDPOINT_DEPTH_MASK  0x00F80000u
#define RESTIR_PT_PATH_FLAGS_ENDPOINT_DEPTH_SHIFT 19u
#define RESTIR_PT_MAX_ENDPOINT_DEPTH              31u

// What produced the selected contribution. A path with no reconnection anchor can
// only be shifted by replaying it to its end, and replay has to reproduce the same
// kind of term the base path ended on - emission seen through a BSDF bounce and an
// explicitly sampled environment direction are different estimators that happen to
// land at the same vertex.
#define RESTIR_PT_PATH_FLAGS_ENDPOINT_KIND_MASK  0x07000000u
#define RESTIR_PT_PATH_FLAGS_ENDPOINT_KIND_SHIFT 24u
// Interior contribution carrying a reconnection anchor; never replayed to its end.
#define RESTIR_PT_ENDPOINT_KIND_INTERIOR       0u
// Emission found by a BSDF-sampled ray, MIS-weighted against light sampling.
#define RESTIR_PT_ENDPOINT_KIND_BSDF_EMISSIVE  1u
// Explicitly sampled environment direction.
#define RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT    2u
// Explicitly sampled emissive triangle. Always carries a forced anchor
// (Section 6.2.3), so it never reaches the replay path.
#define RESTIR_PT_ENDPOINT_KIND_EMISSIVE_NEE   3u
// Environment reached because a BSDF-sampled ray left the scene. Unlike every
// other kind this endpoint is not a surface, so replay must let the final ray miss
// rather than treating an escape as a failed shift.
#define RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT_MISS 4u

// Bit per kind for ReSTIRPTShiftParameters::replayEndpointMask.
#define RESTIR_PT_REPLAY_BIT(kind) (1u << (kind))
// Every kind that FullReplayPathToSurface knows how to reproduce. Interior and
// emissive-NEE endpoints are absent because they always carry an anchor and are
// handled by the hybrid shift instead.
#define RESTIR_PT_REPLAY_MASK_ALL                                                                        (RESTIR_PT_REPLAY_BIT(RESTIR_PT_ENDPOINT_KIND_BSDF_EMISSIVE)                                            | RESTIR_PT_REPLAY_BIT(RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT)                                            | RESTIR_PT_REPLAY_BIT(RESTIR_PT_ENDPOINT_KIND_ENVIRONMENT_MISS))

// Upper bound on user-selectable path length, enforced by the UI and clamped
// again before reaching the shader. Bounded by the reconnection-length field.
#define RESTIR_PT_MAX_BOUNCES 31u

#ifndef __cplusplus
static const uint ReSTIRPTInvalidInstanceId = 0xffffffffu;
#else
#define ReSTIRPTInvalidInstanceId 0xffffffffu
#endif

// ---------------------------------------------------------------------------
// Packed reservoir
// ---------------------------------------------------------------------------
// One ReSTIR PT reservoir holds an entire path, which cannot be stored vertex by
// vertex. Instead it stores two random seeds plus a single geometric anchor:
//   - initRandomSeed regenerates the path prefix by replay,
//   - rcVertex* pins the reconnection vertex the shift reconnects to.
// The prefix is recomputed, never stored. That is the core space/time trade that
// makes ReSTIR PT viable at all.
//
// Layout follows Algorithm 1 of the supplemental, which compresses the original
// 88-byte reservoir to 64 bytes. Fields are declared as scalars (not vectors) so
// the struct has an identical footprint in C++ and Slang without alignment
// surprises. Storing F as RGB rather than a scalar is what later makes the
// Section 6.3 color-noise fix free: the vector integrand is already resident.
struct ReSTIRPTPackedReservoir
{
    // Unbiased contribution weight (the GRIS "W"), and the RGB integrand F.
    // The resampling target function is pHat = luminance(F).
    float ucw;
    float integrandR;
    float integrandG;
    float integrandB;

    // Seeds the replay of the path prefix and of the segment past the
    // reconnection vertex respectively.
    uint32_t initRandomSeed;
    uint32_t rcVertexRandomSeed;
    // M (8-bit) plus the path-type flags defined above.
    uint32_t pathFlags;
    // Reconnection vertex identity. Instance + primitive + barycentrics locates
    // it exactly without storing a world position, which would cost 12 bytes and
    // lose precision at distance.
    uint32_t rcVertexInstanceId;

    uint32_t rcVertexPrimitiveIndex;
    // Barycentrics as two 16-bit unorms; the third coordinate is implied.
    uint32_t rcVertexBarycentrics;
    // Incident direction at the reconnection vertex, octahedral-encoded as two
    // 16-bit unorms.
    uint32_t rcVertexWi;
    float    rcVertexRadianceR;

    float rcVertexRadianceG;
    float rcVertexRadianceB;
    // Denominator of Equation 2's Jacobian, evaluated on the base path:
    //   D_x = p_{k-1}(w_{k-1}, lobe_{k-1}) * G(x_{k-1} -> x_k) * p_k(w_k, lobe_k)
    // The three terms collapse into one product because only the product is ever
    // needed: the shift recomputes the same three terms in the target domain as
    // D_y and the Jacobian is D_y / D_x. Both p terms are JOINT lobe densities,
    // matching what SampleSurfaceBsdf divided by.
    float rcVertexJacobianTerms;
    // The source vertex's solid angle density for an NEE endpoint's light sample.
    //
    // UNUSED. Nothing reads it: it is assigned, packed and unpacked, and no shader
    // consumes it. An earlier version of this comment claimed path MIS weighting
    // needed it, which sent a later investigation looking for a missing Jacobian
    // that was never missing - the shift recomputes the destination density and is
    // correct to use jacobian = 1, because the RIS weight it multiplies against is
    // 1/(M * selection probability), a pure number carrying no source density.
    //
    // Kept rather than deleted so the packed reservoir stays exactly 64 bytes with
    // its final field at offset 60, both of which are asserted. Reclaiming these
    // four bytes would save ~8 MB per array at 1080p and risk a worse structured
    // stride, so it is not obviously a win - see the layout note above.
    float rcVertexNeeLightPdf;
};

// ---------------------------------------------------------------------------
// Per-pass parameter blocks
// ---------------------------------------------------------------------------

struct ReSTIRPTRuntimeParameters
{
    uint32_t frameIndex;
    // Per-frame random integer. Paired spatial reuse re-randomizes its pairing
    // textures every frame (flip/mirror/transpose/offset) because a pairing
    // texture is self-inverting and would otherwise repeat the same pairs.
    uint32_t uniformRandomNumber;
    uint32_t pad0;
    uint32_t pad1;
};

struct ReSTIRPTReservoirBufferParameters
{
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t pad0;
    uint32_t pad1;
};

struct ReSTIRPTBufferIndices
{
    uint32_t initialSamplingOutputBufferIndex;
    uint32_t temporalResamplingInputBufferIndex;
    uint32_t temporalResamplingOutputBufferIndex;
    uint32_t spatialResamplingInputBufferIndex;

    uint32_t spatialResamplingOutputBufferIndex;
    uint32_t shadingInputBufferIndex;
    uint32_t pad0;
    uint32_t pad1;
};

struct ReSTIRPTInitialSamplingParameters
{
    // Maximum path length. Unlike ReSTIR DI there is no separate "secondary
    // bounce" budget: Section 6.1 unifies direct and indirect light into one
    // reservoir, so a single limit governs the whole path.
    uint32_t maxBounces;

    // NEE light candidates drawn at the primary hit. Deeper bounces get fewer,
    // on the paper's inverse-square schedule: 32/i^2 at bounce i, clamped to >= 1.
    // Was neeCandidatesAtPrimary. No shader ever read it, while the UI slider
    // wrote it, so the control silently did nothing; the live field is
    // ReSTIRPTNeeParameters::primaryCandidates. Kept as padding to hold the
    // block layout, which is asserted.
    uint32_t pad3;

    // Importance-sample the environment map when generating NEE candidates.
    uint32_t environmentMapImportanceSampling;

    // Russian roulette (Section 6.2.4). Applied at initial sampling ONLY. It must
    // never run during random replay, otherwise the roulette would kill paths
    // that the base path survived and the shift would fail spuriously. That
    // restriction is a correctness requirement, not a tuning choice, so it is not
    // exposed as a separate flag.
    uint32_t enableRussianRoulette;

    // First bounce index at which roulette may terminate a path. Early bounces
    // carry most of the energy and are never rouletted.
    uint32_t russianRouletteStartBounce;

    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIRPTShiftParameters
{
    // One of RESTIR_PT_SHIFT_*. RESERVED - nothing reads this yet. The shift is
    // always hybrid; whether an anchorless path is replayed or refused is
    // replayEndpointMask below. Kept because the parameter block layout is
    // asserted, and left unread rather than given a UI control that does nothing.
    uint32_t shiftMapping;

    // One of RESTIR_PT_RECONNECTION_CRITERIA_*.
    uint32_t reconnectionCriteria;

    // Section 4, Equation 5. Bounds the change in area/solid-angle density that
    // reconnecting from a different previous vertex would cause, expressed as a
    // multiple of the primary ray footprint. Larger values are more conservative
    // and postpone reconnection (more replay, more cost, safer shift).
    // The paper's sweep finds per-scene optima in 0.005-0.04 across six very
    // different scenes, averaging 0.0175; 0.02 is the recommended constant.
    float footprintThreshold;

    // Section 4.2. Roughness floor at the vertex preceding the reconnection
    // vertex. Retained even under the footprint criteria as a cheap guard for
    // cases the footprint bound cannot cover: strong parallax, high curvature,
    // and reconnection to environment lights where the vertex is at infinity.
    //
    // UNITS: this is glTF *perceptual* roughness, matching SurfaceData::roughness,
    // which Intersection.h.slang clamps to [0.045, 1.0]. The Disney BSDF squares it
    // to get the GGX alpha, so a threshold of 0.2 here means alpha = 0.04. Compare
    // against the same quantity the surface reports or the test silently never
    // fires: a threshold meant for alpha applied to perceptual roughness (or the
    // reverse) is off by a square and looks like "reconnection never happens".
    float minRoughness;

    // Which endpoint kinds may be replayed to their end when a path found no
    // reconnection anchor. Bit k enables RESTIR_PT_ENDPOINT_KIND_k; zero disables
    // replay entirely and the shift is refused as before.
    //
    // A mask rather than a flag because the three replayable kinds reproduce three
    // different estimators, and a bias that only one of them introduces is
    // otherwise indistinguishable from a bias they all share. Enabling them one at
    // a time is the only way to attribute an energy difference to a branch.
    uint32_t replayEndpointMask;

    // Legacy criteria only (RESTIR_PT_RECONNECTION_CRITERIA_LEGACY): minimum
    // distance between the reconnection vertex and its predecessor, which avoids
    // the geometric singularity of connecting across a near-zero gap. Superseded
    // by the footprint test, kept so the ablation is meaningful.
    float legacyMinDistance;

    uint32_t pad0;
    uint32_t pad1;
};

struct ReSTIRPTTemporalResamplingParameters
{
    // Confidence weight cap ("M cap"). The paper's default is 20. When
    // decorrelation is enabled this is the upper end of the lerp, not the value
    // actually used - see ReSTIRPTDecorrelationParameters.
    uint32_t maxHistoryLength;

    // Relative depth similarity threshold for accepting a reprojected surface,
    // e.g. 0.1 means within 10% of the current depth.
    float depthThreshold;

    // Normal similarity threshold, compared against the dot product of the two
    // shading normals.
    float normalThreshold;

    // Section 6.4. Reproject disoccluded pixels with a second motion vector that
    // assumes the disoccluded surface moves consistently with its occluder,
    // recovering history that a single motion vector discards. ReSTIR PT can use
    // this safely because unbiased path resampling does not clone patterns the
    // way radiance-caching approaches would.
    uint32_t enableDualMotionVectors;
};

struct ReSTIRPTSpatialResamplingParameters
{
    // Spatial neighbor count. The paper uses 3; paired reuse (below) makes each
    // neighbor cost roughly half of an unpaired one.
    uint32_t numSamples;

    // Screen-space reuse radius in pixels, matching the ReSTIR PT default of 30.
    float samplingRadius;

    // Section 3. When enabled, pixels are paired so that if a reuses from b then
    // b also reuses from a, letting one pair of shifts serve both pixels' MIS
    // weights. Halves spatial shift work. When disabled the renderer falls back
    // to independent random neighbors, which needs two shifts per neighbor.
    uint32_t enablePairedSpatialReuse;

    // Standard deviation of the pairing texture's coordinate deltas, in pixels.
    // Derived from samplingRadius rather than set directly, so the paired and
    // unpaired paths sample at the same mean distance and are comparable:
    // sigma = sqrt(8 / (9*pi)) * radius, giving sigma = 16.0 at radius 30.
    float pairingSigma;

    float depthThreshold;
    float normalThreshold;

    // Cheap material comparison before paying for a shift. A shift onto a
    // dissimilar material is usually rejected anyway, so testing first saves the
    // expensive part.
    uint32_t enableMaterialSimilarityTest;

    uint32_t pad0;
};

// Section 3. One entry per paired spatial neighbour slot, describing where that
// slot's pairing texture lives and how it is re-randomized this frame.
struct ReSTIRPTPairingTextureParameters
{
    // Edge length in texels; the texture tiles across the screen. Zero marks the
    // slot unusable, which is how a failed generation disables paired reuse for
    // that neighbour rather than reading garbage.
    uint32_t size;
    // Index of this texture's first texel in the shared pairing buffer.
    uint32_t bufferOffset;
    // Per-frame symmetry: bit 0 swaps x/y, bit 1 flips x, bit 2 flips y. A pairing
    // texture is self-inverting, so reusing it unchanged would pair the same two
    // pixels every frame and correlate their samples permanently. Conjugating the
    // involution by a symmetry keeps it an involution while changing who pairs
    // with whom.
    uint32_t transform;
    // Per-frame translation, packed as y in the high 16 bits and x in the low 16.
    // Translation alone would not change the pairing structure, only its phase,
    // which is why it is combined with the symmetry above.
    uint32_t translation;
};

struct ReSTIRPTDecorrelationParameters
{
    // Section 5. Duplication maps trade a small, localized bias for a large
    // reduction in correlation artifacts (the paper measures 3.25% mean absolute
    // relative bias in a deliberately hostile scene). Bias only appears where the
    // cap is actually reduced, i.e. in already-correlated regions.
    uint32_t enable;

    // Lower end of the cap lerp, used where the duplication score saturates.
    // capUsed = lerp(maxHistoryLength, capMin, score^gamma).
    float capMin;

    // Shapes how fast the cap collapses as duplication rises. gamma = 1 is a
    // linear ramp; values near 0 (the paper uses 0.1) drop the cap sharply as
    // soon as any duplication appears.
    float gamma;

    uint32_t pad0;
};

struct ReSTIRPTShadingParameters
{
    // Section 6.3. Resampling picks samples by a scalar target function
    // (luminance), which leaves chroma poorly importance-sampled and shows up as
    // color noise. Accumulating vector-valued resampling weights and shading with
    // those marginalizes over the index choice. It is free here because F is
    // already evaluated during spatial reuse.
    uint32_t enableVectorWeights;

    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

struct ReSTIRPTNeeParameters
{
    // Section 6.1. Draw several NEE candidates from this pixel's light tile and
    // resample them, instead of taking the first light the CDF returns. Off falls
    // back to the original single-sample NEE, which is also the only mode the
    // reference path tracer uses - so the two remain independent estimators of the
    // same integral.
    uint32_t enableLightTiles;

    // Candidate count at the primary hit. The paper uses 32 there and decays it by
    // an inverse square in the bounce index, because deeper bounces contribute less
    // and are also where the divergence cost of extra candidates is worst.
    uint32_t primaryCandidates;

    uint32_t pad0;
    uint32_t pad1;
};

#endif // RESTIR_PT_PARAMETERS_H
