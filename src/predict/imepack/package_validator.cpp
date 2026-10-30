#include "predict/imepack/package_validator.h"

#include <windows.h>
#include <bcrypt.h>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <optional>
#include <unordered_set>
#include <vector>

namespace zhixu::predict::imepack {
namespace {

constexpr std::uintmax_t kMaximumCompressedBytes = 2ULL * 1024 * 1024 * 1024;
constexpr std::uintmax_t kMaximumExpandedBytes = 4ULL * 1024 * 1024 * 1024;
constexpr std::uintmax_t kMaximumExpansionRatio = 100;
constexpr size_t kMaximumManifestBytes = 1024 * 1024;
constexpr size_t kMaximumJsonlLineBytes = 1024 * 1024;
constexpr size_t kSha256Bytes = 32;

struct Manifest final {
  std::string package_id;
  std::string version;
  std::string entries_path;
  std::string entries_sha256;
};

bool IsAsciiAlpha(const char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsAsciiAlphaNumeric(const char value) {
  return IsAsciiAlpha(value) || (value >= '0' && value <= '9');
}

bool IsValidUtf8(const std::string_view text) {
  for (size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    if (first <= 0x7f) {
      ++index;
      continue;
    }

    size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation_count = 1;
      code_point = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation_count = 2;
      code_point = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation_count = 3;
      code_point = first & 0x07;
    } else {
      return false;
    }
    if (index + continuation_count >= text.size()) return false;

    for (size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xc0) != 0x80) return false;
      code_point = (code_point << 6) | (continuation & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) || code_point > 0x10ffff) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
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

bool IsReverseDnsPackageId(const std::string_view package_id) {
  size_t label_start = 0;
  size_t label_count = 0;
  while (label_start < package_id.size()) {
    const auto separator = package_id.find('.', label_start);
    const auto label = package_id.substr(label_start, separator - label_start);
    if (label.empty() || !IsAsciiAlpha(label.front()) || label.back() == '-') return false;
    for (const auto character : label) {
      if (!((character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '-')) return false;
    }
    ++label_count;
    if (separator == std::string_view::npos) break;
    label_start = separator + 1;
  }
  return label_count >= 2;
}

bool IsSemanticVersion(const std::string_view version) {
  size_t component_start = 0;
  for (int component = 0; component != 3; ++component) {
    const auto component_end = version.find(component == 2 ? '-' : '.', component_start);
    const auto text = version.substr(component_start, component_end - component_start);
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    for (const auto character : text) {
      if (character < '0' || character > '9') return false;
    }
    if (component == 2) return component_end == std::string_view::npos ||
                               (component_end + 1 < version.size());
    if (component_end == std::string_view::npos) return false;
    component_start = component_end + 1;
  }
  return false;
}

bool IsLocale(const std::string_view locale) {
  if (locale.size() < 2 || !IsAsciiAlpha(locale[0]) || !IsAsciiAlpha(locale[1])) return false;
  for (size_t index = 2; index < locale.size(); ++index) {
    if (!(IsAsciiAlphaNumeric(locale[index]) || locale[index] == '-')) return false;
  }
  return true;
}

bool IsSha256(const std::string_view digest) {
  if (digest.size() != kSha256Bytes * 2) return false;
  for (const auto character : digest) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) return false;
  }
  return true;
}

bool IsGeneratedAt(const std::string_view generated_at) {
  return generated_at.size() == 20 && generated_at[4] == '-' && generated_at[7] == '-' &&
         generated_at[10] == 'T' && generated_at[13] == ':' && generated_at[16] == ':' &&
         generated_at[19] == 'Z';
}

bool HasRequiredString(const nlohmann::json& object, const char* name, std::string* result) {
  const auto found = object.find(name);
  if (found == object.end() || !found->is_string()) return false;
  *result = found->get<std::string>();
  return !result->empty();
}

bool HasStringArray(const nlohmann::json& object, const char* name, const bool require_items) {
  const auto values = object.find(name);
  if (values == object.end() || !values->is_array() || (require_items && values->empty())) return false;
  for (const auto& value : *values) {
    if (!value.is_string() || value.get<std::string>().empty()) return false;
  }
  return true;
}

std::optional<PackageError> ValidateJsonlLine(
    const std::string_view line, std::unordered_set<std::string>* entry_ids) {
  if (!IsValidUtf8(line)) return PackageError::kInvalidUtf8;
  const auto json = nlohmann::json::parse(line, nullptr, false, false);
  if (json.is_discarded() || !json.is_object()) return PackageError::kInvalidManifest;

  std::string entry_id;
  std::string type;
  if (!HasRequiredString(json, "id", &entry_id) || !HasRequiredString(json, "type", &type)) {
    return PackageError::kInvalidManifest;
  }
  if (!entry_ids->insert(entry_id).second) return PackageError::kDuplicateId;
  for(const auto* field:{"chapter","source_url","source_version","display_version"}) {
    if(json.contains(field) && (!json[field].is_string() || json[field].get<std::string>().size()>16384))return PackageError::kInvalidManifest;
  }
  if(json.contains("keywords") && !HasStringArray(json,"keywords",false))return PackageError::kInvalidManifest;
  if(json.contains("original_segments") && !HasStringArray(json,"original_segments",false))return PackageError::kInvalidManifest;

  if (type == "phrase") {
    std::string text;
    std::string pinyin;
    const auto weight = json.find("weight");
    if (!HasRequiredString(json, "text", &text) || !HasRequiredString(json, "pinyin", &pinyin) ||
        weight == json.end() || !weight->is_number() || !HasStringArray(json, "tags", false)) {
      return PackageError::kInvalidManifest;
    }
    return std::nullopt;
  }
  if (type == "continuation") {
    std::string trigger;
    const auto completions = json.find("completions");
    if (!HasRequiredString(json, "trigger", &trigger) || completions == json.end() ||
        !completions->is_array() || completions->empty() || !HasStringArray(json, "tags", false)) {
      return PackageError::kInvalidManifest;
    }
    for (const auto& completion : *completions) {
      std::string text;
      if (!completion.is_object()) return PackageError::kInvalidManifest;
      const auto weight = completion.find("weight");
      if (!HasRequiredString(completion, "text", &text) ||
          weight == completion.end() || !weight->is_number()) {
        return PackageError::kInvalidManifest;
      }
    }
    return std::nullopt;
  }
  if (type == "quotation") {
    std::string work;
    std::string author;
    if (!HasStringArray(json, "segments", true) || !HasRequiredString(json, "work", &work) ||
        !HasRequiredString(json, "author", &author) || !HasStringArray(json, "tags", false)) {
      return PackageError::kInvalidManifest;
    }
    return std::nullopt;
  }
  return PackageError::kInvalidManifest;
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

class Sha256 final {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return;

    ULONG object_bytes = 0;
    ULONG received_bytes = 0;
    if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
                          &received_bytes, 0) < 0 ||
        received_bytes != sizeof(object_bytes)) {
      return;
    }
    object_.resize(object_bytes);
    if (BCryptCreateHash(algorithm_, &hash_, object_.data(),
                         static_cast<ULONG>(object_.size()), nullptr, 0, 0) < 0) {
      hash_ = nullptr;
    }
  }
  ~Sha256() {
    if (hash_ != nullptr) BCryptDestroyHash(hash_);
    if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
  [[nodiscard]] bool valid() const noexcept { return hash_ != nullptr; }
  [[nodiscard]] bool Update(const void* data, const ULONG bytes) {
    return BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), bytes, 0) >= 0;
  }
  [[nodiscard]] bool Finish(std::array<unsigned char, kSha256Bytes>* result) {
    return BCryptFinishHash(hash_, result->data(), static_cast<ULONG>(result->size()), 0) >= 0;
  }

 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  std::vector<unsigned char> object_;
};

