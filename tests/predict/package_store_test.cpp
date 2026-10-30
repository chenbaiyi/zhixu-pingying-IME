#include "predict/imepack/package_store.h"
#include "predict/imepack/package_validator.h"

#include <windows.h>
#include <bcrypt.h>
#include <mz.h>
#include <mz_os.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace zhixu::predict::imepack {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              (L"zhixu-package-store-" +
               std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()))) {}
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

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

std::string Manifest(const std::string_view entries_sha256,
                     const std::string_view version = "1.0.0") {
  return R"({"schema_version":"1.0","id":"com.zhixu.acceptance","name":"Acceptance package",)"
         R"("version":")" + std::string(version) + R"(","locale":"zh-CN","content_types":["continuation"],)"
         R"("source":"local-test","license":"MIT","generated_at":"2026-09-24T00:00:00Z",)"
         R"("entries":{"path":"entries.jsonl","sha256":")" + std::string(entries_sha256) + R"("}})";
}

bool CreateArchive(const std::filesystem::path& path,
                   const std::vector<std::pair<std::string, std::string>>& entries) {
  void* writer = mz_zip_writer_create();
  if (writer == nullptr) return false;
  const auto close = [&writer]() {
    if (writer != nullptr) {
      mz_zip_writer_close(writer);
      mz_zip_writer_delete(&writer);
    }
  };
  if (mz_zip_writer_open_file(writer, path.string().c_str(), 0, 0) != MZ_OK) {
    close();
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
      close();
      return false;
    }
  }
  const bool closed = mz_zip_writer_close(writer) == MZ_OK;
  mz_zip_writer_delete(&writer);
  return closed;
}

TEST(PackageStore, InstallsImmutablePackageAndSwitchesActiveVersion) {
  const std::string entries =
      "{\"id\":\"acceptance.daoyun\",\"type\":\"continuation\",\"trigger\":\"道韵\","
      "\"completions\":[{\"text\":\"法则\",\"weight\":1.0}],\"tags\":[\"acceptance\"]}\n"
      "{\"id\":\"acceptance.quotation\",\"type\":\"quotation\","
      "\"segments\":[\"道可道\",\"非常道\"],\"work\":\"道德经\",\"author\":\"老子\","
      "\"tags\":[\"acceptance\"]}\n";
  TemporaryDirectory temporary;
  const auto archive = temporary.path() / L"acceptance.imepack";
  ASSERT_TRUE(CreateArchive(archive, {
      {"manifest.json", Manifest(Sha256(entries))},
      {"entries.jsonl", entries},
  }));
  const auto validated = ValidatePackage(archive);
  ASSERT_TRUE(validated);

  PackageStore store(temporary.path() / L"packages");
  const auto installed = store.InstallPackage(*validated);

  ASSERT_TRUE(installed);
  EXPECT_EQ(installed->package_id, "com.zhixu.acceptance");
  EXPECT_EQ(installed->version, "1.0.0");
  EXPECT_TRUE(std::filesystem::exists(installed->package_path / L"manifest.json"));
  const auto index_path = installed->package_path / L"indexes" / L"continuation.jsonl";
  std::ifstream index(index_path, std::ios::binary);
  ASSERT_TRUE(index);
  const std::string index_contents((std::istreambuf_iterator<char>(index)),
                                   std::istreambuf_iterator<char>());
  EXPECT_NE(index_contents.find("\"trigger\":\"道韵\""), std::string::npos);
  EXPECT_NE(index_contents.find("\"text\":\"法则\""), std::string::npos);
  EXPECT_NE(index_contents.find("\"tags\":[\"acceptance\"]"), std::string::npos);
  const auto quotation_index_path = installed->package_path / L"indexes" / L"quotation.jsonl";
  std::ifstream quotation_index(quotation_index_path, std::ios::binary);
  ASSERT_TRUE(quotation_index);
  const std::string quotation_index_contents((std::istreambuf_iterator<char>(quotation_index)),
                                             std::istreambuf_iterator<char>());
  EXPECT_NE(quotation_index_contents.find("\"match\":\"道可道\""), std::string::npos);
  EXPECT_NE(quotation_index_contents.find("\"text\":\"非常道\""), std::string::npos);
  EXPECT_NE(quotation_index_contents.find("\"work\":\"道德经\""), std::string::npos);
  EXPECT_NE(quotation_index_contents.find("\"author\":\"老子\""), std::string::npos);
  EXPECT_NE(quotation_index_contents.find("\"source_context\":\"segment:1\""), std::string::npos);
  EXPECT_NE(quotation_index_contents.find("\"tags\":[\"acceptance\"]"), std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(temporary.path() / L"packages" / L"com.zhixu.acceptance" /
                                      L"active.json"));
  const auto active_roots = store.ActivePackageRoots();
  ASSERT_TRUE(active_roots);
  ASSERT_EQ(active_roots->size(), 1u);
  EXPECT_EQ(active_roots->front(), installed->package_path);

  const auto repeated = store.InstallPackage(*validated);

  ASSERT_FALSE(repeated);
  EXPECT_EQ(repeated.error(), PackageError::kAlreadyInstalled);
}

