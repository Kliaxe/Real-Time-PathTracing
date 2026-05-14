#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Experiments/ExperimentTypes.h"

namespace nvsamples
{

std::optional<ExperimentPlan> CreateNamedExperimentPlan(std::string_view name, const std::filesystem::path& outputRoot);
std::vector<std::string_view> GetAvailableExperimentPlanNames();
uint32_t GetRequiredHeadlessFrameCount(const ExperimentPlan& plan);

}  // namespace nvsamples
