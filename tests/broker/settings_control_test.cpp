#include <windows.h>
#undef KEY_EVENT
#include "predict/imepack/package_store.h"
#include "predict/imepack/package_validator.h"
#include <bcrypt.h>
#include <mz.h>
#include <mz_os.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <array>
#include <fstream>
#include <vector>
#include "broker/settings_control.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace zhixu::broker {
namespace {

std::filesystem::path UniqueSettingsPath() {
  return std::filesystem::temp_directory_path() /
         (L"ZhixuIME-settings-control-" + std::to_wstring(GetCurrentProcessId()) + L".json");
}

protocol::Envelope Request(const protocol::v1::MessageType type, const std::string& payload,
                           const std::uint64_t request_id) {
  protocol::Envelope request;
  request.set_protocol_version(protocol::kProtocolVersion);
  request.set_message_type(type);
  request.set_session_id("settings-test");
  request.set_request_id(request_id);
  request.set_payload(payload);
  return request;
}

protocol::Envelope ControlRequest(const protocol::v1::ControlOperation operation,
                                  const std::string& arguments, const std::uint64_t request_id) {
  protocol::v1::ControlCommand command;
  command.set_operation(operation);
  command.set_arguments(arguments);
  std::string payload;
  command.SerializeToString(&payload);
  return Request(protocol::v1::CONTROL_COMMAND, payload, request_id);
}

protocol::Envelope HelloRequest(const std::uint64_t request_id) {
  protocol::v1::Hello hello;
  hello.set_min_version(protocol::kProtocolVersion);
  hello.set_max_version(protocol::kProtocolVersion);
  std::string payload;
  hello.SerializeToString(&payload);
  return Request(protocol::v1::HELLO, payload, request_id);
}

protocol::v1::ControlResult ParseResult(const protocol::Envelope& response) {
  EXPECT_EQ(response.message_type(), protocol::v1::CONTROL_RESULT);
  protocol::v1::ControlResult result;
  EXPECT_TRUE(result.ParseFromString(response.payload()));
  return result;
}

bool ConnectWithRetry(ipc::NamedPipeClient& client, const ipc::PipeEndpoint& endpoint) {
  using namespace std::chrono_literals;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (client.Connect(endpoint, 50ms)) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
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

TEST(SettingsControl, RejectsCommandsBeforeHello) {
  const auto path = UniqueSettingsPath();
  std::filesystem::remove(path);
  settings::SettingsStore store(path);
  SettingsControl control(store);

  const auto response = control.Handle(ControlRequest(protocol::v1::GET_SETTINGS, "", 1));

  EXPECT_EQ(response.message_type(), protocol::v1::MESSAGE_TYPE_UNSPECIFIED);
}

TEST(SettingsControl, AppliesWhitelistedPatchAndReturnsPersistedSettings) {
  const auto path = UniqueSettingsPath();
  std::filesystem::remove(path);
  settings::SettingsStore store(path);
  SettingsControl control(store);
  ASSERT_EQ(control.Handle(HelloRequest(1)).message_type(), protocol::v1::HELLO_ACK);

  const auto patched = ParseResult(control.Handle(
      ControlRequest(protocol::v1::PATCH_SETTINGS, R"({"privacy_mode":true,"candidate_count":8})", 2)));
  EXPECT_EQ(patched.error_code(), static_cast<std::uint32_t>(SettingsControlError::kOk));

  const auto loaded = store.Load();
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->privacy_mode);
  EXPECT_EQ(loaded->candidate_count, 8U);
  EXPECT_TRUE(loaded->learning_enabled);

  const auto returned = ParseResult(control.Handle(ControlRequest(protocol::v1::GET_SETTINGS, "", 3)));
  EXPECT_EQ(returned.error_code(), static_cast<std::uint32_t>(SettingsControlError::kOk));
  EXPECT_NE(returned.data().find("\"privacy_mode\": true"), std::string::npos);
  EXPECT_NE(returned.data().find("\"candidate_count\": 8"), std::string::npos);
}

TEST(SettingsControl, RejectsUnknownPatchFieldsWithoutChangingSettings) {
  const auto path = UniqueSettingsPath();
  std::filesystem::remove(path);
  settings::SettingsStore store(path);
  SettingsControl control(store);
  ASSERT_EQ(control.Handle(HelloRequest(1)).message_type(), protocol::v1::HELLO_ACK);

  const auto rejected =
      ParseResult(control.Handle(ControlRequest(protocol::v1::PATCH_SETTINGS, R"({"unexpected":true})", 2)));
  EXPECT_EQ(rejected.error_code(), static_cast<std::uint32_t>(SettingsControlError::kMalformedArguments));

  const auto loaded = store.Load();
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->privacy_mode);
  EXPECT_EQ(loaded->candidate_count, 8U);
}

