#include "SceneAssetCatalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <unordered_set>

#include "Common/PathUtils.hpp"
#include "SceneCatalog.h"

namespace nvsamples
{

namespace
{

std::string ToLowerAscii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
  std::vector<AssetEntry>      assets;
  std::unordered_set<std::string> seenPaths;

  for(const auto& contentDir : nvsamples::GetContentDirs())
  {
    const std::filesystem::path rootDir = contentDir / subDir;
    std::error_code             ec;
    if(!std::filesystem::exists(rootDir, ec))
    {
      continue;
    }

    for(const auto& entry :
        std::filesystem::recursive_directory_iterator(rootDir, std::filesystem::directory_options::skip_permission_denied))
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

      assets.emplace_back(AssetEntry{.label = relativeText, .relativePath = relativePath});
    }
  }

  std::sort(assets.begin(), assets.end(), [](const AssetEntry& a, const AssetEntry& b) { return a.label < b.label; });
  return assets;
}

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

SceneAssetCatalogData SceneAssetCatalog::Discover() const
{
  SceneAssetCatalogData out{};

  static constexpr std::array<const char*, 2> kModelExts = {".gltf", ".glb"};
  static constexpr std::array<const char*, 2> kHdriExts  = {".hdr", ".exr"};

  out.modelAssets      = DiscoverAssetsInDir("Models", kModelExts);
  out.hdriAssets       = DiscoverAssetsInDir("HDRI", kHdriExts);
  out.sceneDefinitions = nvsamples::CreateSceneCatalog();

  // Validate scene references against discovered model files so missing assets
  // are reported once at startup rather than failing when scene is selected.
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

  if(!out.sceneDefinitions.empty())
  {
    out.selectedSceneIndex = nvsamples::FindDefaultSceneIndex(out.sceneDefinitions);
    if(out.selectedSceneIndex >= out.sceneDefinitions.size())
    {
      out.selectedSceneIndex = 0;
    }
  }

  if(!out.hdriAssets.empty())
  {
    static constexpr std::array<const char*, 4> kPreferredHdri = {"HDRI/SymmetricalGarden.hdr", "HDRI/SymmetricalGarden.exr",
                                                                   "HDRI/Black.hdr", "HDRI/Meadow.hdr"};
    out.selectedHdriIndex = FindAssetIndex(out.hdriAssets, kPreferredHdri);
  }

  return out;
}

}  // namespace nvsamples

