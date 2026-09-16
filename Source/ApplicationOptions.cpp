#include "ApplicationOptions.h"

#include <charconv>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace rtpt
{
namespace
{

// Parses a whole string as an unsigned integer, leaving value untouched on failure.
template <typename T>
bool ParseUnsigned(std::string_view text, T& value)
{
  if(text.empty())
  {
    return false;
  }

  T parsed {};

  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);

  // from_chars rejects a sign for unsigned types; the end check also rejects trailing characters such as "12px".
  if(error != std::errc {} || end != text.data() + text.size())
  {
    return false;
  }

  value = parsed;

  return true;
}

// Reads an option value written as "--option value" or "--option=value". False means a different option or a missing value, so callers check IsValueOption first.
bool ReadOptionValue(int& argumentIndex, int argc, char** argv, std::string_view argument, std::string_view option, std::string_view& value, std::string& error)
{
  // Separate form: the value is the next argument, which is consumed by advancing the caller's index.
  if(argument == option)
  {
    if(argumentIndex + 1 >= argc)
    {
      error = "Missing value for " + std::string(option);
      return false;
    }

    value = argv[++argumentIndex];

    return true;
  }

  // Joined form: the value follows the equals sign within the same argument.

  const std::string optionWithEquals = std::string(option) + "=";

  if(argument.starts_with(optionWithEquals))
  {
    value = argument.substr(optionWithEquals.size());

    if(value.empty())
    {
      error = "Missing value for " + std::string(option);
      return false;
    }

    return true;
  }

  return false;
}

// True when the argument names the option in either the separate or the joined form.
bool IsValueOption(std::string_view argument, std::string_view option)
{
  return argument == option || argument.starts_with(std::string(option) + "=");
}

}  // namespace

ApplicationOptionsParseResult ParseApplicationOptions(int argc, char** argv)
{
  ApplicationOptionsParseResult result {};

  // Arguments
  // Each argument is matched against every known option in turn. The first failure returns immediately with result.error set,
  // so later arguments are never interpreted against a half-parsed command line.

  for(int argumentIndex = 1; argumentIndex < argc; ++argumentIndex)
  {
    const std::string_view argument = argv[argumentIndex];

    // Flags
    // Flags take no value, so matching the argument is enough.

    if(argument == "--help" || argument == "-h")
    {
      result.showHelp = true;
      continue;
    }

    if(argument == "--restir-reference")
    {
      result.options.restirReference = true;
      continue;
    }

    if(argument == "--headless")
    {
      result.options.headless = true;
      continue;
    }

    if(argument == "--validation-sync")
    {
      result.options.synchronizationValidation = true;
      continue;
    }

    // Value options
    // Numeric options parse into a local first so an invalid value never lands in the options.

    std::string_view value;

    if(IsValueOption(argument, "--resolve-mode"))
    {
      if(!ReadOptionValue(argumentIndex, argc, argv, argument, "--resolve-mode", value, result.error))
      {
        return result;
      }

      // Matching against GetResolveModeName makes every accepted spelling round-trip through the name function.
      std::optional<RenderResolveMode> resolveMode;

      for(const RenderResolveMode candidate : { RenderResolveMode::eOff, RenderResolveMode::eAccumulate, RenderResolveMode::eDenoise })
      {
        if(value == GetResolveModeName(candidate))
        {
          resolveMode = candidate;
        }
      }

      if(!resolveMode)
      {
        result.error = "--resolve-mode must be off, accumulate, or denoise";
        return result;
      }

      result.options.resolveMode = resolveMode;

      continue;
    }

    if(IsValueOption(argument, "--frames"))
    {
      if(!ReadOptionValue(argumentIndex, argc, argv, argument, "--frames", value, result.error))
      {
        return result;
      }

      if(!ParseUnsigned(value, result.options.frameCount) || result.options.frameCount == 0)
      {
        result.error = "--frames must be a positive integer";
        return result;
      }

      continue;
    }

    if(IsValueOption(argument, "--width"))
    {
      uint32_t width {};

      if(!ReadOptionValue(argumentIndex, argc, argv, argument, "--width", value, result.error))
      {
        return result;
      }

      if(!ParseUnsigned(value, width) || width == 0)
      {
        result.error = "--width must be a positive integer";
        return result;
      }

      result.options.width = width;

      continue;
    }

    if(IsValueOption(argument, "--height"))
    {
      uint32_t height {};

      if(!ReadOptionValue(argumentIndex, argc, argv, argument, "--height", value, result.error))
      {
        return result;
      }

      if(!ParseUnsigned(value, height) || height == 0)
      {
        result.error = "--height must be a positive integer";
        return result;
      }

      result.options.height = height;

      continue;
    }

    if(IsValueOption(argument, "--scene-index"))
    {
      size_t sceneIndex {};

      if(!ReadOptionValue(argumentIndex, argc, argv, argument, "--scene-index", value, result.error))
      {
        return result;
      }

      // Only the syntax is checked here; the catalog size is unknown until assets are discovered.
      if(!ParseUnsigned(value, sceneIndex))
      {
        result.error = "--scene-index must be a non-negative integer";
        return result;
      }

      result.options.sceneIndex = sceneIndex;

      continue;
    }

    if(IsValueOption(argument, "--renderer"))
    {
      if(!ReadOptionValue(argumentIndex, argc, argv, argument, "--renderer", value, result.error))
      {
        return result;
      }

      // These spellings must match GetRenderModeName so capture metadata round-trips to the command line.
      if(value == "raster")
      {
        result.options.renderMode = RenderMode::eRasterizer;
      }
      else if(value == "path-tracer")
      {
        result.options.renderMode = RenderMode::ePathTracing;
      }
      else if(value == "restir-pt")
      {
        result.options.renderMode = RenderMode::eReSTIRPTEnhanced;
      }
      else
      {
        result.error = "--renderer must be raster, path-tracer, or restir-pt";
        return result;
      }

      continue;
    }

    if(IsValueOption(argument, "--capture-prefix"))
    {
      if(!ReadOptionValue(argumentIndex, argc, argv, argument, "--capture-prefix", value, result.error))
      {
        return result;
      }

      result.options.capturePrefix = std::filesystem::path(value);

      continue;
    }

    if(IsValueOption(argument, "--profile-output"))
    {
      if(!ReadOptionValue(argumentIndex, argc, argv, argument, "--profile-output", value, result.error))
      {
        return result;
      }

      result.options.profileOutput = std::filesystem::path(value);

      continue;
    }

    result.error = "Unknown argument: " + std::string(argument);

    return result;
  }

  // Cross-option validation
  // These rules depend on the whole command line, so they run after every argument has been seen.
  // Capturing and the profile report only happen at the end of a headless run, which is why a capture prefix or profile output without --headless is rejected.

  if(result.options.restirReference && (result.options.renderMode != RenderMode::eReSTIRPTEnhanced || result.options.resolveMode == RenderResolveMode::eDenoise))
  {
    result.error = "--restir-reference requires --renderer restir-pt and --resolve-mode off or accumulate";
    return result;
  }

  if(result.options.width.has_value() != result.options.height.has_value())
  {
    result.error = "--width and --height must be supplied together";
  }
  else if(result.options.capturePrefix.has_value() && !result.options.headless)
  {
    result.error = "--capture-prefix requires --headless";
  }
  else if(result.options.profileOutput.has_value() && !result.options.headless)
  {
    result.error = "--profile-output requires --headless";
  }

  return result;
}

