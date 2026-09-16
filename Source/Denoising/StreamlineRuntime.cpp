#include "StreamlineRuntime.h"

#include <cstring>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include "Framework/Platform/Log.h"

#if RTPT_WITH_STREAMLINE

#include <windows.h>

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_d.h>
#include <sl_helpers.h>

// Streamline API forwarders
// The SDK headers declare the core API as plain extern "C" functions and expect them to come from linking sl.interposer.lib. The interposer is loaded at runtime here instead, so the executable still starts where the DLLs are missing, and these definitions forward each call to the export of the loaded module.
// Defining them under the SDK's own names keeps the SDK's inline helpers usable: slDLSSDSetOptions in sl_dlss_d.h, for example, reaches its plugin through slGetFeatureFunction.
// The module handle is file-scope because extern "C" functions cannot reach a class member, and Streamline itself allows a single session per process.

namespace
{

HMODULE g_StreamlineModule = nullptr;

// Mirrors CreateInfo::presentsFrames for the log callback, which, as a plain function pointer, cannot reach the runtime.
bool g_StreamlinePresentsFrames = true;

// Resolves one export of the interposer. A missing module or export reports eErrorNotInitialized rather than crashing, so a call that slips past the availability checks fails like any other Streamline error.
template<typename Function>
Function* FindStreamlineExport(const char* name)
{
  return g_StreamlineModule != nullptr ? reinterpret_cast<Function*>(GetProcAddress(g_StreamlineModule, name)) : nullptr;
}

}  // namespace

extern "C" sl::Result slInit(const sl::Preferences& pref, uint64_t sdkVersion)
{
  const auto function = FindStreamlineExport<PFun_slInit>("slInit");
  return function != nullptr ? function(pref, sdkVersion) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slShutdown()
{
  const auto function = FindStreamlineExport<PFun_slShutdown>("slShutdown");
  return function != nullptr ? function() : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo)
{
  const auto function = FindStreamlineExport<PFun_slIsFeatureSupported>("slIsFeatureSupported");
  return function != nullptr ? function(feature, adapterInfo) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function)
{
  const auto forward = FindStreamlineExport<PFun_slGetFeatureFunction>("slGetFeatureFunction");
  return forward != nullptr ? forward(feature, functionName, function) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex)
{
  const auto function = FindStreamlineExport<PFun_slGetNewFrameToken>("slGetNewFrameToken");
  return function != nullptr ? function(token, frameIndex) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport)
{
  const auto function = FindStreamlineExport<PFun_slSetConstants>("slSetConstants");
  return function != nullptr ? function(values, frame, viewport) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* resources, uint32_t numResources, sl::CommandBuffer* cmdBuffer)
{
  const auto function = FindStreamlineExport<PFun_slSetTagForFrame>("slSetTagForFrame");
  return function != nullptr ? function(frame, viewport, resources, numResources, cmdBuffer) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer)
{
  const auto function = FindStreamlineExport<PFun_slEvaluateFeature>("slEvaluateFeature");
  return function != nullptr ? function(feature, frame, inputs, numInputs, cmdBuffer) : sl::Result::eErrorNotInitialized;
}

extern "C" sl::Result slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport)
{
  const auto function = FindStreamlineExport<PFun_slFreeResources>("slFreeResources");
  return function != nullptr ? function(feature, viewport) : sl::Result::eErrorNotInitialized;
}

#endif  // RTPT_WITH_STREAMLINE