TEST(PackageStore, RejectsMissingArchiveWithoutCreatingActiveVersion) {
  TemporaryDirectory temporary;
  PackageStore store(temporary.path() / L"packages");
  const ValidatedPackage package{
      .archive_path = temporary.path() / L"missing.imepack",
      .package_id = "com.zhixu.acceptance",
      .version = "1.0.0",
  };

  const auto result = store.InstallPackage(package);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PackageError::kIo);
  EXPECT_FALSE(std::filesystem::exists(temporary.path() / L"packages" / L"com.zhixu.acceptance" /
                                       L"active.json"));
}

TEST(PackageStore, RollsBackThenDisablesBeforeRemovingVersions) {
  const std::string entries =
      "{\"id\":\"acceptance.daoyun\",\"type\":\"continuation\",\"trigger\":\"道韵\","
      "\"completions\":[{\"text\":\"法则\",\"weight\":1.0}],\"tags\":[\"acceptance\"]}\n";
  TemporaryDirectory temporary;
  const auto first_archive = temporary.path() / L"acceptance-1.0.0.imepack";
  const auto second_archive = temporary.path() / L"acceptance-1.0.1.imepack";
  ASSERT_TRUE(CreateArchive(first_archive, {
      {"manifest.json", Manifest(Sha256(entries), "1.0.0")},
      {"entries.jsonl", entries},
  }));
  ASSERT_TRUE(CreateArchive(second_archive, {
      {"manifest.json", Manifest(Sha256(entries), "1.0.1")},
      {"entries.jsonl", entries},
  }));
  const auto first_validated = ValidatePackage(first_archive);
  const auto second_validated = ValidatePackage(second_archive);
  ASSERT_TRUE(first_validated);
  ASSERT_TRUE(second_validated);

  PackageStore store(temporary.path() / L"packages");
  ASSERT_TRUE(store.InstallPackage(*first_validated));
  ASSERT_TRUE(store.InstallPackage(*second_validated));

  const auto active_uninstall = store.Uninstall("com.zhixu.acceptance", "1.0.1");
  ASSERT_FALSE(active_uninstall);
  EXPECT_EQ(active_uninstall.error(), PackageError::kIo);
  ASSERT_TRUE(store.Rollback("com.zhixu.acceptance"));
  ASSERT_TRUE(store.Uninstall("com.zhixu.acceptance", "1.0.1"));

  ASSERT_TRUE(store.Disable("com.zhixu.acceptance"));
  ASSERT_TRUE(store.Uninstall("com.zhixu.acceptance", "1.0.0"));
  EXPECT_FALSE(std::filesystem::exists(temporary.path() / L"packages" / L"com.zhixu.acceptance" /
                                       L"active.json"));
}

TEST(PackageStore, RejectsDifferentContentsAtAnInstalledImmutableVersion) {
 TemporaryDirectory temporary;
 const std::string first=R"({"id":"c1","type":"continuation","trigger":"昏暗","completions":[{"text":"黯淡无光","weight":1.0}],"tags":[]})";
 const std::string second=R"({"id":"c2","type":"continuation","trigger":"昏暗","completions":[{"text":"灯火通明","weight":1.0}],"tags":[]})";
 const auto a=temporary.path()/L"first.imepack",b=temporary.path()/L"second.imepack";
 ASSERT_TRUE(CreateArchive(a,{{"manifest.json",Manifest(Sha256(first))},{"entries.jsonl",first}}));
 ASSERT_TRUE(CreateArchive(b,{{"manifest.json",Manifest(Sha256(second))},{"entries.jsonl",second}}));
 const auto va=ValidatePackage(a),vb=ValidatePackage(b);ASSERT_TRUE(va);ASSERT_TRUE(vb);
 PackageStore store(temporary.path()/L"packages");ASSERT_TRUE(store.InstallPackage(*va));
 const auto result=store.InstallPackage(*vb);ASSERT_FALSE(result);EXPECT_EQ(result.error(),PackageError::kIntegrity);
}

}  // namespace
}  // namespace zhixu::predict::imepack
