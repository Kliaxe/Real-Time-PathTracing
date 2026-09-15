#include "SceneAssetCatalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <unordered_set>

#include "Framework/Platform/Paths.h"
#include "SceneCatalog.h"

namespace rtpt
{

namespace
{

// Path comparisons in this file are case-insensitive, so the same asset matches regardless of how a scene or the file system spells it.
std::string ToLowerAscii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  return value;
}

bool HasAnyExtension(const std::filesystem::path& path, const std::span<const char* const> extensions)
{
  const std::string ext = ToLowerAscii(path.extension().string());

  for(const char* candidate : extensions)
  {
    if(ext == candidate)
    {
      return true;
    }
  }

  return false;
}

std::vector<AssetEntry> DiscoverAssetsInDir(const std::filesystem::path& subDir, const std::span<const char* const> extensions)
{
  std::vector<AssetEntry>         assets;
  std::unordered_set<std::string> seenPaths;

  // Content directories
  // Every content directory is scanned. Paths are stored relative to the directory they were found in, and the first directory to provide a relative path wins, so later directories cannot duplicate an entry.
  // File system errors skip the offending entry instead of aborting discovery.

  for(const auto& contentDir : rtpt::ContentDirectories())
  {
    const std::filesystem::path rootDir = contentDir / subDir;
    std::error_code             ec;

    if(!std::filesystem::exists(rootDir, ec))
    {
      continue;
    }

    for(const auto& entry : std::filesystem::recursive_directory_iterator(rootDir, std::filesystem::directory_options::skip_permission_denied))
    {
      if(!entry.is_regular_file(ec) || ec)
      {
        ec.clear();
        continue;
      }

      const std::filesystem::path assetFile = entry.path();

      if(!HasAnyExtension(assetFile, extensions))
      {
        continue;
      }

      const std::filesystem::path relativePath = std::filesystem::relative(assetFile, contentDir, ec);

      if(ec)
      {
        continue;
      }

      const std::string relativeText = relativePath.generic_string();
      const std::string key          = ToLowerAscii(relativeText);

      if(!seenPaths.insert(key).second)
      {
        continue;
      }

      assets.emplace_back(AssetEntry { .label = relativeText, .relativePath = relativePath });
    }
  }

  // Sorting gives the UI combos a stable order independent of directory iteration order.
  std::sort(assets.begin(), assets.end(), [](const AssetEntry& a, const AssetEntry& b) { return a.label < b.label; });

  return assets;
}

// Returns the index of the first preferred path that exists, trying them in priority order, or 0 when none is present.
size_t FindAssetIndex(const std::vector<AssetEntry>& assets, const std::span<const char* const> preferredPaths)
{
  for(const char* preferred : preferredPaths)
  {
    const std::string preferredLower = ToLowerAscii(preferred);

    for(size_t i = 0; i < assets.size(); ++i)
    {
      if(ToLowerAscii(assets[i].relativePath.generic_string()) == preferredLower)
      {
        return i;
      }
    }
  }

  return 0;
}

bool AssetExistsInList(const std::vector<AssetEntry>& assets, const std::filesystem::path& candidatePath)
{
  const std::string candidateLower = ToLowerAscii(candidatePath.generic_string());

  for(const AssetEntry& asset : assets)
  {
    if(ToLowerAscii(asset.relativePath.generic_string()) == candidateLower)
    {
      return true;
    }
  }

  return false;
}

}  // namespace

SceneAssetCatalogData DiscoverSceneAssets()
{
  SceneAssetCatalogData out {};

  // Discovery
  // Models and HDRIs come from fixed subdirectories of each content directory; scene presets are built in code.

  static constexpr std::array<const char*, 2> kModelExts = { ".gltf", ".glb" };
  static constexpr std::array<const char*, 2> kHdriExts  = { ".hdr", ".exr" };

  out.modelAssets      = DiscoverAssetsInDir("Models", kModelExts);
  out.hdriAssets       = DiscoverAssetsInDir("HDRI", kHdriExts);
  out.sceneDefinitions = rtpt::CreateSceneCatalog();

  // Validation
  // Scene references are checked against discovered model files so missing assets are reported once at startup rather than failing when the scene is selected.

  for(const SceneDefinition& sceneDef : out.sceneDefinitions)
  {
    for(const SceneModelEntry& modelEntry : sceneDef.models)
    {
      if(!AssetExistsInList(out.modelAssets, modelEntry.assetPath))
      {
        out.warnings.push_back("Scene '" + sceneDef.label + "' references missing asset '" + modelEntry.assetPath.generic_string() + "'");
      }
    }
  }

  // Initial selection
  // Both indices are guaranteed to be in range for non-empty lists.

  if(!out.sceneDefinitions.empty())
  {
    out.selectedSceneIndex = rtpt::FindDefaultSceneIndex(out.sceneDefinitions);

    if(out.selectedSceneIndex >= out.sceneDefinitions.size())
    {
      out.selectedSceneIndex = 0;
    }
  }

  if(!out.hdriAssets.empty())
  {
    // Preferred environments in priority order; the first one present is selected.
    static constexpr std::array<const char*, 4> kPreferredHdri = { "HDRI/SymmetricalGarden.hdr", "HDRI/SymmetricalGarden.exr", "HDRI/Black.hdr", "HDRI/Meadow.hdr" };

    out.selectedHdriIndex = FindAssetIndex(out.hdriAssets, kPreferredHdri);
  }

  return out;
}

}  // namespace rtpt
