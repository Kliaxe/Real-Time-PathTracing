#include "PathTracing/Common/ResolveHistoryTracker.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <new>
#include <string_view>

namespace
{

using rtpt::RenderResolveMode;
using rtpt::ResolveHistoryTracker;

// FrameRecord
// Everything one BeginFrame/FinishFrame cycle reported, so a check can read the frame's decisions and the counter on both sides of FinishFrame.

struct FrameRecord
{
  // Decisions BeginFrame took for the frame.
  ResolveHistoryTracker::FrameState   state {};
  // What FinishFrame asked the owner to do with NRD.
  ResolveHistoryTracker::FinishResult finish {};
  // Counter between BeginFrame and FinishFrame, the value a renderer uploads for the frame.
  uint32_t                            countDuringFrame = 0;
  // Counter after FinishFrame, the value the next frame starts from.
  uint32_t                            countAfterFrame  = 0;
};

// Runs one complete frame through the tracker in the order ResolveHistory drives it.
FrameRecord RunFrame(ResolveHistoryTracker& tracker, const ResolveHistoryTracker::FrameInput& input)
{
  FrameRecord record;

  record.state            = tracker.BeginFrame(input);
  record.countDuringFrame = tracker.GetAccumulatedFrameCount();
  record.finish           = tracker.FinishFrame(record.state);
  record.countAfterFrame  = tracker.GetAccumulatedFrameCount();

  return record;
}

// Reports a failed expectation and returns the condition, so every check in a test runs and a test passes only if all of them hold.
bool Expect(bool condition, std::string_view message)
{
  if(!condition)
  {
    std::cerr << message << '\n';
  }

  return condition;
}

// Returns a scene info with a camera and environment set to distinct values. The tracker reads nothing else from it.
shaderio::GltfSceneInfo MakeSceneInfo()
{
  shaderio::GltfSceneInfo sceneInfo {};

  sceneInfo.viewProjMatrix          = glm::mat4(1.0f);
  sceneInfo.viewProjInvMatrix       = glm::mat4(1.0f);
  sceneInfo.viewInvMatrix           = glm::mat4(1.0f);
  sceneInfo.cameraPosition          = glm::vec3(0.0f, 1.0f, 5.0f);
  sceneInfo.useSky                  = 1;
  sceneInfo.useHdrEnv               = 0;
  sceneInfo.environmentTextureIndex = -1;
  sceneInfo.backgroundColor         = glm::vec3(0.1f, 0.2f, 0.3f);

  return sceneInfo;
}

// Moves the camera sideways without touching the environment, so only the accumulation signature changes.
void MoveCamera(shaderio::GltfSceneInfo& sceneInfo, float offset)
{
  sceneInfo.cameraPosition.x        += offset;
  sceneInfo.viewInvMatrix[3][0]     += offset;
  sceneInfo.viewProjInvMatrix[3][0] += offset;
  sceneInfo.viewProjMatrix[3][0]    -= offset;
}

// Returns frame input for sceneInfo in the given mode, with a fixed TLAS address and resolution.
ResolveHistoryTracker::FrameInput MakeInput(const shaderio::GltfSceneInfo& sceneInfo, RenderResolveMode resolveMode)
{
  return ResolveHistoryTracker::FrameInput { .sceneInfo = &sceneInfo, .topLevelAsAddress = 0x1000, .viewportSize = { 1280, 720 }, .resolveMode = resolveMode, .denoiserSignalsAvailable = true };
}

// The first accumulate frame has no history to continue, so it restarts. Unchanged inputs then continue the history and the counter grows by one per finished frame.
bool TestAccumulationStartsAndCounts()
{
  bool passed = true;

  ResolveHistoryTracker tracker;

  const shaderio::GltfSceneInfo           sceneInfo = MakeSceneInfo();
  const ResolveHistoryTracker::FrameInput input     = MakeInput(sceneInfo, RenderResolveMode::eAccumulate);

  const FrameRecord first = RunFrame(tracker, input);

  passed &= Expect(first.state.accumulateEnabled && first.state.accumulationRestarted, "the first accumulate frame must restart accumulation");
  passed &= Expect(first.countDuringFrame == 0 && first.countAfterFrame == 1, "the first accumulate frame must be counted as frame one");

  for(uint32_t frame = 2; frame <= 4; ++frame)
  {
    const FrameRecord record = RunFrame(tracker, input);

    passed &= Expect(!record.state.accumulationRestarted, "unchanged inputs must continue accumulation");
    passed &= Expect(record.countDuringFrame == frame - 1 && record.countAfterFrame == frame, "the counter must grow by one per finished accumulate frame");
  }

  return passed;
}

// A camera move changes what an accumulated pixel shows, so it restarts accumulation while accumulating.
// Outside accumulate mode it must not report a restart, because renderers wipe their own reuse history on one, and the counter stays at zero.
bool TestCameraChangeRestartsOnlyWhileAccumulating()
{
  bool passed = true;

  // Accumulate mode
  // A few steady frames build up history first, so the restart is caused by the move and not by the first frame.

  ResolveHistoryTracker accumulateTracker;

  shaderio::GltfSceneInfo accumulateScene = MakeSceneInfo();

  for(uint32_t frame = 0; frame < 3; ++frame)
  {
    RunFrame(accumulateTracker, MakeInput(accumulateScene, RenderResolveMode::eAccumulate));
  }

  MoveCamera(accumulateScene, 0.5f);

  const FrameRecord accumulateMove = RunFrame(accumulateTracker, MakeInput(accumulateScene, RenderResolveMode::eAccumulate));

  passed &= Expect(accumulateMove.state.accumulationRestarted, "a camera move must restart accumulation in accumulate mode");
  passed &= Expect(accumulateMove.countDuringFrame == 0 && accumulateMove.countAfterFrame == 1, "a camera move must restart the counter in accumulate mode");

  // Off and denoise modes
  // One warm-up frame first: the very first frame after construction restarts in every mode, because history starts out invalidated.

  for(const RenderResolveMode resolveMode : { RenderResolveMode::eOff, RenderResolveMode::eDenoise })
  {
    ResolveHistoryTracker tracker;

    shaderio::GltfSceneInfo sceneInfo = MakeSceneInfo();

    RunFrame(tracker, MakeInput(sceneInfo, resolveMode));

    MoveCamera(sceneInfo, 0.5f);

    const FrameRecord moved = RunFrame(tracker, MakeInput(sceneInfo, resolveMode));

    passed &= Expect(!moved.state.accumulationRestarted, "a camera move must not report an accumulation restart outside accumulate mode");
    passed &= Expect(moved.countDuringFrame == 0 && moved.countAfterFrame == 0, "the counter must stay at zero outside accumulate mode");
  }

  return passed;
}

// Frames rendered with accumulation off leave the counter at zero, so switching to accumulate with unchanged inputs starts counting from zero.
bool TestSwitchToAccumulateStartsFromZero()
{
  bool passed = true;

  ResolveHistoryTracker tracker;

  const shaderio::GltfSceneInfo sceneInfo = MakeSceneInfo();

  RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eOff));
  RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eOff));

  const FrameRecord first  = RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eAccumulate));
  const FrameRecord second = RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eAccumulate));

  passed &= Expect(first.countDuringFrame == 0 && first.countAfterFrame == 1, "switching to accumulate must start counting from zero");
  passed &= Expect(second.countDuringFrame == 1 && second.countAfterFrame == 2, "accumulation must keep counting after the switch");

  return passed;
}

