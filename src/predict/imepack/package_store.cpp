#include "predict/imepack/package_store.h"

#include "predict/imepack/index_builder.h"
#include "predict/imepack/package_validator.h"

#include <windows.h>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace zhixu::predict::imepack {
namespace {

std::atomic_uint64_t g_request_sequence = 0;

struct ActiveVersion final {
  std::string version;
  std::optional<std::string> previous_version;
};

bool IsSafeStoreComponent(const std::string_view value) {
  if (value.empty() || value == "." || value == ".." ||
      value.find('/') != std::string_view::npos ||
      value.find('\\') != std::string_view::npos ||
      value.find(':') != std::string_view::npos ||
      value.find("..") != std::string_view::npos) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
    return character >= 0x21 && character <= 0x7e;
  });
}

std::filesystem::path PathFromUtf8(const std::string_view value) {
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const unsigned char character : value) utf8.push_back(static_cast<char8_t>(character));
  return std::filesystem::path(utf8);
}

bool IsNormalizedRelativePath(const char* path) {
  if (path == nullptr || *path == '\0' || path[0] == '/' || path[0] == '\\') return false;
  std::string_view remaining(path);
  while (!remaining.empty()) {
    const auto separator = remaining.find('/');
    const auto segment = remaining.substr(0, separator);
    if (segment.empty() || segment == "." || segment == ".." ||
        segment.find('\\') != std::string_view::npos ||
        segment.find(':') != std::string_view::npos) return false;
    if (separator == std::string_view::npos) break;
    remaining.remove_prefix(separator + 1);
  }
  return true;
}

class Reader final {
 public:
  Reader() : handle_(mz_zip_reader_create()) {}
  ~Reader() {
    if (handle_ != nullptr) {
      mz_zip_reader_close(handle_);
      mz_zip_reader_delete(&handle_);
    }
  }
  [[nodiscard]] void* get() const noexcept { return handle_; }

 private:
  void* handle_ = nullptr;
};

class StagingDirectory final {
 public:
  explicit StagingDirectory(std::filesystem::path path) : path_(std::move(path)) {}
  ~StagingDirectory() {
    if (!committed_) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  void Commit() noexcept { committed_ = true; }

 private:
  std::filesystem::path path_;
  bool committed_ = false;
};

bool WriteAll(HANDLE file, const char* bytes, size_t size) {
  while (size != 0) {
    const auto chunk = static_cast<DWORD>(std::min<size_t>(size, MAXDWORD));
    DWORD written = 0;
    if (!WriteFile(file, bytes, chunk, &written, nullptr) || written == 0) return false;
    bytes += written;
    size -= written;
  }
  return true;
}

tl::expected<void, PackageError> ExtractSnapshot(
    const std::filesystem::path& archive_path, const std::filesystem::path& staging_root) {
  Reader reader;
  if (reader.get() == nullptr ||
      mz_zip_reader_open_file(reader.get(), reinterpret_cast<const char*>(archive_path.u8string().c_str())) != MZ_OK) {
    return tl::unexpected(PackageError::kInvalidArchive);
  }

  std::array<char, 64 * 1024> buffer{};
  for (int result = mz_zip_reader_goto_first_entry(reader.get()); result == MZ_OK;
       result = mz_zip_reader_goto_next_entry(reader.get())) {
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(reader.get(), &info) != MZ_OK || info == nullptr ||
        !IsNormalizedRelativePath(info->filename) ||
        (info->linkname != nullptr && *info->linkname != '\0')) {
      return tl::unexpected(PackageError::kUnsafePath);
    }

    const auto destination = staging_root / PathFromUtf8(info->filename);
    std::error_code error;
    if (mz_zip_reader_entry_is_dir(reader.get()) == MZ_OK) {
      std::filesystem::create_directories(destination, error);
      if (error) return tl::unexpected(PackageError::kIo);
      continue;
    }
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return tl::unexpected(PackageError::kIo);

    const auto file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return tl::unexpected(PackageError::kIo);
    const auto close_file = [&file]() { CloseHandle(file); };
    if (mz_zip_reader_entry_open(reader.get()) != MZ_OK) {
      close_file();
      return tl::unexpected(PackageError::kInvalidArchive);
    }

    for (;;) {
      const auto read = mz_zip_reader_entry_read(reader.get(), buffer.data(),
                                                  static_cast<int32_t>(buffer.size()));
      if (read < 0) {
        mz_zip_reader_entry_close(reader.get());
        close_file();
        return tl::unexpected(PackageError::kInvalidArchive);
      }
      if (read == 0) break;
      if (!WriteAll(file, buffer.data(), static_cast<size_t>(read))) {
        mz_zip_reader_entry_close(reader.get());
        close_file();
        return tl::unexpected(PackageError::kIo);
      }
    }
    const auto entry_closed = mz_zip_reader_entry_close(reader.get()) == MZ_OK;
    const auto flushed = FlushFileBuffers(file) != FALSE;
    close_file();
    if (!entry_closed) return tl::unexpected(PackageError::kInvalidArchive);
    if (!flushed) return tl::unexpected(PackageError::kIo);
  }
  return {};
}

