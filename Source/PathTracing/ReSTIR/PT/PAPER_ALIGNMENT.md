# ReSTIR PT Enhanced - alignment with the paper

Daqi Lin, Markus Kettunen, Chris Wyman, "ReSTIR PT Enhanced: Algorithmic Advances
for Faster and More Robust ReSTIR Path Tracing", Proc. ACM Comput. Graph. Interact.
Tech. 9(1), Article 13, May 2026 (I3D 2026).
https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/

Everything below is checked against the paper text, not against recollection. An
earlier version of this file was written from memory and got two things backwards -
it credited the sort to the paper (the paper asks for stream compaction, which is
what is implemented) and treated Section 5's bias as a defect (the paper introduces
it deliberately). Where the paper is silent, this says so.

## Technique coverage

| Paper | Status | Note |
|---|---|---|
| 3 reciprocal paired spatial reuse | implemented | two shifts per pair, not four |
| 4 footprint reconnection criteria | implemented | |
| 4.2 single-vertex roughness threshold | implemented | |
| 5 duplication maps | implemented | |
| 6.1 unified direct + global illumination | implemented | |
| 6.2.1 low-level code optimization | **partial** | reservoir storage is 64 B as the paper specifies; the branch-to-conditional-move rewrite was deliberately deferred |
| 6.2.2 stream compaction for random replay | implemented | on by default |
| 6.2.3 forced NEE light reconnection | implemented | |
| 6.2.4 Russian roulette at initial sampling only | implemented | removed from replay, density corrected in primary sample space |
| 6.3 reducing color noise | implemented | vector-valued resampling weights |
| 6.4 dual motion vectors | implemented | |

Not implemented, deliberately: selectable reconnection-only and replay-only shift
mappings. Those are the paper's baselines for comparison, not contributions of
Enhanced.

## Constants, each verified against the paper text

| Constant | Paper | Here | |
|---|---|---|---|
| footprint threshold c (Eq. 5) | 0.02 | 0.02 | matches |
| roughness threshold alpha | 0.2 | 0.2 | matches |
| temporal confidence cap c_Cap | 20 | 20 | matches |
| spatial neighbours | 3 | 3 | matches |
| duplication map neighbourhood | 17x17, divided by 288 | 17x17, divided by 288 | matches |
| Section 5 c_Cap^min | 1 | 1 | matches |
| Section 5 alpha | 0.1 | 0.1 | matches |
| **spatial reuse radius** | **30 px** | **6 px** | **DEVIATION** |

The paper's wording on the last two groups, for anyone re-checking: "we found
c_Cap^min = 1 and alpha = 0.1 yield good results", and "we borrow ReSTIR PT's default
temporal and spatial parameters, namely c_Cap = 20 for temporal resampling and use of
3 random spatial neighbors in a 30-pixel radius".

## The one real deviation: spatial radius 6 instead of 30

At the paper's radius of 30 the shift Jacobians here misbehave. Mean forward Jacobian
on Cornell Box, and the Jacobian round-trip error that a bijective shift must keep
near zero:

| | radius 6 | radius 30 |
|---|---|---|
| paired, mean J | 1.004 | **1.661** |
| unpaired, mean J | 1.003 | 1.037 |
| paired, mean abs(J_fwd*J_inv - 1) | 0.042 | **0.608** |
| unpaired, same | 0.041 | 0.202 |

Two separate effects. A mild degradation with radius in both modes, which is at least
plausible - more parallax makes shifts harder. And a much larger PAIRED-specific one:
16x more excess over one, and a round-trip error of 0.6 where the shift is supposed
to be a bijection.

The footprint criterion has been cleared as the cause. Equation 5's right-hand side
and its constant are verified against the paper, and all three plausible readings of
its left-hand side were implemented and measured - the paired figure sits at 1.66 in
every one of them (see the comment in PTReservoir.hlsli). So whatever makes paired
reuse degrade with radius is elsewhere, and is unexplained. Until it is found, the
radius stays at 6, which is the one place this implementation knowingly departs from
the paper's parameters.

## Section 5 is deliberately biased - this is not a defect

Worth stating because it was misread here once, at a cost. The paper:

> "Because this approach adjusts confidence weights based on specific samples, the
> partition of unity of MIS weights m_i is violated, introducing a small bias. This
> bias only occurs in correlated areas where we modify c_Cap; essentially our
> approach trades correlation for bias."