// InvalidateHistory zeroes the counter at once and restarts both accumulation and NRD history on the next frame only.
bool TestInvalidateHistoryRestartsBothOnce()
{
  bool passed = true;

  const shaderio::GltfSceneInfo sceneInfo = MakeSceneInfo();

  // Accumulate mode
  // Inputs never change, so the only reason for a restart is the invalidation itself.

  ResolveHistoryTracker accumulateTracker;

  const ResolveHistoryTracker::FrameInput accumulateInput = MakeInput(sceneInfo, RenderResolveMode::eAccumulate);

  for(uint32_t frame = 0; frame < 3; ++frame)
  {
    RunFrame(accumulateTracker, accumulateInput);
  }

  accumulateTracker.InvalidateHistory();

  passed &= Expect(accumulateTracker.GetAccumulatedFrameCount() == 0, "InvalidateHistory must zero the counter immediately");

  const FrameRecord accumulateRestart = RunFrame(accumulateTracker, accumulateInput);
  const FrameRecord accumulateAfter   = RunFrame(accumulateTracker, accumulateInput);

  passed &= Expect(accumulateRestart.state.accumulationRestarted && accumulateRestart.state.denoiserHistoryInvalidated, "the frame after InvalidateHistory must restart accumulation and denoiser history");
  passed &= Expect(!accumulateAfter.state.accumulationRestarted && !accumulateAfter.state.denoiserHistoryInvalidated, "InvalidateHistory must restart history for one frame only");

  // Denoise mode
  // NRD history only continues on denoise frames, so its restart is checked there, against a steady frame that kept it.

  ResolveHistoryTracker denoiseTracker;

  const ResolveHistoryTracker::FrameInput denoiseInput = MakeInput(sceneInfo, RenderResolveMode::eDenoise);

  RunFrame(denoiseTracker, denoiseInput);

  const FrameRecord denoiseSteady = RunFrame(denoiseTracker, denoiseInput);

  denoiseTracker.InvalidateHistory();

  const FrameRecord denoiseRestart = RunFrame(denoiseTracker, denoiseInput);
  const FrameRecord denoiseAfter   = RunFrame(denoiseTracker, denoiseInput);

  passed &= Expect(!denoiseSteady.state.denoiserHistoryInvalidated, "unchanged denoise frames must keep NRD history");
  passed &= Expect(denoiseRestart.state.accumulationRestarted && denoiseRestart.state.denoiserHistoryInvalidated, "the frame after InvalidateHistory must restart both histories in denoise mode");
  passed &= Expect(!denoiseAfter.state.accumulationRestarted && !denoiseAfter.state.denoiserHistoryInvalidated, "InvalidateHistory must restart denoise history for one frame only");

  return passed;
}