tl::expected<std::optional<ActiveVersion>, PackageError> ReadActiveVersion(
    const std::filesystem::path& package_root) {
  const auto active_path = package_root / L"active.json";
  std::error_code error;
  const bool exists = std::filesystem::exists(active_path, error);
  if (error) return tl::unexpected(PackageError::kIo);
  if (!exists) return std::optional<ActiveVersion>{};
  if (std::filesystem::file_size(active_path, error) > 4096 || error) {
    return tl::unexpected(PackageError::kIo);
  }

  std::ifstream input(active_path, std::ios::binary);
  if (!input) return tl::unexpected(PackageError::kIo);
  const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  const auto json = nlohmann::json::parse(contents, nullptr, false, false);
  if (json.is_discarded() || !json.is_object() || !json.contains("version") ||
      !json["version"].is_string()) {
    return tl::unexpected(PackageError::kIntegrity);
  }
  ActiveVersion active{.version = json["version"].get<std::string>()};
  if (!IsSafeStoreComponent(active.version)) return tl::unexpected(PackageError::kIntegrity);
  if (json.contains("previous_version")) {
    if (!json["previous_version"].is_string()) return tl::unexpected(PackageError::kIntegrity);
    active.previous_version = json["previous_version"].get<std::string>();
    if (!IsSafeStoreComponent(*active.previous_version)) {
      return tl::unexpected(PackageError::kIntegrity);
    }
  }
  return std::optional<ActiveVersion>{std::move(active)};
}

