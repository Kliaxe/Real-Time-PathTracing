#pragma once

// CameraUi
// Camera section of the Settings window: input mode, fly speed, and direct edits of the camera state.
// Lives next to CameraController rather than in Application. Camera edits never report history invalidation because the renderers detect camera changes themselves.

#include "Camera/CameraController.h"

namespace rtpt
{

void DrawCameraSection(CameraController& camera);

}  // namespace rtpt
