#include "SceneResolver.h"

namespace rtpt
{

SceneSelectionOutput ResolveSceneSelection(const SceneSelectionInput& input)
{
  SceneSelectionOutput out {};

  // With no scenes there is nothing to resolve; the null sceneDefinition tells the caller.
  if(input.sceneDefinitions.empty())
  {
    return out;
  }

  // Scene
  // An out-of-range index falls back to the first entry rather than failing, so a stale UI index still yields a valid scene.

  out.resolvedSceneIndex = input.selectedSceneIndex;

  if(out.resolvedSceneIndex >= input.sceneDefinitions.size())
  {
    out.resolvedSceneIndex = 0;
  }

  out.sceneDefinition = &input.sceneDefinitions[out.resolvedSceneIndex];

  // HDRI
  // Same clamping as the scene. Without any HDRI assets the path stays empty and the scene loads without an environment map.

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

}  // namespace rtpt