// A frame that does not denoise leaves NRD's history describing an image sequence it no longer follows, so the tracker must ask for it to be dropped and restart it on the next denoise frame.
bool TestSkippedDenoiseDropsNrdHistory()
{
  bool passed = true;

  ResolveHistoryTracker tracker;

  const shaderio::GltfSceneInfo sceneInfo = MakeSceneInfo();

  RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));

  const FrameRecord denoised = RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));
  const FrameRecord skipped  = RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eOff));
  const FrameRecord resumed  = RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));

  passed &= Expect(denoised.state.denoiseEnabled && !denoised.finish.dropDenoiserHistory, "a denoise frame must keep NRD history");
  passed &= Expect(!skipped.state.denoiseEnabled && skipped.finish.dropDenoiserHistory, "a frame that does not denoise must drop NRD history");
  passed &= Expect(resumed.state.denoiseEnabled && resumed.state.denoiserHistoryInvalidated, "the next denoise frame must restart NRD history");

  return passed;
}

// Without signals that describe the beauty image, NRD must stand down even in denoise mode, exactly as if denoising were off.
bool TestUnavailableSignalsDisableDenoise()
{
  bool passed = true;

  ResolveHistoryTracker tracker;

  const shaderio::GltfSceneInfo sceneInfo = MakeSceneInfo();

  RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));

  ResolveHistoryTracker::FrameInput unavailableInput = MakeInput(sceneInfo, RenderResolveMode::eDenoise);

  unavailableInput.denoiserSignalsAvailable = false;

  const FrameRecord unavailable = RunFrame(tracker, unavailableInput);
  const FrameRecord available   = RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));

  passed &= Expect(!unavailable.state.denoiseEnabled, "unavailable denoiser signals must disable denoising in denoise mode");
  passed &= Expect(unavailable.finish.dropDenoiserHistory, "unavailable denoiser signals must drop NRD history");
  passed &= Expect(available.state.denoiseEnabled && available.state.denoiserHistoryInvalidated, "denoising must restart NRD history once signals are available again");

  return passed;
}

// NRD reprojects with motion vectors, so a pure camera move keeps its history. A change to what is lit, with the camera still, must restart it.
bool TestDenoiserHistoryIgnoresCamera()
{
  bool passed = true;

  ResolveHistoryTracker tracker;

  shaderio::GltfSceneInfo sceneInfo = MakeSceneInfo();

  RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));
  RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));

  MoveCamera(sceneInfo, 0.5f);

  const FrameRecord cameraMove = RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));

  sceneInfo.backgroundColor = glm::vec3(0.9f, 0.8f, 0.7f);

  const FrameRecord environmentChange = RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));
  const FrameRecord unchanged         = RunFrame(tracker, MakeInput(sceneInfo, RenderResolveMode::eDenoise));

  passed &= Expect(!cameraMove.state.denoiserHistoryInvalidated, "a pure camera move must keep NRD history");
  passed &= Expect(environmentChange.state.denoiserHistoryInvalidated, "a denoiser signature change with the camera still must restart NRD history");
  passed &= Expect(!unchanged.state.denoiserHistoryInvalidated, "NRD history must continue once the environment is stable again");

  return passed;
}

