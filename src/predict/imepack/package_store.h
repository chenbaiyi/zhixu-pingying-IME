#pragma once

#include "predict/imepack/package_types.h"

#include <filesystem>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

namespace zhixu::predict::imepack {

class PackageStore final {
 public:
  explicit PackageStore(std::filesystem::path root);

  [[nodiscard]] tl::expected<InstalledPackage, PackageError> InstallPackage(
      const ValidatedPackage& package);
  [[nodiscard]] tl::expected<void, PackageError> ActivateVersion(
      std::string_view package_id, std::string_view version);
  [[nodiscard]] tl::expected<void, PackageError> Rollback(
      std::string_view package_id);
  [[nodiscard]] tl::expected<void, PackageError> Disable(
      std::string_view package_id);
  [[nodiscard]] tl::expected<void, PackageError> Uninstall(
      std::string_view package_id, std::string_view version);
  [[nodiscard]] tl::expected<std::vector<std::filesystem::path>, PackageError>
  ActivePackageRoots() const;

 private:
  std::filesystem::path root_;
};

}  // namespace zhixu::predict::imepack
