/*
 * Copyright (c) 2023-2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

// Main.cpp role:
// - Keep startup and shutdown flow minimal.
// - Delegate all rendering/runtime logic to Source/Application.cpp.

// Enable the use of Nsight Aftermath for crash tracking and shader debugging
// #define USE_NSIGHT_AFTERMATH

#include <memory>

#include "Application.h"

#include <nvaftermath/aftermath.hpp>
#include <nvapp/application.hpp>
#include <nvapp/elem_camera.hpp>
#include <nvapp/elem_default_menu.hpp>
#include <nvapp/elem_default_title.hpp>
#include <nvutils/logger.hpp>
#include <nvutils/parameter_parser.hpp>
#include <nvutils/file_operations.hpp>
#include <nvvk/context.hpp>
#include <nvvk/validation_settings.hpp>

int main(int argc, char** argv)
{
  nvapp::ApplicationCreateInfo appInfo = {};

  // Parse CLI switches shared by all app elements.
  nvutils::ParameterParser   cli(nvutils::getExecutablePath().stem().string());
  nvutils::ParameterRegistry reg;
  reg.add({"headless", "Run in headless mode"}, &appInfo.headless, true);
  cli.add(reg);
  cli.parse(argc, argv);

  VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjectFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
  };
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
  };
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
  };
  nvvk::ContextInitInfo vkSetup = {
      .instanceExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME},
      .deviceExtensions =
          {
              {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME},
              {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, &shaderObjectFeatures},
              // Request the core ray tracing pieces up front so the device is
              // born with the capabilities our future path tracing module needs.
              {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, &accelerationStructureFeatures},
              {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, &rayTracingPipelineFeatures},
              {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME},
          },
  };

  if(!appInfo.headless)
  {
    nvvk::addSurfaceExtensions(vkSetup.instanceExtensions, &vkSetup.deviceExtensions);
  }

  // Let the installed validation layer use its own defaults.
  // The nvpro validation-settings helper currently advertises several GPU-AV
  // keys that newer validation layer builds no longer recognize, which creates
  // noisy startup warnings without improving signal for this sample.
  vkSetup.instanceCreateInfoExt = nullptr;

#if defined(USE_NSIGHT_AFTERMATH)
  auto& aftermath = AftermathCrashTracker::getInstance();
  aftermath.initialize();
  aftermath.addExtensions(vkSetup.deviceExtensions);
  nvvk::CheckError::getInstance().setCallbackFunction([&](VkResult result) { aftermath.errorCallback(result); });
#endif

  nvvk::Context vkContext;
  if(vkContext.init(vkSetup) != VK_SUCCESS)
  {
    LOGE("Error in Vulkan context creation\n");
    return 1;
  }

  appInfo.name           = "RealTimePathTracing";
  appInfo.instance       = vkContext.getInstance();
  appInfo.device         = vkContext.getDevice();
  appInfo.physicalDevice = vkContext.getPhysicalDevice();
  appInfo.queues         = vkContext.getQueueInfos();

  nvapp::Application application;
  application.init(appInfo);

  // Build app elements in one place so Main stays orchestration-only.
  auto cameraManip = std::make_shared<nvutils::CameraManipulator>();
  auto foundation  = nvsamples::CreateApplicationElement(cameraManip);
  auto elemCamera  = std::make_shared<nvapp::ElementCamera>();
  auto windowTitle = std::make_shared<nvapp::ElementDefaultWindowTitle>();
  auto windowMenu  = std::make_shared<nvapp::ElementDefaultMenu>();

  elemCamera->setCameraManipulator(cameraManip);

  application.addElement(windowMenu);
  application.addElement(windowTitle);
  application.addElement(elemCamera);
  application.addElement(foundation);

  application.run();

  application.deinit();
  vkContext.deinit();

  return 0;
}