TEST(SettingsControl, SecuredPipeRequiresHelloBeforeSettingsCommands) {
  using namespace std::chrono_literals;
  const auto path = UniqueSettingsPath();
  std::filesystem::remove(path);
  settings::SettingsStore store(path);
  SettingsControl control(store);
  constexpr std::wstring_view kRole = L"broker-settings-control-test";
  std::jthread server([&](const std::stop_token stop_token) {
    const auto served = control.Serve(kRole, stop_token);
    EXPECT_TRUE(served) << static_cast<int>(served.error());
  });

  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(kRole);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  const auto before_hello = client.Call(ControlRequest(protocol::v1::GET_SETTINGS, "", 1), 1s);
  ASSERT_TRUE(before_hello);
  EXPECT_EQ(before_hello->message_type(), protocol::v1::MESSAGE_TYPE_UNSPECIFIED);

  const auto hello = client.Call(HelloRequest(2), 1s);
  ASSERT_TRUE(hello);
  EXPECT_EQ(hello->message_type(), protocol::v1::HELLO_ACK);
  const auto settings = client.Call(ControlRequest(protocol::v1::GET_SETTINGS, "", 3), 1s);
  ASSERT_TRUE(settings);
  EXPECT_EQ(ParseResult(*settings).error_code(), static_cast<std::uint32_t>(SettingsControlError::kOk));

  client.Close();
  server.request_stop();
}

TEST(SettingsControl, ClearsOnlyScopedLearningAndReportsMissingPackageFailure) {
 const auto root=std::filesystem::temp_directory_path()/(L"zhixu-corpus-control-"+std::to_wstring(GetCurrentProcessId()));
 std::filesystem::create_directories(root);
 settings::SettingsStore store(root/L"settings.json");SettingsControl control(store,root);
 ASSERT_EQ(control.Handle(HelloRequest(1)).message_type(),protocol::v1::HELLO_ACK);
 EXPECT_EQ(ParseResult(control.Handle(ControlRequest(protocol::v1::CLEAR_LEARNING,"",2))).error_code(),0u);
 EXPECT_NE(ParseResult(control.Handle(ControlRequest(protocol::v1::INSTALL_PACKAGE,"missing.imepack",3))).error_code(),0u);
 std::filesystem::remove_all(root);
}

TEST(SettingsControl, ReinstallReactivatesAnExistingImmutableVersion) {
 const auto root=std::filesystem::temp_directory_path()/(L"zhixu-reinstall-"+std::to_wstring(GetTickCount64()));std::filesystem::create_directories(root);
 const std::string entries=R"({"id":"one","type":"continuation","trigger":"山风","completions":[{"text":"拂过山岗","weight":1.0}],"tags":[]})" "\n";
 const auto archive=root/L"valid.imepack";ASSERT_TRUE(CreateArchive(archive,{{"manifest.json",Manifest(Sha256(entries))},{"entries.jsonl",entries}}));
 const auto validated=predict::imepack::ValidatePackage(archive);ASSERT_TRUE(validated);
 predict::imepack::PackageStore packages(root/L"packages");ASSERT_TRUE(packages.InstallPackage(*validated));ASSERT_TRUE(packages.Disable("com.zhixu.acceptance"));
 settings::SettingsStore settings(root/L"settings.json");SettingsControl control(settings,root);ASSERT_EQ(control.Handle(HelloRequest(1)).message_type(),protocol::v1::HELLO_ACK);
 EXPECT_EQ(ParseResult(control.Handle(ControlRequest(protocol::v1::INSTALL_PACKAGE,archive.string(),2))).error_code(),0u);
 const auto active=packages.ActivePackageRoots();ASSERT_TRUE(active);EXPECT_EQ(active->size(),1u);
 std::filesystem::remove_all(root);
}
TEST(SettingsControl, InstallsFromChineseArchivePath) {
 const auto root=std::filesystem::temp_directory_path()/(L"zhixu-reinstall-"+std::to_wstring(GetTickCount64()));std::filesystem::create_directories(root);
 const std::string entries=R"({"id":"one","type":"continuation","trigger":"山风","completions":[{"text":"拂过山岗","weight":1.0}],"tags":[]})" "\n";
 const auto archive=root/L"valid.imepack";ASSERT_TRUE(CreateArchive(archive,{{"manifest.json",Manifest(Sha256(entries))},{"entries.jsonl",entries}}));
 const auto chinese=root/L"玄幻资料.imepack";std::filesystem::rename(archive,chinese);
 predict::imepack::PackageStore packages(root/L"packages");
 settings::SettingsStore settings(root/L"settings.json");SettingsControl control(settings,root);ASSERT_EQ(control.Handle(HelloRequest(1)).message_type(),protocol::v1::HELLO_ACK);
 EXPECT_EQ(ParseResult(control.Handle(ControlRequest(protocol::v1::INSTALL_PACKAGE,std::string(reinterpret_cast<const char*>(chinese.u8string().c_str())),2))).error_code(),0u);
 const auto active=packages.ActivePackageRoots();ASSERT_TRUE(active);EXPECT_EQ(active->size(),1u);
 std::filesystem::remove_all(root);
}
}  // namespace
}  // namespace zhixu::broker