namespace rtpt
{

#if RTPT_WITH_STREAMLINE

namespace
{

// Identifies this renderer to NGX. Streamline requires either an NVIDIA-issued application id or an engine type with a project id; a project id is any stable GUID.
constexpr const char* kProjectId = "5c1d9a4e-7b2f-4c61-9e0d-3f8a2b6c7d10";

// Checks that a DLL carries a valid Authenticode signature from NVIDIA before it is loaded.
// Streamline's own sl::security::verifyEmbeddedSignature needs WINTRUST_SIGNATURE_SETTINGS, which the MinGW Windows headers do not declare, so this is the same check without the secondary-signature count: the chain must verify, and its leaf certificate must name NVIDIA.
// Revocation is not checked, so a machine without network access can still start.
bool VerifyNvidiaSignature(const std::filesystem::path& path)
{
  WINTRUST_FILE_INFO fileInfo {};

  fileInfo.cbStruct      = sizeof(fileInfo);
  fileInfo.pcwszFilePath = path.c_str();

  WINTRUST_DATA trustData {};

  trustData.cbStruct            = sizeof(trustData);
  trustData.dwUIChoice          = WTD_UI_NONE;
  trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
  trustData.dwUnionChoice       = WTD_CHOICE_FILE;
  trustData.pFile               = &fileInfo;
  trustData.dwStateAction       = WTD_STATEACTION_VERIFY;
  trustData.dwProvFlags         = WTD_CACHE_ONLY_URL_RETRIEVAL;

  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;

  const LONG status = WinVerifyTrust(nullptr, &action, &trustData);

  // Signer
  // Only a verified chain is inspected. The state data must be closed whether or not verification passed.

  bool signedByNvidia = false;

  if(status == ERROR_SUCCESS)
  {
    CRYPT_PROVIDER_DATA* providerData = WTHelperProvDataFromStateData(trustData.hWVTStateData);
    CRYPT_PROVIDER_SGNR* signer       = providerData != nullptr ? WTHelperGetProvSignerFromChain(providerData, 0, FALSE, 0) : nullptr;
    CRYPT_PROVIDER_CERT* certificate  = signer != nullptr ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;

    if(certificate != nullptr)
    {
      wchar_t subject[256] {};

      CertGetNameStringW(certificate->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject, static_cast<DWORD>(std::size(subject)));

      signedByNvidia = std::wstring_view(subject).starts_with(L"NVIDIA");
    }
  }

  trustData.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &action, &trustData);

  return signedByNvidia;
}

// Routes Streamline's log into the renderer's log. Info lines are dropped: the development plugins print dozens of them at startup, and warnings and errors are what explain a missing feature.
void ForwardStreamlineLog(sl::LogType type, const char* message)
{
  if(type == sl::LogType::eInfo || message == nullptr)
  {
    return;
  }

  std::string_view text(message);

  while(!text.empty() && (text.back() == '\n' || text.back() == '\r'))
  {
    text.remove_suffix(1);
  }

  // Expected messages
  // Streamline reports these on every run of this integration, and none of them is a fault:
  // - sl.common always tries to load Reflex's NvLowLatencyVk.dll. Reflex is not used, so the DLL is not shipped.
  // - Without VK_EXT_debug_utils, which Release builds do not enable, it only loses object names and profiling markers.
  // - Streamline warns that Vulkan was called before slInit as soon as the interposer loads, even in a program that makes no Vulkan call first.
  // - The command buffer state-tracking hooks do not exist under manual hooking. They restore pipeline state after an evaluation, which this renderer does not need: every pass binds its own pipeline and descriptors.
  // - Headless runs never present, so presentCommon never runs. A 1500-frame headless run peaked at the same process memory as a 100-frame run, so the skipped bookkeeping does not accumulate. In a run that presents the same message means the present hook is missing, so it is only dropped when nothing presents.

  constexpr std::string_view expectedMessages[] = {
      "NvLowLatencyVk",
      "VK_EXT_debug_utils extension not enabled",
      "were invoked before slInit()",
      "is NOT supported, plugin will not function properly",
  };

  for(const std::string_view expected : expectedMessages)
  {
    if(text.find(expected) != std::string_view::npos)
    {
      return;
    }
  }

  if(!g_StreamlinePresentsFrames && text.find("presentCommon() was not observed") != std::string_view::npos)
  {
    return;
  }

  Log(type == sl::LogType::eError ? LogLevel::Error : LogLevel::Warning, fmt::format("Streamline: {}", text));
}

// Streamline's float4x4 is row-major for row vectors, which stores exactly the columns of glm's column-major, column-vector matrix: row i is glm column i. The bytes therefore copy straight across.
sl::float4x4 ToStreamline(const glm::mat4& matrix)
{
  sl::float4x4 result;

  for(uint32_t row = 0; row < 4; ++row)
  {
    result.setRow(row, sl::float4(matrix[row][0], matrix[row][1], matrix[row][2], matrix[row][3]));
  }

  return result;
}

sl::DLSSDPreset ToStreamline(RayReconstructionPreset preset)
{
  switch(preset)
  {
    case RayReconstructionPreset::eD:
      return sl::DLSSDPreset::ePresetD;
    case RayReconstructionPreset::eE:
      return sl::DLSSDPreset::ePresetE;
    case RayReconstructionPreset::eDefault:
      break;
  }

  return sl::DLSSDPreset::eDefault;
}

// Describes one image to Streamline. Vulkan resources need their description, and the layout the image is in when Streamline records its dispatch.
sl::Resource MakeResource(const StreamlineRuntime::RayReconstructionImage& image)
{
  sl::Resource resource(sl::ResourceType::eTex2d, image.image, nullptr, image.view, VK_IMAGE_LAYOUT_GENERAL);

  resource.width        = image.extent.width;
  resource.height       = image.extent.height;
  resource.nativeFormat = static_cast<uint32_t>(image.format);
  resource.mipLevels    = 1;
  resource.arrayLayers  = 1;
  resource.flags        = 0;
  resource.usage        = image.usage;

  return resource;
}

}  // namespace