tl::expected<void, PackageError> WriteActiveVersion(
    const std::filesystem::path& package_root, const std::string_view version,
    const std::optional<std::string>& previous_version) {
  const auto active_path = package_root / L"active.json";
  const auto pending_path = package_root /
      (L"active." + std::to_wstring(GetCurrentProcessId()) + L"." +
       std::to_wstring(g_request_sequence.fetch_add(1)) + L".tmp");
  nlohmann::json json{{"version", version}};
  if (previous_version) json["previous_version"] = *previous_version;
  const auto contents = json.dump() + "\n";

  const auto file = CreateFileW(pending_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return tl::unexpected(PackageError::kIo);
  const auto wrote = WriteAll(file, contents.data(), contents.size());
  const auto flushed = FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  if (!wrote || !flushed) {
    DeleteFileW(pending_path.c_str());
    return tl::unexpected(PackageError::kIo);
  }

  if (std::filesystem::exists(active_path)) {
    if (!ReplaceFileW(active_path.c_str(), pending_path.c_str(), nullptr,
                      REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
      DeleteFileW(pending_path.c_str());
      return tl::unexpected(PackageError::kIo);
    }
  } else if (!MoveFileExW(pending_path.c_str(), active_path.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(pending_path.c_str());
    return tl::unexpected(PackageError::kIo);
  }
  return {};
}

}  // namespace

PackageStore::PackageStore(std::filesystem::path root) : root_(std::move(root)) {}

tl::expected<InstalledPackage, PackageError> PackageStore::InstallPackage(
    const ValidatedPackage& package) {
  std::error_code error;
  std::filesystem::create_directories(root_ / L".staging", error);
  if (error) return tl::unexpected(PackageError::kIo);

  const auto request_id = std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"-" +
      std::to_wstring(g_request_sequence.fetch_add(1));
  StagingDirectory staging(root_ / L".staging" / request_id);
  std::filesystem::create_directories(staging.path(), error);
  if (error) return tl::unexpected(PackageError::kIo);

  const auto snapshot_path = staging.path() / L".package.imepack";
  std::filesystem::copy_file(package.archive_path, snapshot_path,
                             std::filesystem::copy_options::none, error);
  if (error) return tl::unexpected(PackageError::kIo);

  const auto snapshot = ValidatePackage(snapshot_path);
  if (!snapshot) return tl::unexpected(snapshot.error());
  if (snapshot->package_id != package.package_id || snapshot->version != package.version ||
      (!package.entries_sha256.empty() && snapshot->entries_sha256 != package.entries_sha256)) {
    return tl::unexpected(PackageError::kIntegrity);
  }

  const auto package_root = root_ / std::filesystem::path(snapshot->package_id);
  const auto destination = package_root / std::filesystem::path(snapshot->version);
  if (std::filesystem::exists(destination, error) && !error) {
    std::ifstream manifest_input(destination/L"manifest.json",std::ios::binary);
    const auto manifest=nlohmann::json::parse(manifest_input,nullptr,false);
    if(manifest.is_discarded() || !manifest.contains("entries") || !manifest["entries"].is_object() ||
       manifest["entries"].value("sha256",std::string{})!=snapshot->entries_sha256)return tl::unexpected(PackageError::kIntegrity);
    return tl::unexpected(PackageError::kAlreadyInstalled);
  }
  if (error) return tl::unexpected(PackageError::kIo);
  const auto extraction = ExtractSnapshot(snapshot_path, staging.path());
  if (!extraction) return tl::unexpected(extraction.error());
  const auto indexed = BuildPackageIndex(staging.path());
  if (!indexed) return tl::unexpected(indexed.error());

  std::filesystem::create_directories(package_root, error);
  if (error) return tl::unexpected(PackageError::kIo);
  const auto active_before = ReadActiveVersion(package_root);
  if (!active_before) return tl::unexpected(active_before.error());
  std::optional<std::string> previous_version;
  if (*active_before && (*active_before)->version != snapshot->version) {
    previous_version = (*active_before)->version;
  }
  std::filesystem::rename(staging.path(), destination, error);
  if (error) return tl::unexpected(PackageError::kIo);
  staging.Commit();

  const auto active = WriteActiveVersion(package_root, snapshot->version, previous_version);
  if (!active) return tl::unexpected(active.error());
  return InstalledPackage{
      .package_path = destination,
      .package_id = snapshot->package_id,
      .version = snapshot->version,
  };
}

tl::expected<void, PackageError> PackageStore::ActivateVersion(
    const std::string_view package_id, const std::string_view version) {
  if (!IsSafeStoreComponent(package_id) || !IsSafeStoreComponent(version)) {
    return tl::unexpected(PackageError::kUnsafePath);
  }
  const auto package_root = root_ / std::filesystem::path(std::string(package_id));
  const auto destination = package_root / std::filesystem::path(std::string(version));
  std::error_code error;
  if (!std::filesystem::is_directory(destination, error) || error) {
    return tl::unexpected(PackageError::kIo);
  }
  const auto active = ReadActiveVersion(package_root);
  if (!active) return tl::unexpected(active.error());
  std::optional<std::string> previous_version;
  if (*active && (*active)->version != version) previous_version = (*active)->version;
  return WriteActiveVersion(package_root, version, previous_version);
}

tl::expected<void, PackageError> PackageStore::Rollback(const std::string_view package_id) {
  if (!IsSafeStoreComponent(package_id)) return tl::unexpected(PackageError::kUnsafePath);
  const auto package_root = root_ / std::filesystem::path(std::string(package_id));
  const auto active = ReadActiveVersion(package_root);
  if (!active) return tl::unexpected(active.error());
  if (!*active || !(*active)->previous_version) return tl::unexpected(PackageError::kIo);
  return ActivateVersion(package_id, *(*active)->previous_version);
}

tl::expected<void, PackageError> PackageStore::Disable(const std::string_view package_id) {
  if (!IsSafeStoreComponent(package_id)) return tl::unexpected(PackageError::kUnsafePath);
  const auto active_path = root_ / std::filesystem::path(std::string(package_id)) / L"active.json";
  if (DeleteFileW(active_path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return {};
  return tl::unexpected(PackageError::kIo);
}

tl::expected<std::vector<std::filesystem::path>, PackageError> PackageStore::ActivePackageRoots() const {
  std::error_code error;
  if (!std::filesystem::exists(root_, error)) {
    if (error) return tl::unexpected(PackageError::kIo);
    return std::vector<std::filesystem::path>{};
  }
  if (!std::filesystem::is_directory(root_, error) || error) {
    return tl::unexpected(PackageError::kIo);
  }

  std::vector<std::filesystem::path> roots;
  for (std::filesystem::directory_iterator entry(root_, error), end;
       !error && entry != end; entry.increment(error)) {
    const auto status = entry->symlink_status(error);
    if (error) break;
    if (std::filesystem::is_symlink(status)) return tl::unexpected(PackageError::kIntegrity);
    if (!std::filesystem::is_directory(status)) continue;

    const auto package_id = entry->path().filename().string();
    if (!IsSafeStoreComponent(package_id)) return tl::unexpected(PackageError::kIntegrity);
    const auto active = ReadActiveVersion(entry->path());
    if (!active) return tl::unexpected(active.error());
    if (!*active) continue;

    const auto version_root = entry->path() / PathFromUtf8((*active)->version);
    const auto version_status = std::filesystem::symlink_status(version_root, error);
    if (error || std::filesystem::is_symlink(version_status) ||
        !std::filesystem::is_directory(version_status)) {
      return tl::unexpected(PackageError::kIntegrity);
    }
    roots.push_back(version_root);
  }
  if (error) return tl::unexpected(PackageError::kIo);
  std::sort(roots.begin(), roots.end());
  return roots;
}

tl::expected<void, PackageError> PackageStore::Uninstall(
    const std::string_view package_id, const std::string_view version) {
  if (!IsSafeStoreComponent(package_id) || !IsSafeStoreComponent(version)) {
    return tl::unexpected(PackageError::kUnsafePath);
  }
  const auto package_root = root_ / std::filesystem::path(std::string(package_id));
  const auto active = ReadActiveVersion(package_root);
  if (!active) return tl::unexpected(active.error());
  if (*active && (*active)->version == version) return tl::unexpected(PackageError::kIo);

  const auto destination = package_root / std::filesystem::path(std::string(version));
  std::error_code error;
  if (!std::filesystem::is_directory(destination, error) || error) {
    return tl::unexpected(PackageError::kIo);
  }
  std::filesystem::remove_all(destination, error);
  if (error) return tl::unexpected(PackageError::kIo);
  return {};
}

}  // namespace zhixu::predict::imepack
