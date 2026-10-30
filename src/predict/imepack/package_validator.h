#pragma once

#include <filesystem>

#include <tl/expected.hpp>

#include "predict/imepack/package_types.h"

namespace zhixu::predict::imepack {

[[nodiscard]] tl::expected<ValidatedPackage, PackageError> ValidatePackage(
    const std::filesystem::path& archive_path);

}  // namespace zhixu::predict::imepack
