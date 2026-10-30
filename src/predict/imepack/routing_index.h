#pragma once
#include "predict/imepack/package_types.h"
#include "predict/prediction/prediction_types.h"
#include <filesystem>
#include <tl/expected.hpp>
namespace zhixu::predict::imepack {
[[nodiscard]] tl::expected<void,PackageError> BuildRoutingIndex(const std::vector<std::filesystem::path>& roots);
[[nodiscard]] bool RoutingIndexMatches(const std::vector<std::filesystem::path>& roots);
[[nodiscard]] std::vector<std::filesystem::path> RoutePackageRoots(const std::vector<std::filesystem::path>& roots,const PredictionContext& context);
}
