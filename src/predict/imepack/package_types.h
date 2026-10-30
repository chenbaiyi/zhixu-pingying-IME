#pragma once

#include <filesystem>
#include <string>

namespace zhixu::predict::imepack {

enum class PackageError {
  kIo,
  kInvalidArchive,
  kInvalidManifest,
  kUnsupportedSchema,
  kUnsafePath,
  kInvalidUtf8,
  kDuplicateId,
  kIntegrity,
  kLimitExceeded,
  kAlreadyInstalled,
};

struct ValidatedPackage {
  std::filesystem::path archive_path;
  std::string package_id;
  std::string version;
  std::string entries_sha256;
};

struct InstalledPackage {
  std::filesystem::path package_path;
  std::string package_id;
  std::string version;
};

}  // namespace zhixu::predict::imepack