std::string ToLowerHex(const std::array<unsigned char, kSha256Bytes>& digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

tl::expected<std::string, PackageError> ReadCurrentEntry(void* reader, const size_t maximum_bytes) {
  if (mz_zip_reader_entry_open(reader) != MZ_OK) {
    return tl::unexpected(PackageError::kInvalidArchive);
  }

  std::string contents;
  std::array<char, 64 * 1024> buffer{};
  for (;;) {
    const auto read = mz_zip_reader_entry_read(reader, buffer.data(),
                                                static_cast<int32_t>(buffer.size()));
    if (read < 0) {
      mz_zip_reader_entry_close(reader);
      return tl::unexpected(PackageError::kInvalidArchive);
    }
    if (read == 0) break;
    if (static_cast<size_t>(read) > maximum_bytes - contents.size()) {
      mz_zip_reader_entry_close(reader);
      return tl::unexpected(PackageError::kLimitExceeded);
    }
    contents.append(buffer.data(), static_cast<size_t>(read));
  }
  if (mz_zip_reader_entry_close(reader) != MZ_OK) {
    return tl::unexpected(PackageError::kInvalidArchive);
  }
  return contents;
}

tl::expected<std::string, PackageError> ValidateAndHashJsonlEntry(void* reader) {
  if (mz_zip_reader_entry_open(reader) != MZ_OK) {
    return tl::unexpected(PackageError::kInvalidArchive);
  }

  Sha256 hash;
  if (!hash.valid()) {
    mz_zip_reader_entry_close(reader);
    return tl::unexpected(PackageError::kIo);
  }
  std::array<char, 64 * 1024> buffer{};
  std::string line;
  std::unordered_set<std::string> entry_ids;
  for (;;) {
    const auto read = mz_zip_reader_entry_read(reader, buffer.data(),
                                                static_cast<int32_t>(buffer.size()));
    if (read < 0) {
      mz_zip_reader_entry_close(reader);
      return tl::unexpected(PackageError::kInvalidArchive);
    }
    if (read > 0 && !hash.Update(buffer.data(), static_cast<ULONG>(read))) {
      mz_zip_reader_entry_close(reader);
      return tl::unexpected(PackageError::kIo);
    }
    if (read == 0) break;
    for (int32_t index = 0; index < read; ++index) {
      const auto character = buffer[static_cast<size_t>(index)];
      if (character == '\n') {
        if (line.empty()) {
          mz_zip_reader_entry_close(reader);
          return tl::unexpected(PackageError::kInvalidManifest);
        }
        if (const auto validation = ValidateJsonlLine(line, &entry_ids)) {
          mz_zip_reader_entry_close(reader);
          return tl::unexpected(*validation);
        }
        line.clear();
      } else {
        if (line.size() >= kMaximumJsonlLineBytes) {
          mz_zip_reader_entry_close(reader);
          return tl::unexpected(PackageError::kLimitExceeded);
        }
        line.push_back(character);
      }
    }
  }
  if (!line.empty()) {
    if (const auto validation = ValidateJsonlLine(line, &entry_ids)) {
      mz_zip_reader_entry_close(reader);
      return tl::unexpected(*validation);
    }
  }
  if (mz_zip_reader_entry_close(reader) != MZ_OK) {
    return tl::unexpected(PackageError::kInvalidArchive);
  }

  std::array<unsigned char, kSha256Bytes> digest{};
  if (!hash.Finish(&digest)) return tl::unexpected(PackageError::kIo);
  return ToLowerHex(digest);
}

tl::expected<Manifest, PackageError> ParseManifest(const std::string_view contents) {
  if (!IsValidUtf8(contents)) return tl::unexpected(PackageError::kInvalidUtf8);

  const auto json = nlohmann::json::parse(contents, nullptr, false, false);
  if (json.is_discarded() || !json.is_object()) {
    return tl::unexpected(PackageError::kInvalidManifest);
  }

  Manifest manifest;
  std::string schema_version;
  std::string name;
  std::string locale;
  std::string source;
  std::string license;
  std::string generated_at;
  if (!HasRequiredString(json, "schema_version", &schema_version) ||
      !HasRequiredString(json, "id", &manifest.package_id) ||
      !HasRequiredString(json, "name", &name) ||
      !HasRequiredString(json, "version", &manifest.version) ||
      !HasRequiredString(json, "locale", &locale) ||
      !HasRequiredString(json, "source", &source) ||
      !HasRequiredString(json, "license", &license) ||
      !HasRequiredString(json, "generated_at", &generated_at)) {
    return tl::unexpected(PackageError::kInvalidManifest);
  }
  if (schema_version != "1.0") return tl::unexpected(PackageError::kUnsupportedSchema);

  const auto content_types = json.find("content_types");
  if (content_types == json.end() || !content_types->is_array() || content_types->empty()) {
    return tl::unexpected(PackageError::kInvalidManifest);
  }
  for (const auto& type : *content_types) {
    if (!type.is_string()) return tl::unexpected(PackageError::kInvalidManifest);
    const auto value = type.get<std::string>();
    if (value != "phrase" && value != "continuation" && value != "quotation") {
      return tl::unexpected(PackageError::kInvalidManifest);
    }
  }

  const auto entries = json.find("entries");
  if (entries == json.end() || !entries->is_object() ||
      !HasRequiredString(*entries, "path", &manifest.entries_path) ||
      !HasRequiredString(*entries, "sha256", &manifest.entries_sha256) ||
      !IsReverseDnsPackageId(manifest.package_id) || !IsSemanticVersion(manifest.version) ||
      !IsLocale(locale) || !IsGeneratedAt(generated_at) ||
      !IsNormalizedRelativePath(manifest.entries_path.c_str()) ||
      !manifest.entries_path.ends_with(".jsonl") || !IsSha256(manifest.entries_sha256)) {
    return tl::unexpected(PackageError::kInvalidManifest);
  }
  return manifest;
}

}  // namespace

