#include "predict/imepack/index_builder.h"
#include "predict/imepack/disk_index.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace zhixu::predict::imepack {
namespace {

constexpr std::uintmax_t kMaximumManifestBytes = 1024 * 1024;
constexpr size_t kMaximumJsonLineBytes = 1024 * 1024;

bool IsNormalizedRelativePath(const std::string_view path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\') return false;
  std::string_view remaining = path;
  while (!remaining.empty()) {
    const auto separator = remaining.find('/');
    const auto segment = remaining.substr(0, separator);
    if (segment.empty() || segment == "." || segment == ".." ||
        segment.find('\\') != std::string_view::npos ||
        segment.find(':') != std::string_view::npos) {
      return false;
    }
    if (separator == std::string_view::npos) break;
    remaining.remove_prefix(separator + 1);
  }
  return true;
}

std::filesystem::path PathFromUtf8(const std::string_view value) {
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const unsigned char character : value) utf8.push_back(static_cast<char8_t>(character));
  return std::filesystem::path(utf8);
}

tl::expected<std::string, PackageError> NormalizeNfc(const std::string_view value) {
  if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return tl::unexpected(PackageError::kLimitExceeded);
  }
  const auto wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                               static_cast<int>(value.size()), nullptr, 0);
  if (wide_length == 0 && !value.empty()) return tl::unexpected(PackageError::kInvalidUtf8);
  std::wstring wide(static_cast<size_t>(wide_length), L'\0');
  if (wide_length != 0 &&
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                          wide.data(), wide_length) == 0) {
    return tl::unexpected(PackageError::kInvalidUtf8);
  }
  const auto normalized_length = NormalizeString(NormalizationC, wide.data(), wide_length, nullptr, 0);
  if (normalized_length <= 0 && !wide.empty()) return tl::unexpected(PackageError::kInvalidUtf8);
  std::wstring normalized(static_cast<size_t>(normalized_length), L'\0');
  if (normalized_length != 0 &&
      NormalizeString(NormalizationC, wide.data(), wide_length, normalized.data(), normalized_length) <= 0) {
    return tl::unexpected(PackageError::kInvalidUtf8);
  }
  const auto utf8_length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, normalized.data(),
                                                normalized_length, nullptr, 0, nullptr, nullptr);
  if (utf8_length == 0 && !normalized.empty()) return tl::unexpected(PackageError::kInvalidUtf8);
  std::string utf8(static_cast<size_t>(utf8_length), '\0');
  if (utf8_length != 0 &&
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, normalized.data(), normalized_length,
                          utf8.data(), utf8_length, nullptr, nullptr) == 0) {
    return tl::unexpected(PackageError::kInvalidUtf8);
  }
  return utf8;
}

bool WriteAll(const HANDLE file, const char* bytes, size_t size) {
  while (size != 0) {
    const auto chunk = static_cast<DWORD>(std::min<size_t>(size, MAXDWORD));
    DWORD written = 0;
    if (!WriteFile(file, bytes, chunk, &written, nullptr) || written == 0) return false;
    bytes += written;
    size -= written;
  }
  return true;
}

class PendingIndex final {
 public:
  PendingIndex(std::filesystem::path pending, std::filesystem::path destination)
      : pending_(std::move(pending)), destination_(std::move(destination)) {}
  ~PendingIndex() {
    Close();
    if (!committed_) DeleteFileW(pending_.c_str());
  }