#endif  // RTPT_WITH_STREAMLINE

StreamlineRuntime::~StreamlineRuntime()
{
  Shutdown();

#if RTPT_WITH_STREAMLINE
  // The module is released last: volk's function table points into it until the process ends, so it is only unloaded when nothing can call Vulkan any more.
  if(m_Module != nullptr)
  {
    FreeLibrary(static_cast<HMODULE>(m_Module));

    m_Module           = nullptr;
    g_StreamlineModule = nullptr;
  }
#endif
}

void StreamlineRuntime::Initialize(const CreateInfo& createInfo)
{
#if !RTPT_WITH_STREAMLINE
  static_cast<void>(createInfo);

  MarkUnavailable("this build was configured without Streamline");
#else
  // A second Initialize would load a second session, which Streamline does not support.
  if(m_Module != nullptr)
  {
    return;
  }

  // Load
  // Production builds refuse an interposer without NVIDIA's signature, so a DLL planted next to the executable cannot inject itself into the Vulkan loader. Development DLLs are not signed, so Debug builds skip the check.

  const std::filesystem::path interposerPath = createInfo.pluginDirectory / "sl.interposer.dll";

  if(!std::filesystem::exists(interposerPath))
  {
    MarkUnavailable("sl.interposer.dll was not found next to the executable");
    return;
  }

#if RTPT_STREAMLINE_PRODUCTION
  if(!VerifyNvidiaSignature(interposerPath))
  {
    MarkUnavailable("sl.interposer.dll does not carry a valid NVIDIA signature");
    return;
  }
#endif

  m_Module = LoadLibraryW(interposerPath.c_str());

  if(m_Module == nullptr)
  {
    MarkUnavailable(fmt::format("sl.interposer.dll could not be loaded (Windows error {})", GetLastError()));
    return;
  }

  g_StreamlineModule         = static_cast<HMODULE>(m_Module);
  g_StreamlinePresentsFrames = createInfo.presentsFrames;

  // Session
  // Manual hooking keeps Streamline out of every Vulkan call except the few it has to see: volk still dispatches straight to the driver for everything else.
  // Frame-based tagging ties tags to a frame token, which keeps each evaluation's inputs separate from the next frame recorded while it is in flight.
  // Over-the-air updates are off so a run is reproducible with the DLLs the build pinned.

  const std::wstring pluginDirectory = createInfo.pluginDirectory.wstring();
  const wchar_t*     pluginPaths[]   = { pluginDirectory.c_str() };
  const sl::Feature  features[]      = { sl::kFeatureDLSS_RR };

  sl::Preferences preferences {};

  preferences.showConsole        = false;
  preferences.logLevel           = sl::LogLevel::eDefault;
  preferences.pathsToPlugins     = pluginPaths;
  preferences.numPathsToPlugins  = 1;
  preferences.logMessageCallback = ForwardStreamlineLog;
  preferences.flags              = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eDisableCLStateTracking;
  preferences.featuresToLoad     = features;
  preferences.numFeaturesToLoad  = 1;
  preferences.engine             = sl::EngineType::eCustom;
  preferences.engineVersion      = "0.1";
  preferences.projectId          = kProjectId;
  preferences.renderAPI          = sl::RenderAPI::eVulkan;

  const sl::Result result = slInit(preferences, sl::kSDKVersion);

  if(result != sl::Result::eOk)
  {
    FreeLibrary(static_cast<HMODULE>(m_Module));

    m_Module           = nullptr;
    g_StreamlineModule = nullptr;

    MarkUnavailable(fmt::format("Streamline failed to initialize ({})", sl::getResultAsStr(result)));
    return;
  }

  m_Initialized       = true;
  m_UnavailableReason = "Ray Reconstruction support has not been checked yet";
#endif
}

