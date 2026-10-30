#pragma once

#include "predict/imepack/package_types.h"

#include <filesystem>

#include <tl/expected.hpp>

namespace zhixu::predict::imepack {

[[nodiscard]] tl::expected<void, PackageError> BuildPackageIndex(
    const std::filesystem::path& package_root);

}  // namespace zhixu::predict::imepack