  [[nodiscard]] tl::expected<void, PackageError> Open() {
    file_ = CreateFileW(pending_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return tl::unexpected(PackageError::kIo);
    return {};
  }

  [[nodiscard]] tl::expected<void, PackageError> Write(const std::string_view line) {
    if (file_ == INVALID_HANDLE_VALUE || !WriteAll(file_, line.data(), line.size()) ||
        !WriteAll(file_, "\n", 1)) {
      return tl::unexpected(PackageError::kIo);
    }
    return {};
  }

  [[nodiscard]] tl::expected<void, PackageError> Commit() {
    if (file_ == INVALID_HANDLE_VALUE || !FlushFileBuffers(file_)) {
      Close();
      return tl::unexpected(PackageError::kIo);
    }
    Close();
    if (!MoveFileExW(pending_.c_str(), destination_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return tl::unexpected(PackageError::kIo);
    }
    committed_ = true;
    return {};
  }

 private:
  void Close() {
    if (file_ != INVALID_HANDLE_VALUE) {
      CloseHandle(file_);
      file_ = INVALID_HANDLE_VALUE;
    }
  }

  std::filesystem::path pending_;
  std::filesystem::path destination_;
  HANDLE file_ = INVALID_HANDLE_VALUE;
  bool committed_ = false;
};

bool HasStringArray(const nlohmann::json& entry, const char* name, const bool require_items) {
  const auto values = entry.find(name);
  if (values == entry.end() || !values->is_array() || (require_items && values->empty())) return false;
  for (const auto& value : *values) {
    if (!value.is_string() || value.get<std::string>().empty()) return false;
  }
  return true;
}

}  // namespace

tl::expected<void, PackageError> BuildPackageIndex(const std::filesystem::path& package_root) {
  const auto manifest_path = package_root / L"manifest.json";
  std::error_code error;
  if (std::filesystem::file_size(manifest_path, error) > kMaximumManifestBytes || error) {
    return tl::unexpected(PackageError::kIo);
  }
  std::ifstream manifest_input(manifest_path, std::ios::binary);
  if (!manifest_input) return tl::unexpected(PackageError::kIo);
  const std::string manifest_contents((std::istreambuf_iterator<char>(manifest_input)),
                                      std::istreambuf_iterator<char>());
  const auto manifest = nlohmann::json::parse(manifest_contents, nullptr, false, false);
  if (manifest.is_discarded() || !manifest.is_object() || !manifest.contains("entries") ||
      !manifest["entries"].is_object() || !manifest["entries"].contains("path") ||
      !manifest["entries"]["path"].is_string()) {
    return tl::unexpected(PackageError::kIntegrity);
  }
  const auto entries_path_text = manifest["entries"]["path"].get<std::string>();
  if (!IsNormalizedRelativePath(entries_path_text)) return tl::unexpected(PackageError::kIntegrity);

  std::ifstream entries_input(package_root / PathFromUtf8(entries_path_text), std::ios::binary);
  if (!entries_input) return tl::unexpected(PackageError::kIo);
  const auto index_directory = package_root / L"indexes";
  std::filesystem::create_directories(index_directory, error);
  if (error) return tl::unexpected(PackageError::kIo);
  PendingIndex continuation_index(index_directory / L"continuation.pending.jsonl",
                                  index_directory / L"continuation.jsonl");
  PendingIndex quotation_index(index_directory / L"quotation.pending.jsonl",
                               index_directory / L"quotation.jsonl");
  const auto continuation_opened = continuation_index.Open();
  if (!continuation_opened) return tl::unexpected(continuation_opened.error());
  const auto quotation_opened = quotation_index.Open();
  if (!quotation_opened) return tl::unexpected(quotation_opened.error());

  std::string line;
  while (std::getline(entries_input, line)) {
    if (line.size() > kMaximumJsonLineBytes) return tl::unexpected(PackageError::kLimitExceeded);
    const auto entry = nlohmann::json::parse(line, nullptr, false, false);
    if (entry.is_discarded() || !entry.is_object() || !entry.contains("type") ||
        !entry["type"].is_string()) {
      return tl::unexpected(PackageError::kIntegrity);
    }
    const auto type = entry["type"].get<std::string>();
    if (type == "continuation") {
      if (!entry.contains("id") || !entry["id"].is_string() || !entry.contains("trigger") ||
          !entry["trigger"].is_string() || !entry.contains("completions") ||
          !entry["completions"].is_array() || !HasStringArray(entry, "tags", false)) {
        return tl::unexpected(PackageError::kIntegrity);
      }
      const auto trigger = entry["trigger"].get<std::string>();
      const auto normalized_trigger = NormalizeNfc(trigger);
      if (!normalized_trigger) return tl::unexpected(normalized_trigger.error());
      for (const auto& completion : entry["completions"]) {
        if (!completion.is_object() || !completion.contains("text") || !completion["text"].is_string() ||
            !completion.contains("weight") || !completion["weight"].is_number()) {
          return tl::unexpected(PackageError::kIntegrity);
        }
        const nlohmann::json indexed{{"id", entry["id"]},
                                     {"trigger", trigger},
                                     {"normalized_trigger", *normalized_trigger},
                                     {"text", completion["text"]},
                                     {"weight", completion["weight"]},
                                     {"tags", entry["tags"]}};
        const auto written = continuation_index.Write(indexed.dump());
        if (!written) return tl::unexpected(written.error());
      }
      continue;
    }
    if (type != "quotation") continue;
    if (!entry.contains("id") || !entry["id"].is_string() || !entry.contains("work") ||
        !entry["work"].is_string() || !entry.contains("author") || !entry["author"].is_string() ||
        !HasStringArray(entry, "segments", true) || !HasStringArray(entry, "tags", false)) {
      return tl::unexpected(PackageError::kIntegrity);
    }
    const auto& segments = entry["segments"];
    for (size_t position = 0; position + 1 < segments.size(); ++position) {
      const nlohmann::json indexed{{"id", entry["id"]},
                                   {"match", segments[position]},
                                   {"text", segments[position + 1]},
                                   {"weight", 1.0},
                                   {"work", entry["work"]},
                                   {"author", entry["author"]},
                                   {"source_context", "segment:" + std::to_string(position + 1)},
                                   {"tags", entry["tags"]}};
      const auto written = quotation_index.Write(indexed.dump());
      if (!written) return tl::unexpected(written.error());
    }
  }
  if (!entries_input.eof()) return tl::unexpected(PackageError::kIo);
  const auto continuation_committed = continuation_index.Commit();
  if (!continuation_committed) return tl::unexpected(continuation_committed.error());
  const auto quotation_committed = quotation_index.Commit();
  if (!quotation_committed) return tl::unexpected(quotation_committed.error());
  return BuildDiskIndex(package_root);
}

}  // namespace zhixu::predict::imepack
