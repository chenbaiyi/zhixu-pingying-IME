#include "predict/imepack/package_validator.h"

#include <windows.h>
#include <bcrypt.h>
#include <mz.h>
#include <mz_os.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <chrono>
#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace zhixu::predict::imepack {
namespace {

class TemporaryArchive final {
 public:
  TemporaryArchive()
      : path_(std::filesystem::temp_directory_path() /
              (L"zhixu-package-validator-" +
               std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) +
               L".imepack")) {}
  ~TemporaryArchive() { std::filesystem::remove(path_); }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

bool CreateArchive(const std::filesystem::path& path,
                   const std::vector<std::pair<std::string, std::string>>& entries) {
  void* writer = mz_zip_writer_create();
  if (writer == nullptr) return false;

  const auto close_writer = [&writer]() {
    if (writer != nullptr) {
      mz_zip_writer_close(writer);
      mz_zip_writer_delete(&writer);
    }
  };
  if (mz_zip_writer_open_file(writer, path.string().c_str(), 0, 0) != MZ_OK) {
    close_writer();
    return false;
  }
  mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_DEFLATE);
  mz_zip_writer_set_compress_level(writer, MZ_COMPRESS_LEVEL_BEST);
  for (const auto& [name, contents] : entries) {
    mz_zip_file info{};
    info.version_madeby = MZ_VERSION_MADEBY;
    info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
    info.flag = MZ_ZIP_FLAG_UTF8;
    info.filename = name.c_str();
    info.uncompressed_size = static_cast<int64_t>(contents.size());
    if (mz_zip_writer_entry_open(writer, &info) != MZ_OK ||
        (!contents.empty() &&
         mz_zip_writer_entry_write(writer, contents.data(), static_cast<int32_t>(contents.size())) !=
             static_cast<int32_t>(contents.size())) ||
        mz_zip_writer_entry_close(writer) != MZ_OK) {
      close_writer();
      return false;
    }
  }
  const bool closed = mz_zip_writer_close(writer) == MZ_OK;
  mz_zip_writer_delete(&writer);
  return closed;
}

std::string Manifest(std::string schema_version, std::string entries_sha256) {
  return R"({"schema_version":")" + std::move(schema_version) +
         R"(","id":"com.zhixu.acceptance","name":"Acceptance package","version":"1.0.0",)"
         R"("locale":"zh-CN","content_types":["continuation"],"source":"local-test",)"
         R"("license":"MIT","generated_at":"2026-09-24T00:00:00Z",)"
         R"("entries":{"path":"entries.jsonl","sha256":")" + std::move(entries_sha256) + R"("}})";
}

std::string Sha256(const std::string_view contents) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  EXPECT_EQ(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0), 0);

  ULONG object_bytes = 0;
  ULONG received_bytes = 0;
  EXPECT_EQ(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
                              &received_bytes, 0), 0);
  std::vector<unsigned char> object(object_bytes);
  BCRYPT_HASH_HANDLE hash = nullptr;
  EXPECT_EQ(BCryptCreateHash(algorithm, &hash, object.data(), static_cast<ULONG>(object.size()),
                             nullptr, 0, 0), 0);
  EXPECT_EQ(BCryptHashData(hash, reinterpret_cast<PUCHAR>(
                                      const_cast<char*>(contents.data())),
                           static_cast<ULONG>(contents.size()), 0), 0);
  std::array<unsigned char, 32> digest{};
  EXPECT_EQ(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0), 0);
  EXPECT_EQ(BCryptDestroyHash(hash), 0);
  EXPECT_EQ(BCryptCloseAlgorithmProvider(algorithm, 0), 0);

  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

TEST(PackageValidator, RejectsMissingArchiveWithoutCreatingPackageState) {
  const auto result = ValidatePackage(std::filesystem::temp_directory_path() / L"missing.imepack");

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kIo);
}

TEST(PackageValidator, RejectsParentTraversalEntry) {
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {{"../escape.txt", "content"}}));

  const auto result = ValidatePackage(archive.path());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kUnsafePath);
}

