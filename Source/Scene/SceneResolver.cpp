#include "SceneResolver.h"

// Role:
// Clamp and resolve user selection indices to stable catalog entries.

namespace nvsamples
{

SceneResolver::Output SceneResolver::Resolve(const Input& input) const
{
  Output out{};

  if(input.sceneDefinitions.empty())
  {
    return out;
  }

  out.resolvedSceneIndex = input.selectedSceneIndex;
  if(out.resolvedSceneIndex >= input.sceneDefinitions.size())
  {
    out.resolvedSceneIndex = 0;
  }
  out.sceneDefinition = &input.sceneDefinitions[out.resolvedSceneIndex];

  if(!input.hdriAssets.empty())
  {
    out.resolvedHdriIndex = input.selectedHdriIndex;
    if(out.resolvedHdriIndex >= input.hdriAssets.size())
    {
      out.resolvedHdriIndex = 0;
    }
    out.hdriRelativePath = input.hdriAssets[out.resolvedHdriIndex].relativePath;
  }

  return out;
}

}  // namespace nvsamples
