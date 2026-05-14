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
#include <optional>
#include <filesystem>
#include <string>
#include <string_view>

#include "Application.h"
#include "Experiments/ExperimentController.h"
#include "Experiments/ExperimentPlan.h"

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
  std::string           experimentName;
  std::filesystem::path experimentOutput;
  bool                  listExperiments = false;
  reg.add({"headless", "Run in headless mode"}, &appInfo.headless, true);
  reg.add({"experiment", "Run a named thesis experiment plan"}, &experimentName);
  reg.add({"experiment-output", "Output folder for experiment captures and metadata"}, &experimentOutput);
  reg.add({"list-experiments", "Print available thesis experiment plans"}, &listExperiments, true);
  cli.add(reg);
  cli.parse(argc, argv);

  if(listExperiments)
  {
    LOGI("Available experiments:\n");
    for(std::string_view name : nvsamples::GetAvailableExperimentPlanNames())
    {
      LOGI("  %.*s\n", static_cast<int>(name.size()), name.data());
    }
    return 0;
  }

  std::shared_ptr<nvsamples::ExperimentController> experimentController;
  if(!experimentName.empty())
  {
    appInfo.headless = true;
    if(experimentOutput.empty())
    {
      experimentOutput = std::filesystem::path("Results") / experimentName;
    }

    std::optional<nvsamples::ExperimentPlan> experimentPlan =
        nvsamples::CreateNamedExperimentPlan(experimentName, experimentOutput);
    if(!experimentPlan)
    {
      LOGE("Unknown experiment '%s'\n", experimentName.c_str());
      LOGE("Available experiments:\n");
      for(std::string_view name : nvsamples::GetAvailableExperimentPlanNames())
      {
        LOGE("  %.*s\n", static_cast<int>(name.size()), name.data());
      }
      return 1;
    }

    experimentController      = std::make_shared<nvsamples::ExperimentController>(std::move(*experimentPlan));
    appInfo.headlessFrameCount = experimentController->GetRequiredHeadlessFrameCount();
    appInfo.windowSize         = {1280, 720};
  }

  VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjectFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
  };
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
  };
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
  };
  VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
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
              // ReSTIR PT's compute resampling passes use inline visibility tests
              // during candidate shifting, which requires the ray query feature.
              {VK_KHR_RAY_QUERY_EXTENSION_NAME, &rayQueryFeatures},
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
  auto renderer    = std::make_shared<nvsamples::Application>(cameraManip);
  auto elemCamera  = std::make_shared<nvapp::ElementCamera>();
  auto windowTitle = std::make_shared<nvapp::ElementDefaultWindowTitle>();
  auto windowMenu  = std::make_shared<nvapp::ElementDefaultMenu>();

  elemCamera->setCameraManipulator(cameraManip);
  renderer->SetExperimentController(experimentController);

  application.addElement(windowMenu);
  application.addElement(windowTitle);
  application.addElement(elemCamera);
  application.addElement(renderer);

  application.run();

  application.deinit();
  vkContext.deinit();

  return 0;
}




