#include "ApplicationOptions.h"

#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
// Parses a command line given without the executable name.
// ParseApplicationOptions takes a mutable argv the way main receives it, so the arguments are copied into owned strings first. The result copies every value it keeps, so it outlives them.
rtpt::ApplicationOptionsParseResult Parse(std::initializer_list<std::string_view> arguments)
{
  std::vector<std::string> storage { "RealTimePathTracing" };

  for(const std::string_view argument : arguments)
  {
    storage.emplace_back(argument);
  }

  std::vector<char*> argv;

  for(std::string& argument : storage)
  {
    argv.push_back(argument.data());
  }

  return rtpt::ParseApplicationOptions(static_cast<int>(argv.size()), argv.data());
}
}  // namespace

int main()
{
  // Resolve mode spellings
  // Every spelling the capture scenarios use must select its enum value, in both the separate and the joined form.

  const std::pair<std::string_view, rtpt::RenderResolveMode> resolveSpellings[] {
    { "off", rtpt::RenderResolveMode::eOff },
    { "accumulate", rtpt::RenderResolveMode::eAccumulate },
    { "denoise", rtpt::RenderResolveMode::eDenoiseNrd },
    { "denoise-rr", rtpt::RenderResolveMode::eDenoiseRayReconstruction },
  };

  for(const auto& [spelling, resolveMode] : resolveSpellings)
  {
    const std::string joined = "--resolve-mode=" + std::string(spelling);

    const rtpt::ApplicationOptionsParseResult separateForm = Parse({ "--resolve-mode", spelling });
    const rtpt::ApplicationOptionsParseResult joinedForm   = Parse({ joined });

    if(!separateForm.error.empty() || separateForm.options.resolveMode != resolveMode || !joinedForm.error.empty() || joinedForm.options.resolveMode != resolveMode)
    {
      std::cerr << "--resolve-mode " << spelling << " did not select its resolve mode\n";
      return 1;
    }
  }

  // Resolve mode name round trip
  // Capture tooling passes GetResolveModeName spellings back on the command line, so each name must parse to the mode it came from.

  for(const auto& [spelling, resolveMode] : resolveSpellings)
  {
    const rtpt::ApplicationOptionsParseResult parsed = Parse({ "--resolve-mode", rtpt::GetResolveModeName(resolveMode) });

    if(!parsed.error.empty() || parsed.options.resolveMode != resolveMode || rtpt::GetResolveModeName(resolveMode) != spelling)
    {
      std::cerr << "GetResolveModeName does not round-trip for " << spelling << '\n';
      return 1;
    }
  }

  // Resolve mode errors and absence
  // An unknown or missing value must be rejected rather than falling back to a default, and leaving the option out must not pick a mode so each renderer keeps its own default.

  if(Parse({ "--resolve-mode", "denoised" }).error.empty() || Parse({ "--resolve-mode" }).error.empty() || Parse({ "--resolve-mode=" }).error.empty())
  {
    std::cerr << "an invalid or missing --resolve-mode value was accepted\n";
    return 1;
  }

  const rtpt::ApplicationOptionsParseResult withoutResolveMode = Parse({ "--headless", "--renderer", "path-tracer" });

  if(!withoutResolveMode.error.empty() || withoutResolveMode.options.resolveMode.has_value())
  {
    std::cerr << "omitting --resolve-mode still selected a resolve mode\n";
    return 1;
  }

  // Reference comparison and denoising
  // The ReSTIR reference comparison needs raw or accumulated radiance, so an explicit denoise request is rejected while accumulate is accepted.

  if(Parse({ "--renderer", "restir-pt", "--restir-reference", "--resolve-mode", "denoise" }).error.empty() || Parse({ "--renderer", "restir-pt", "--restir-reference", "--resolve-mode", "denoise-rr" }).error.empty() || !Parse({ "--renderer", "restir-pt", "--restir-reference", "--resolve-mode", "accumulate" }).error.empty())
  {
    std::cerr << "--restir-reference does not gate the resolve mode correctly\n";
    return 1;
  }

  // Renderer
  // Like the resolve mode, renderer spellings are written into capture metadata and must parse back to the same renderer.

  for(const rtpt::RenderMode renderMode : { rtpt::RenderMode::eRasterizer, rtpt::RenderMode::ePathTracing, rtpt::RenderMode::eReSTIRPTEnhanced })
  {
    const rtpt::ApplicationOptionsParseResult parsed = Parse({ "--renderer", rtpt::GetRenderModeName(renderMode) });

    if(!parsed.error.empty() || parsed.options.renderMode != renderMode)
    {
      std::cerr << "GetRenderModeName does not round-trip for " << rtpt::GetRenderModeName(renderMode) << '\n';
      return 1;
    }
  }

  if(Parse({ "--renderer", "pathtracer" }).error.empty())
  {
    std::cerr << "an unknown --renderer value was accepted\n";
    return 1;
  }

  // Frame count
  // Headless runs render exactly this many frames, so zero and values with trailing characters are rejected.

  const rtpt::ApplicationOptionsParseResult frames = Parse({ "--frames", "64" });

  if(!frames.error.empty() || frames.options.frameCount != 64)
  {
    std::cerr << "--frames 64 did not set the frame count\n";
    return 1;
  }

  if(Parse({ "--frames", "0" }).error.empty() || Parse({ "--frames", "12px" }).error.empty() || Parse({ "--frames" }).error.empty())
  {
    std::cerr << "an invalid --frames value was accepted\n";
    return 1;
  }

  // Capture prefix
  // Captures are only written at the end of a headless run, so the prefix is rejected without --headless.

  if(Parse({ "--capture-prefix", "capture" }).error.empty())
  {
    std::cerr << "--capture-prefix was accepted without --headless\n";
    return 1;
  }

  const rtpt::ApplicationOptionsParseResult capture = Parse({ "--headless", "--capture-prefix", "captures/frame" });

  if(!capture.error.empty() || capture.options.capturePrefix != std::filesystem::path("captures/frame"))
  {
    std::cerr << "--capture-prefix with --headless did not set the prefix\n";
    return 1;
  }

  // Profile output
  // The timing report is written at the end of a headless run, so like the capture prefix it is rejected without --headless, and a missing value is an error rather than an empty path.

  if(Parse({ "--profile-output", "profile.json" }).error.empty() || Parse({ "--headless", "--profile-output" }).error.empty() || Parse({ "--headless", "--profile-output=" }).error.empty())
  {
    std::cerr << "--profile-output was accepted without --headless or without a value\n";
    return 1;
  }

  const rtpt::ApplicationOptionsParseResult profile       = Parse({ "--headless", "--profile-output", "profiles/run.json" });
  const rtpt::ApplicationOptionsParseResult joinedProfile = Parse({ "--headless", "--profile-output=profiles/run.json", "--capture-prefix", "captures/frame" });

  if(!profile.error.empty() || profile.options.profileOutput != std::filesystem::path("profiles/run.json") || !joinedProfile.error.empty() || joinedProfile.options.profileOutput != std::filesystem::path("profiles/run.json") || !joinedProfile.options.capturePrefix.has_value())
  {
    std::cerr << "--profile-output with --headless did not set the output path\n";
    return 1;
  }

  const rtpt::ApplicationOptionsParseResult withoutProfile = Parse({ "--headless" });

  if(!withoutProfile.error.empty() || withoutProfile.options.profileOutput.has_value())
  {
    std::cerr << "omitting --profile-output still selected an output path\n";
    return 1;
  }

  return 0;
}