It reports 3.25% average absolute relative bias on a hard scene. Measured here, the
same setting costs up to about 6%. That is the intended trade, and the acceptance
gate's tolerance was raised to 7% to stop it rejecting the paper's own parameter.

The failure mode Section 5 exists to prevent is described by the paper as
"low-probability, high-energy fireflies... spread to neighbor pixels via
spatiotemporal reuse, forming correlation blobs... persisting over many frames".
That is precisely the runaway documented below, which was caused here by tuning
alpha up to 0.5 to reduce the bias - buying accuracy by discarding the containment.

## Two deliberate departures, each measured rather than assumed

**6.2.2 compacts a wider set than the paper does.** The paper discards "pairs that do
not require replay"; this discards pairs that fail the surface-compatibility test,
which is a larger set kept. Implementing the paper's narrower criterion would need
the traced pass split in two - a cheap reconnection-only path and a compacted replay
path - so it was measured before being attempted. The fraction of paths with NO
reconnection anchor, i.e. those that need full replay:

| scene | needs replay |
|---|---|
| Cornell Box | ~49% |
| Bunny Dielectric | 99.8% |
| Bunny Metallic | 99.8% |

On the bunny scenes essentially everything needs replay, so the narrower criterion
would discard almost nothing and the split would buy nothing. On Cornell it could
help roughly half the pairs. Not worth the restructuring on this evidence; revisit if
a workload appears whose paths mostly reconnect.

**6.2.1 is half done, and the remaining half is not worth doing here.** The paper's
two items are reducing reservoir storage from 88 to 64 bytes - done, the packed
reservoir is exactly 64 - and replacing branches with conditional moves. The branch
work targets the spatial resampling pass, which measures 0.57 ms of a 21.5 ms frame
on Cornell Box: a 2.7% ceiling on the whole optimisation. An earlier attempt also
found several of the obvious sites unsafe (the zero-weight guard also guards an RNG
draw, so removing it changes the sampler; a bijectivity guard prevents 0 * inf).
Deliberately left; the frame time is in initial sampling, not here.

## Resolved: the divergence that kept 6.2.2 disabled

For a long stretch this renderer diverged with 6.2.2's compaction enabled - energy
growing frame over frame until the image was infinite. It is fixed, and the cause is
worth recording because the investigation took a long time and reached three wrong
conclusions first.

**The cause was Section 5's alpha set to 0.5 instead of the paper's 0.1.** That was a
local retuning, made to remove the bias Section 5 deliberately introduces. Removing
the bias also removed the containment: alpha controls how early the confidence cap
engages, and at 0.5 it engages too late to stop a high-energy sample replicating
through reuse - exactly the failure the paper describes it as preventing. Six camera
poses on Bunny Metallic at 1200 frames: five diverged at alpha 0.5, none at 0.1.

Blamed and cleared along the way: paired reuse, within-frame reuse symmetry, and the
compacted pre-pass itself. Each looked well-supported and each was refuted by the
next measurement.

Two things remain true and are worth knowing before changing either constant:

- **6.2.2's compaction depends on Section 5.** With decorrelation disabled it still
  diverges, so the two are not independent switches: the compaction is the most
  sensitive consumer of the confidence cap.
- **Alpha must not drift back up.** At 0.5 the compacted pre-pass diverges on five of
  six camera poses on Bunny Metallic at 1200 accumulated frames; at the paper's 0.1,
  none. Raising it to buy back accuracy is what caused the failure once already.

Three lessons that generalise, all of which cost real time here:

1. **Check the specification before investigating.** Six rounds of measurement were
   spent on what a constant in the paper would have answered in ten minutes. Several
   "boundaries" established along the way - a confidence-cap threshold, a slot-count
   threshold, a paired-versus-unpaired split - were only the shape of the mistuning.
2. **Instrumenting this renderer perturbs it.** Adding a debug write to three shaders
   moved the failure across builds. Anything measured by editing these shaders and
   watching the symptom move is not evidence.
3. **Diagnostics measured the wrong quantity.** Every per-pass statistic here
   described shift quality; the failure lived entirely in reservoir weight magnitude
   and was invisible until a ucw tail was added to the report.