TEST(PackageValidator, RejectsDriveQualifiedEntry) {
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {{"C:/escape.txt", "content"}}));

  const auto result = ValidatePackage(archive.path());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kUnsafePath);
}

TEST(PackageValidator, RejectsArchiveExceedingExpansionRatio) {
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {{"payload.txt", std::string(128 * 1024, '\0')}}));

  const auto result = ValidatePackage(archive.path());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kLimitExceeded);
}

TEST(PackageValidator, AcceptsSupportedManifestWithEmptyEntries) {
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {
      {"manifest.json", Manifest("1.0", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
      {"entries.jsonl", ""},
  }));

  const auto result = ValidatePackage(archive.path());

  ASSERT_TRUE(result);
  EXPECT_EQ(result->package_id, "com.zhixu.acceptance");
  EXPECT_EQ(result->version, "1.0.0");
}

TEST(PackageValidator, RejectsUnsupportedManifestSchema) {
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {
      {"manifest.json", Manifest("2.0", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")},
      {"entries.jsonl", ""},
  }));

  const auto result = ValidatePackage(archive.path());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kUnsupportedSchema);
}

TEST(PackageValidator, RejectsEntriesWithUnexpectedSha256) {
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {
      {"manifest.json", Manifest("1.0", std::string(64, '0'))},
      {"entries.jsonl", "{\"id\":\"integrity\",\"type\":\"phrase\",\"text\":\"text\","
                        "\"pinyin\":\"wenben\",\"weight\":1.0,\"tags\":[]}\n"},
  }));

  const auto result = ValidatePackage(archive.path());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kIntegrity);
}

TEST(PackageValidator, RejectsInvalidUtf8Manifest) {
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {
      {"manifest.json", std::string("{\"schema_version\":\"1.0\",\"name\":\"") +
                            static_cast<char>(0xc3) + "\"}"},
      {"entries.jsonl", ""},
  }));

  const auto result = ValidatePackage(archive.path());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kInvalidUtf8);
}

TEST(PackageValidator, AcceptsDaoyunContinuationPackage) {
  const std::string entries =
      "{\"id\":\"acceptance.daoyun\",\"type\":\"continuation\",\"trigger\":\"道韵\","
      "\"completions\":[{\"text\":\"法则\",\"weight\":1.0}],\"tags\":[\"acceptance\"]}\n";
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {
      {"manifest.json", Manifest("1.0", Sha256(entries))},
      {"entries.jsonl", entries},
  }));

  const auto result = ValidatePackage(archive.path());

  ASSERT_TRUE(result);
  EXPECT_EQ(result->package_id, "com.zhixu.acceptance");
}

TEST(PackageValidator, RejectsDuplicateEntryIds) {
  const std::string entries =
      "{\"id\":\"dup\",\"type\":\"phrase\",\"text\":\"one\",\"pinyin\":\"yi\",\"weight\":1.0,\"tags\":[]}\n"
      "{\"id\":\"dup\",\"type\":\"phrase\",\"text\":\"two\",\"pinyin\":\"er\",\"weight\":1.0,\"tags\":[]}\n";
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {
      {"manifest.json", Manifest("1.0", Sha256(entries))},
      {"entries.jsonl", entries},
  }));

  const auto result = ValidatePackage(archive.path());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kDuplicateId);
}

TEST(PackageValidator, RejectsInvalidUtf8Entries) {
  const std::string entries = std::string("{\"id\":\"bad\",\"type\":\"phrase\",\"text\":\"") +
                              static_cast<char>(0xc3) + "\"}\n";
  TemporaryArchive archive;
  ASSERT_TRUE(CreateArchive(archive.path(), {
      {"manifest.json", Manifest("1.0", Sha256(entries))},
      {"entries.jsonl", entries},
  }));

  const auto result = ValidatePackage(archive.path());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kInvalidUtf8);
}

}  // namespace
}  // namespace zhixu::predict::imepack