void StreamlineRuntime::Shutdown()
{
#if RTPT_WITH_STREAMLINE
  // slShutdown releases every feature resource Streamline still holds, so it must run while the device exists.
  if(m_Initialized)
  {
    slShutdown();
  }
#endif

  m_Initialized                = false;
  m_RayReconstructionSupported = false;
}

PFN_vkGetInstanceProcAddr StreamlineRuntime::GetVulkanEntryPoint() const
{
#if RTPT_WITH_STREAMLINE
  // The interposer's vkGetInstanceProcAddr returns the driver's functions for everything except the calls Streamline intercepts: instance and device creation, which it extends with DLSS's requirements, and the swapchain and present calls it needs to see each frame.
  if(m_Initialized)
  {
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(static_cast<HMODULE>(m_Module), "vkGetInstanceProcAddr"));
  }
#endif

  return nullptr;
}

void StreamlineRuntime::CheckRayReconstructionSupport(VkPhysicalDevice physicalDevice)
{
#if RTPT_WITH_STREAMLINE
  if(!m_Initialized)
  {
    return;
  }

  // The check is against the device the renderer actually picked: Streamline can only drive one adapter, and a laptop's integrated GPU would never qualify.

  sl::AdapterInfo adapterInfo {};

  adapterInfo.vkPhysicalDevice = physicalDevice;

  const sl::Result result = slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo);

  m_RayReconstructionSupported = result == sl::Result::eOk;

  if(!m_RayReconstructionSupported)
  {
    MarkUnavailable(fmt::format("DLSS Ray Reconstruction is not supported on this GPU or driver ({})", sl::getResultAsStr(result)));
    return;
  }

  m_UnavailableReason.clear();

  Log(LogLevel::Info, "DLSS Ray Reconstruction is available");
#else
  static_cast<void>(physicalDevice);
#endif
}

bool StreamlineRuntime::IsRayReconstructionAvailable() const
{
  return m_Initialized && m_RayReconstructionSupported;
}

const std::string& StreamlineRuntime::GetUnavailableReason() const
{
  return m_UnavailableReason;
}

uint32_t StreamlineRuntime::AllocateViewportId()
{
  return m_NextViewportId++;
}

