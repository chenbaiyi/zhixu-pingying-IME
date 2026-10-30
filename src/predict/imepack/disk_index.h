#pragma once
#include "predict/imepack/package_types.h"
#include "predict/prediction/prediction_types.h"
#include <chrono>
#include <filesystem>
#include <tl/expected.hpp>
namespace zhixu::predict::imepack {
[[nodiscard]] tl::expected<void, PackageError> BuildDiskIndex(const std::filesystem::path& root);
[[nodiscard]] std::vector<Suggestion> RecallDiskIndex(const std::filesystem::path& root,
    const PredictionContext& context, size_t limit, std::chrono::steady_clock::time_point deadline, bool priority_only = false);
}