tl::expected<ValidatedPackage, PackageError> ValidatePackage(
    const std::filesystem::path& archive_path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(archive_path, error) || error) {
    return tl::unexpected(PackageError::kIo);
  }

  const auto archive_size = std::filesystem::file_size(archive_path, error);
  if (error) return tl::unexpected(PackageError::kIo);
  if (archive_size > kMaximumCompressedBytes) return tl::unexpected(PackageError::kLimitExceeded);

  Reader reader;
  if (reader.get() == nullptr ||
      mz_zip_reader_open_file(reader.get(), reinterpret_cast<const char*>(archive_path.u8string().c_str())) != MZ_OK) {
    return tl::unexpected(PackageError::kInvalidArchive);
  }

  std::uintmax_t total_compressed = 0;
  std::uintmax_t total_expanded = 0;
  for (int result = mz_zip_reader_goto_first_entry(reader.get()); result == MZ_OK;
       result = mz_zip_reader_goto_next_entry(reader.get())) {
    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(reader.get(), &info) != MZ_OK || info == nullptr) {
      return tl::unexpected(PackageError::kInvalidArchive);
    }
    if (info->filename == nullptr) return tl::unexpected(PackageError::kInvalidArchive);
    if (!IsValidUtf8(info->filename)) return tl::unexpected(PackageError::kInvalidUtf8);
    if (!IsNormalizedRelativePath(info->filename) ||
        (info->linkname != nullptr && *info->linkname != '\0')) {
      return tl::unexpected(PackageError::kUnsafePath);
    }
    if (info->compressed_size < 0 || info->uncompressed_size < 0) {
      return tl::unexpected(PackageError::kInvalidArchive);
    }
    const auto compressed = static_cast<std::uintmax_t>(info->compressed_size);
    const auto expanded = static_cast<std::uintmax_t>(info->uncompressed_size);
    if (compressed > kMaximumCompressedBytes - total_compressed ||
        expanded > kMaximumExpandedBytes - total_expanded ||
        (compressed == 0 ? expanded != 0 : expanded > compressed * kMaximumExpansionRatio)) {
      return tl::unexpected(PackageError::kLimitExceeded);
    }
    total_compressed += compressed;
    total_expanded += expanded;
  }

  if (mz_zip_reader_locate_entry(reader.get(), "manifest.json", 0) != MZ_OK) {
    return tl::unexpected(PackageError::kInvalidManifest);
  }
  const auto manifest_contents = ReadCurrentEntry(reader.get(), kMaximumManifestBytes);
  if (!manifest_contents) return tl::unexpected(manifest_contents.error());
  const auto manifest = ParseManifest(*manifest_contents);
  if (!manifest) return tl::unexpected(manifest.error());

  if (mz_zip_reader_locate_entry(reader.get(), manifest->entries_path.c_str(), 0) != MZ_OK) {
    return tl::unexpected(PackageError::kInvalidManifest);
  }
  const auto entries_hash = ValidateAndHashJsonlEntry(reader.get());
  if (!entries_hash) return tl::unexpected(entries_hash.error());
  if (*entries_hash != manifest->entries_sha256) return tl::unexpected(PackageError::kIntegrity);

  return ValidatedPackage{
      .archive_path = archive_path,
      .package_id = std::move(manifest->package_id),
      .version = std::move(manifest->version),
      .entries_sha256 = std::move(manifest->entries_sha256),
  };
}

}  // namespace zhixu::predict::imepack