// Signatures are compared bytewise, so building one from identical inputs must give identical bytes whatever the memory held before.
// Each signature is constructed straight into storage pre-filled with 0x00 or 0xFF; any byte its construction leaves unwritten would differ between the two.
bool TestSignaturePaddingIsDeterministic()
{
  bool passed = true;

  using AccumulationSignature = ResolveHistoryTracker::AccumulationSignature;
  using DenoiserSignature     = ResolveHistoryTracker::DenoiserSignature;
  using FrameState            = ResolveHistoryTracker::FrameState;

  shaderio::GltfSceneInfo sceneInfo = MakeSceneInfo();

  const ResolveHistoryTracker::FrameInput input = MakeInput(sceneInfo, RenderResolveMode::eDenoise);

  // Signature construction
  // The prvalue each Make function returns initializes the object in the pre-filled storage directly, so its bytes are exactly what construction wrote.

  alignas(AccumulationSignature) std::array<std::byte, sizeof(AccumulationSignature)> accumulationZeros;
  alignas(AccumulationSignature) std::array<std::byte, sizeof(AccumulationSignature)> accumulationOnes;
  alignas(DenoiserSignature) std::array<std::byte, sizeof(DenoiserSignature)>         denoiserZeros;
  alignas(DenoiserSignature) std::array<std::byte, sizeof(DenoiserSignature)>         denoiserOnes;

  std::memset(accumulationZeros.data(), 0x00, accumulationZeros.size());
  std::memset(accumulationOnes.data(), 0xFF, accumulationOnes.size());
  std::memset(denoiserZeros.data(), 0x00, denoiserZeros.size());
  std::memset(denoiserOnes.data(), 0xFF, denoiserOnes.size());

  const AccumulationSignature* accumulationFromZeros = new(accumulationZeros.data()) AccumulationSignature(ResolveHistoryTracker::MakeAccumulationSignature(input));
  const AccumulationSignature* accumulationFromOnes  = new(accumulationOnes.data()) AccumulationSignature(ResolveHistoryTracker::MakeAccumulationSignature(input));
  const DenoiserSignature*     denoiserFromZeros     = new(denoiserZeros.data()) DenoiserSignature(ResolveHistoryTracker::MakeDenoiserSignature(input));
  const DenoiserSignature*     denoiserFromOnes      = new(denoiserOnes.data()) DenoiserSignature(ResolveHistoryTracker::MakeDenoiserSignature(input));

  passed &= Expect(ResolveHistoryTracker::SignaturesMatch(*accumulationFromZeros, *accumulationFromOnes), "accumulation signatures from identical inputs must match regardless of prior memory contents");
  passed &= Expect(ResolveHistoryTracker::SignaturesMatch(*denoiserFromZeros, *denoiserFromOnes), "denoiser signatures from identical inputs must match regardless of prior memory contents");

  // Frame state construction
  // The signatures a real frame stores come out of BeginFrame inside a FrameState, so the same must hold there.

  alignas(FrameState) std::array<std::byte, sizeof(FrameState)> frameZeros;
  alignas(FrameState) std::array<std::byte, sizeof(FrameState)> frameOnes;

  std::memset(frameZeros.data(), 0x00, frameZeros.size());
  std::memset(frameOnes.data(), 0xFF, frameOnes.size());

  ResolveHistoryTracker zerosTracker;
  ResolveHistoryTracker onesTracker;

  const FrameState* frameFromZeros = new(frameZeros.data()) FrameState(zerosTracker.BeginFrame(input));
  const FrameState* frameFromOnes  = new(frameOnes.data()) FrameState(onesTracker.BeginFrame(input));

  passed &= Expect(ResolveHistoryTracker::SignaturesMatch(frameFromZeros->accumulationSignature, frameFromOnes->accumulationSignature), "frame accumulation signatures from identical inputs must match regardless of prior memory contents");
  passed &= Expect(ResolveHistoryTracker::SignaturesMatch(frameFromZeros->denoiserSignature, frameFromOnes->denoiserSignature), "frame denoiser signatures from identical inputs must match regardless of prior memory contents");

  // Discrimination
  // Matching must still see real differences, or the checks above would pass trivially.

  MoveCamera(sceneInfo, 0.5f);

  const ResolveHistoryTracker::FrameInput movedInput = MakeInput(sceneInfo, RenderResolveMode::eDenoise);

  passed &= Expect(!ResolveHistoryTracker::SignaturesMatch(*accumulationFromZeros, ResolveHistoryTracker::MakeAccumulationSignature(movedInput)), "a camera move must change the accumulation signature");
  passed &= Expect(ResolveHistoryTracker::SignaturesMatch(*denoiserFromZeros, ResolveHistoryTracker::MakeDenoiserSignature(movedInput)), "a camera move must not change the denoiser signature");

  return passed;
}

}  // namespace

int main()
{
  // Every test runs even after an earlier one fails, so one run reports every broken rule.

  bool passed = true;

  passed &= TestAccumulationStartsAndCounts();
  passed &= TestCameraChangeRestartsOnlyWhileAccumulating();
  passed &= TestSwitchToAccumulateStartsFromZero();
  passed &= TestInvalidateHistoryRestartsBothOnce();
  passed &= TestSkippedDenoiseDropsNrdHistory();
  passed &= TestUnavailableSignalsDisableDenoise();
  passed &= TestDenoiserHistoryIgnoresCamera();
  passed &= TestSignaturePaddingIsDeterministic();

  return passed ? 0 : 1;
}