bool StreamlineRuntime::EvaluateRayReconstruction(const RayReconstructionFrame& frame)
{
#if !RTPT_WITH_STREAMLINE
  static_cast<void>(frame);

  return false;
#else
  if(!IsRayReconstructionAvailable() || frame.cmd == VK_NULL_HANDLE)
  {
    return false;
  }

  // A failure is logged once per distinct result rather than every frame, so a persistent problem does not flood the log.
  const auto failed = [this](sl::Result result, const char* step) {
    if(result == sl::Result::eOk)
    {
      return false;
    }

    if(static_cast<uint32_t>(result) != m_LastReportedFailure)
    {
      Log(LogLevel::Error, fmt::format("DLSS Ray Reconstruction: {} failed ({})", step, sl::getResultAsStr(result)));

      m_LastReportedFailure = static_cast<uint32_t>(result);
    }

    return true;
  };

  // Frame token
  // Every evaluation gets a fresh frame index, and the options, constants, tags, and evaluation below must all quote the same token and viewport.

  sl::FrameToken* frameToken = nullptr;
  const uint32_t  frameIndex = m_FrameIndex++;

  if(failed(slGetNewFrameToken(frameToken, &frameIndex), "slGetNewFrameToken") || frameToken == nullptr)
  {
    return false;
  }

  const sl::ViewportHandle viewport(frame.viewportId);

  // Options
  // DLAA: the renderer always renders at the output resolution, so Ray Reconstruction denoises and anti-aliases without upscaling. Every mode's preset is set so the choice holds whatever mode Streamline resolves to.
  // The view matrices are only read for specular hit distances, which are not tagged, but Streamline validates them anyway.

  const glm::mat4 cameraToWorld = glm::inverse(frame.viewMatrix);
  const sl::DLSSDPreset preset  = ToStreamline(frame.preset);

  sl::DLSSDOptions options {};

  options.mode                   = sl::DLSSMode::eDLAA;
  options.outputWidth            = frame.output.extent.width;
  options.outputHeight           = frame.output.extent.height;
  options.colorBuffersHDR        = sl::Boolean::eTrue;
  options.normalRoughnessMode    = sl::DLSSDNormalRoughnessMode::ePacked;
  options.worldToCameraView      = ToStreamline(frame.viewMatrix);
  options.cameraViewToWorld      = ToStreamline(cameraToWorld);
  options.dlaaPreset             = preset;
  options.qualityPreset          = preset;
  options.balancedPreset         = preset;
  options.performancePreset      = preset;
  options.ultraPerformancePreset = preset;
  options.ultraQualityPreset     = preset;

  if(failed(slDLSSDSetOptions(viewport, options), "slDLSSDSetOptions"))
  {
    return false;
  }

  // Constants
  // Motion vectors already hold camera motion as screen-UV deltas, which are [-1, 1] normalized, so their scale is one. They exclude jitter, and depth is standard [0, 1] with the near plane at zero.
  // The camera basis comes from the inverse view matrix of a right-handed view space that looks down -Z.

  sl::Constants constants {};

  constants.cameraViewToClip        = ToStreamline(frame.projectionMatrix);
  constants.clipToCameraView        = ToStreamline(glm::inverse(frame.projectionMatrix));
  constants.clipToLensClip          = ToStreamline(glm::mat4(1.0f));
  constants.clipToPrevClip          = ToStreamline(frame.clipToPreviousClip);
  constants.prevClipToClip          = ToStreamline(glm::inverse(frame.clipToPreviousClip));
  constants.jitterOffset            = sl::float2(frame.jitterOffset.x, frame.jitterOffset.y);
  constants.mvecScale               = sl::float2(1.0f, 1.0f);
  constants.cameraPinholeOffset     = sl::float2(0.0f, 0.0f);
  constants.cameraPos               = sl::float3(cameraToWorld[3].x, cameraToWorld[3].y, cameraToWorld[3].z);
  constants.cameraUp                = sl::float3(cameraToWorld[1].x, cameraToWorld[1].y, cameraToWorld[1].z);
  constants.cameraRight             = sl::float3(cameraToWorld[0].x, cameraToWorld[0].y, cameraToWorld[0].z);
  constants.cameraFwd               = sl::float3(-cameraToWorld[2].x, -cameraToWorld[2].y, -cameraToWorld[2].z);
  constants.cameraNear              = frame.nearPlane;
  constants.cameraFar               = frame.farPlane;
  constants.cameraFOV               = frame.verticalFov;
  constants.cameraAspectRatio       = frame.aspectRatio;
  constants.depthInverted           = sl::Boolean::eFalse;
  constants.cameraMotionIncluded    = sl::Boolean::eTrue;
  constants.motionVectors3D         = sl::Boolean::eFalse;
  constants.reset                   = frame.resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  constants.orthographicProjection  = frame.orthographic ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  constants.motionVectorsDilated    = sl::Boolean::eFalse;
  constants.motionVectorsJittered   = sl::Boolean::eFalse;

  if(failed(slSetConstants(constants, *frameToken, viewport), "slSetConstants"))
  {
    return false;
  }

  // Tags
  // The images stay untouched until this frame is presented, so no tag needs Streamline to copy it. Every one covers its whole image.

  sl::Resource color                 = MakeResource(frame.color);
  sl::Resource output                = MakeResource(frame.output);
  sl::Resource depth                 = MakeResource(frame.depth);
  sl::Resource motionVectors         = MakeResource(frame.motionVectors);
  sl::Resource diffuseAlbedo         = MakeResource(frame.diffuseAlbedo);
  sl::Resource specularAlbedo        = MakeResource(frame.specularAlbedo);
  sl::Resource normalRoughness       = MakeResource(frame.normalRoughness);
  sl::Resource specularMotionVectors = MakeResource(frame.specularMotionVectors);

  const sl::Extent inputExtent { 0, 0, frame.color.extent.width, frame.color.extent.height };
  const sl::Extent outputExtent { 0, 0, frame.output.extent.width, frame.output.extent.height };

  const sl::ResourceTag tags[] = {
      sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent),
      sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &outputExtent),
      sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent),
      sl::ResourceTag(&motionVectors, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent),
      sl::ResourceTag(&diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent),
      sl::ResourceTag(&specularAlbedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent),
      sl::ResourceTag(&normalRoughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent),
      sl::ResourceTag(&specularMotionVectors, sl::kBufferTypeSpecularMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent),
  };

  const auto commandBuffer = reinterpret_cast<sl::CommandBuffer*>(frame.cmd);

  if(failed(slSetTagForFrame(*frameToken, viewport, tags, static_cast<uint32_t>(std::size(tags)), commandBuffer), "slSetTagForFrame"))
  {
    return false;
  }

  // Evaluate
  // Records Ray Reconstruction's dispatches into the command buffer. With command list state tracking disabled, the pipeline and descriptor bindings it leaves behind are the host's to replace, which every later pass does by binding its own.

  const sl::BaseStructure* inputs[] = { &viewport };

  return !failed(slEvaluateFeature(sl::kFeatureDLSS_RR, *frameToken, inputs, static_cast<uint32_t>(std::size(inputs)), commandBuffer), "slEvaluateFeature");
#endif
}

void StreamlineRuntime::FreeRayReconstruction(uint32_t viewportId)
{
#if RTPT_WITH_STREAMLINE
  // Streamline allocates a viewport's feature resources on its first evaluation; freeing a viewport that never evaluated is harmless.
  if(m_Initialized)
  {
    slFreeResources(sl::kFeatureDLSS_RR, sl::ViewportHandle(viewportId));
  }
#else
  static_cast<void>(viewportId);
#endif
}

void StreamlineRuntime::MarkUnavailable(std::string reason)
{
  Log(LogLevel::Warning, fmt::format("DLSS Ray Reconstruction is unavailable: {}", reason));

  m_RayReconstructionSupported = false;
  m_UnavailableReason          = std::move(reason);
}

}  // namespace rtpt