std::string GetApplicationUsage(const char* executableName)
{
  // Only the file name is shown so the usage line does not depend on how the executable was launched.
  const std::string name = executableName != nullptr ? std::filesystem::path(executableName).filename().string() : "RealTimePathTracing";

  return "Usage: " + name + " [options]\n"
         "  --headless                 Run without a window\n"
         "  --frames <count>           Frames to render in headless mode\n"
         "  --width <pixels>           Viewport width; requires --height\n"
         "  --height <pixels>          Viewport height; requires --width\n"
         "  --scene-index <index>      Scene catalog index\n"
         "  --renderer <name>          raster, path-tracer, or restir-pt\n"
         "  --resolve-mode <mode>      off, accumulate, or denoise\n"
         "  --restir-reference         Compare identical paths without ReSTIR selection or reuse\n"
         "  --capture-prefix <path>    Write .linear.hdr, .final.png, and .json\n"
         "  --profile-output <path>    Write per-pass GPU timings as JSON; requires --headless\n"
         "  --validation-sync          Enable synchronization validation\n"
         "  --help, -h                 Show this help\n";
}

const char* GetRenderModeName(RenderMode renderMode)
{
  switch(renderMode)
  {
    case RenderMode::eRasterizer:
      return "raster";
    case RenderMode::ePathTracing:
      return "path-tracer";
    case RenderMode::eReSTIRPTEnhanced:
      return "restir-pt";
  }

  return "unknown";
}

const char* GetResolveModeName(RenderResolveMode resolveMode)
{
  switch(resolveMode)
  {
    case RenderResolveMode::eOff:
      return "off";
    case RenderResolveMode::eAccumulate:
      return "accumulate";
    case RenderResolveMode::eDenoise:
      return "denoise";
  }

  return "unknown";
}

}  // namespace rtpt
