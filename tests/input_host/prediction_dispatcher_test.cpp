#include "input_host/prediction/prediction_dispatcher.h"
#include "input_host/prediction/prediction_runtime.h"
#include "input_host/prediction/prediction_service_client.h"

#include <chrono>
#include <future>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include "common/ipc/named_pipe.h"
#include "common/protocol/protocol_handshake.h"

#include <Windows.h>
#include "common/settings/settings_store.h"
#include "common/settings/suggestion_policy.h"
#include "predict/personalization/personalization_store.h"
#include <fstream>

namespace zhixu::input_host {
namespace {

using namespace std::chrono_literals;

std::filesystem::path ExecutableDirectory() {
  wchar_t module_path[MAX_PATH]{};
  const auto length = GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
  if (length == 0 || length == std::size(module_path)) return {};
  return std::filesystem::path(module_path).parent_path();
}

class ScopedChildProcess final {
 public:
  explicit ScopedChildProcess(const std::filesystem::path& executable, const std::wstring& args = {}) {
    STARTUPINFOW startup{.cb = sizeof(startup)};
    std::wstring command_line = L"\"" + executable.wstring() + L"\" " + args;
    if (CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, executable.parent_path().c_str(), &startup, &process_) == FALSE) {
      process_ = {};
    }
  }

  ~ScopedChildProcess() {
    if (process_.hProcess == nullptr) return;
    if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
      TerminateProcess(process_.hProcess, ERROR_CANCELLED);
      WaitForSingleObject(process_.hProcess, static_cast<DWORD>(1s.count()));
    }
    CloseHandle(process_.hThread);
    CloseHandle(process_.hProcess);
  }

  [[nodiscard]] bool started() const noexcept { return process_.hProcess != nullptr; }
  [[nodiscard]] bool running() const noexcept {
    return process_.hProcess != nullptr && WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT;
  }
  [[nodiscard]] DWORD exit_code() const noexcept {
    DWORD code = STILL_ACTIVE;
    if (process_.hProcess != nullptr) GetExitCodeProcess(process_.hProcess, &code);
    return code;
  }

 private:
  PROCESS_INFORMATION process_{};
};

TEST(PredictionDispatcher, DebouncesToTheLatestConfirmedCommit) {
  std::mutex mutex;
  std::condition_variable completed;
  std::optional<ConfirmedCommit> dispatched;
  PredictionDispatcher dispatcher(
      [&](ConfirmedCommit commit) {
        { std::lock_guard lock(mutex); dispatched = std::move(commit); }
        completed.notify_all();
      },
      100ms);

  dispatcher.Submit({.text = "first", .context_version = 1, .allow_prediction = true});
  std::this_thread::sleep_for(20ms);
  const auto latest_submitted = std::chrono::steady_clock::now();
  dispatcher.Submit({.text = "latest", .context_version = 2, .allow_prediction = true});

  std::unique_lock lock(mutex);
  ASSERT_TRUE(completed.wait_for(lock, 1s, [&] { return dispatched.has_value(); }));
  EXPECT_EQ(dispatched->text, "latest");
  EXPECT_GE(std::chrono::steady_clock::now() - latest_submitted, 80ms);
}

TEST(PredictionDispatcher, InvalidateCancelsAnUndispatchedCommit) {
  std::mutex mutex;
  std::condition_variable completed;
  bool dispatched = false;
  PredictionDispatcher dispatcher(
      [&](ConfirmedCommit) {
        { std::lock_guard lock(mutex); dispatched = true; }
        completed.notify_all();
      },
      100ms);

  dispatcher.Submit({.text = "commit", .context_version = 1, .allow_prediction = true});
  dispatcher.Invalidate();

  std::unique_lock lock(mutex);
  EXPECT_FALSE(completed.wait_for(lock, 200ms, [&] { return dispatched; }));
}

TEST(PredictionRuntime, SuppressesAResultThatFinishesAfterInvalidation) {
  std::mutex mutex;
  std::condition_variable prediction_started;
  std::condition_variable release_prediction;
  std::condition_variable prediction_finished;
  bool started = false;
  bool released = false;
  bool finished = false;
  unsigned int delivered = 0;
  PredictionRuntime runtime(
      [&](const ConfirmedCommit&) -> std::optional<protocol::v1::PredictionResult> {
        {
          std::lock_guard lock(mutex);
          started = true;
        }
        prediction_started.notify_all();
        std::unique_lock lock(mutex);
        release_prediction.wait(lock, [&] { return released; });
        protocol::v1::PredictionResult result;
        result.set_final_result(true);
        result.add_candidates()->set_text("late");
        finished = true;
        prediction_finished.notify_all();
        return result;
      },
      [&](protocol::v1::PredictionResult, std::uint64_t) {
        std::lock_guard lock(mutex);
        ++delivered;
      },
      0ms);

  runtime.Submit({.text = "commit", .context_version = 7, .allow_prediction = true});
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(prediction_started.wait_for(lock, 1s, [&] { return started; }));
  }
  runtime.Invalidate();
  {
    std::lock_guard lock(mutex);
    released = true;
  }
  release_prediction.notify_all();
  std::unique_lock lock(mutex);
  ASSERT_TRUE(prediction_finished.wait_for(lock, 1s, [&] { return finished; }));
  EXPECT_EQ(delivered, 0U);
}

TEST(PredictionRuntime, DeliversTheCurrentFinalResultWithItsContextVersion) {
  std::mutex mutex;
  std::condition_variable delivered_condition;
  std::optional<protocol::v1::PredictionResult> delivered;
  std::uint64_t delivered_context_version = 0;
  PredictionRuntime runtime(
      [](const ConfirmedCommit&) -> std::optional<protocol::v1::PredictionResult> {
        protocol::v1::PredictionResult result;
        result.set_final_result(true);
        result.add_candidates()->set_text("current");
        return result;
      },
      [&](protocol::v1::PredictionResult result, const std::uint64_t context_version) {
        {
          std::lock_guard lock(mutex);
          delivered = std::move(result);
          delivered_context_version = context_version;
        }
        delivered_condition.notify_all();
      },
      0ms);

  runtime.Submit({.text = "commit", .context_version = 8, .allow_prediction = true});

  std::unique_lock lock(mutex);
  ASSERT_TRUE(delivered_condition.wait_for(lock, 1s, [&] { return delivered.has_value(); }));
  EXPECT_EQ(delivered_context_version, 8U);
  ASSERT_EQ(delivered->candidates_size(), 1);
  EXPECT_EQ(delivered->candidates(0).text(), "current");
}

TEST(PredictionServiceClient, SendsConfirmedCommitAndAcceptsMatchingResult) {
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(L"prediction-service-client-test");
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeServer server;
  protocol::ProtocolHandshake handshake;
  std::mutex request_mutex;
  std::optional<protocol::v1::PredictionRequest> request;
  std::jthread loop([&](const std::stop_token stop_token) {
    const auto served = server.Listen(*endpoint, [&](const protocol::Envelope& envelope) {
      protocol::Envelope response;
      response.set_protocol_version(protocol::kProtocolVersion);
      response.set_session_id(envelope.session_id());
      response.set_request_id(envelope.request_id());
      response.set_context_version(envelope.context_version());
      if (envelope.message_type() == protocol::v1::HELLO) {
        protocol::v1::Hello hello;
        if (!hello.ParseFromString(envelope.payload())) return response;
        const auto accepted = handshake.Accept(hello);
        if (!accepted) return response;
        response.set_message_type(protocol::v1::HELLO_ACK);
        accepted->SerializeToString(response.mutable_payload());
        return response;
      }
      if (envelope.message_type() != protocol::v1::PREDICTION_REQUEST ||
          !handshake.Validate(envelope.message_type())) return response;
      protocol::v1::PredictionRequest parsed;
      if (!parsed.ParseFromString(envelope.payload())) return response;
      { std::lock_guard lock(request_mutex); request = parsed; }
      protocol::v1::PredictionResult result;
      result.set_final_result(true);
      result.add_candidates()->set_text("result");
      result.SerializeToString(response.mutable_payload());
      response.set_message_type(protocol::v1::PREDICTION_RESULT);
      return response;
    }, stop_token);
    EXPECT_TRUE(served) << static_cast<int>(served.error());
  });
  ASSERT_TRUE(server.WaitUntilListening(1s));

  PredictionServiceClient client(*endpoint);
  const auto result = client.Predict({.text = "confirmed", .context_version = 7,
                                      .allow_prediction = true});

  ASSERT_TRUE(result.has_value());
  std::lock_guard lock(request_mutex);
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->context_text(), "confirmed");
  EXPECT_EQ(request->candidate_limit(), 30U);
  EXPECT_TRUE(request->privacy().allow_prediction());
  ASSERT_EQ(result->candidates_size(), 1);
  EXPECT_EQ(result->candidates(0).text(), "result");
  server.Close();
}

TEST(PredictionServiceClient, AcceptsTheActualServiceHandshakeAtANonzeroContextVersion) {
  const auto root=std::filesystem::temp_directory_path()/(L"zhixu-handshake-"+std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(root);
  settings::SettingsStore store(root/L"settings.json");ASSERT_TRUE(store.Save(settings::Settings::Defaults()));
  const auto role=L"handshake-"+std::to_wstring(GetCurrentProcessId());
  const auto executable = std::filesystem::path(ZHIXU_TEST_PREDICT_SERVICE_EXE);
  ASSERT_TRUE(std::filesystem::exists(executable));
  ScopedChildProcess service(executable,L"--data-root \""+root.wstring()+L"\" --pipe-role "+role);
  ASSERT_TRUE(service.started());
  ASSERT_TRUE(service.running()) << "PredictService exited with " << service.exit_code();

  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  ASSERT_TRUE(endpoint);
  PredictionServiceClient client(*endpoint);
  std::optional<protocol::v1::PredictionResult> result;
  for (int attempt = 0; attempt < 50 && !result.has_value(); ++attempt) {
    result = client.Predict({.text = "confirmed", .context_version = 7, .allow_prediction = true,.policy_generation=settings::SuggestionPolicy(root/L"settings.json",root/L"packages")});
    if (!result.has_value()) std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(result.has_value()) << "PredictService running=" << service.running()
                                  << " exit_code=" << service.exit_code();
  EXPECT_TRUE(result->final_result());
}

TEST(PredictionServiceClient, LearnsOnceAndPredictsThroughIsolatedActualService) {
 using namespace std::chrono_literals;
 const auto root=std::filesystem::temp_directory_path()/(L"zhixu-corpus-service-"+std::to_wstring(GetCurrentProcessId()));
 const auto package=root/L"packages"/L"com.zhixu.test"/L"1.0.0";
 std::filesystem::create_directories(package/L"indexes");
 std::ofstream(package.parent_path()/L"active.json")<<R"({"version":"1.0.0"})";
 std::ofstream(package/L"indexes"/L"continuation.jsonl")<<R"({"id":"c1","normalized_trigger":"昏暗","text":"黯淡无光","weight":1.0,"tags":[]})";
 settings::SettingsStore settings(root/L"settings.json");ASSERT_TRUE(settings.Save(settings::Settings::Defaults()));
 const auto role=L"corpus-test-"+std::to_wstring(GetCurrentProcessId());const auto endpoint=ipc::PipeEndpoint::ForCurrentUser(role);ASSERT_TRUE(endpoint);
 {
  ScopedChildProcess child(std::filesystem::path(ZHIXU_TEST_PREDICT_SERVICE_EXE), L"--data-root \""+root.wstring()+L"\" --pipe-role "+role);ASSERT_TRUE(child.started());
  PredictionServiceClient client(*endpoint);std::optional<protocol::v1::PredictionResult> result;
  ConfirmedCommit commit{.text="昏暗",.context_version=7,.allow_learning=true,.allow_prediction=true,.context_text="大地一片昏暗",.context_before="大地一片",.session_id="isolated",.commit_id=9,.policy_generation=settings::SuggestionPolicy(root/L"settings.json",root/L"packages")};
  for(int i=0;i<50 && !result;++i){result=client.Predict(commit);if(!result)std::this_thread::sleep_for(20ms);}
  ASSERT_TRUE(result);ASSERT_EQ(result->candidates_size(),1);EXPECT_EQ(result->candidates(0).text(),"黯淡无光");
  ASSERT_TRUE(client.Observe(commit));ASSERT_TRUE(client.Observe(commit));
  predict::PersonalizationStore learning(root/L"user-data"/L"learning.db",true);EXPECT_EQ(learning.Frequency("昏暗"),1);
  auto exclusion=settings::Settings::Defaults();exclusion.excluded_app_hashes={"new-exclusion"};ASSERT_TRUE(settings.Save(exclusion));
  commit.text="排除后迟到";commit.commit_id=10;EXPECT_FALSE(client.Observe(commit));EXPECT_EQ(learning.Frequency(commit.text),0);
  result=client.Predict(commit);ASSERT_TRUE(result);EXPECT_EQ(result->candidates_size(),0);
  commit.policy_generation.clear();commit.text="无策略";EXPECT_FALSE(client.Observe(commit));EXPECT_EQ(learning.Frequency(commit.text),0);
  auto privacy=settings::Settings::Defaults();privacy.privacy_mode=true;ASSERT_TRUE(settings.Save(privacy));
  commit.text="受保护";commit.commit_id=10;EXPECT_FALSE(client.Observe(commit));EXPECT_EQ(learning.Frequency("受保护"),0);
  result=client.Predict(commit);ASSERT_TRUE(result);EXPECT_EQ(result->candidates_size(),0);
 }
 std::filesystem::remove_all(root);
}

TEST(PredictionDispatcher, FailedLearningIsRetriedWithSameCommitIdentity) {
 std::atomic<int> attempts{0};std::promise<void> delivered;auto ready=delivered.get_future();
 LearningDispatcher dispatcher([&](const ConfirmedCommit& commit){
  EXPECT_EQ(commit.session_id,"retry");EXPECT_EQ(commit.commit_id,42u);
  if(++attempts<3)return false;delivered.set_value();return true;
 });
 dispatcher.Submit({.text="灵气",.allow_learning=true,.session_id="retry",.commit_id=42});
 EXPECT_EQ(ready.wait_for(2s),std::future_status::ready);EXPECT_EQ(attempts,3);
}
}  // namespace
}  // namespace zhixu::input_host

namespace zhixu::input_host {
namespace {
TEST(PredictionRuntime, ColdEmptyOrUnavailableResultGetsOneBoundedRetry) {
  for(bool unavailable:{false,true}) {
    std::atomic<unsigned> calls=0;
    std::promise<unsigned> delivered;auto done=delivered.get_future();
    PredictionRuntime runtime([&](const ConfirmedCommit&)->std::optional<protocol::v1::PredictionResult>{
      const auto call=++calls;if(call==1 && unavailable)return std::nullopt;
      protocol::v1::PredictionResult result;result.set_final_result(true);
      if(call==2)result.add_candidates()->set_text("黄河之水天上来");return result;
    },[&](protocol::v1::PredictionResult result,std::uint64_t){delivered.set_value(result.candidates_size());},0ms);
    runtime.Submit({.text="君不见",.context_version=1,.allow_prediction=true});
    ASSERT_EQ(done.wait_for(1s),std::future_status::ready);EXPECT_EQ(done.get(),1u);EXPECT_EQ(calls.load(),2u);
  }
}
TEST(PredictionRuntime, InvalidationDuringEmptyLookupPreventsRetryAndPublication) {
  std::atomic<unsigned> calls=0,publications=0;
  std::promise<void> queried;auto done=queried.get_future();
  std::unique_ptr<PredictionRuntime> runtime;
  runtime=std::make_unique<PredictionRuntime>([&](const ConfirmedCommit&)->std::optional<protocol::v1::PredictionResult>{
    ++calls;runtime->Invalidate();queried.set_value();protocol::v1::PredictionResult result;result.set_final_result(true);return result;
  },[&](protocol::v1::PredictionResult,std::uint64_t){++publications;},0ms);
  runtime->Submit({.text="君不见",.context_version=1,.allow_prediction=true});
  ASSERT_EQ(done.wait_for(1s),std::future_status::ready);runtime.reset();EXPECT_EQ(calls.load(),1u);EXPECT_EQ(publications.load(),0u);
}
TEST(PredictionRuntime, RepeatedEmptyLookupStopsAfterTwoAttempts) {
  std::atomic<unsigned> calls=0;
  std::promise<unsigned> delivered;auto done=delivered.get_future();
  PredictionRuntime runtime([&](const ConfirmedCommit&)->std::optional<protocol::v1::PredictionResult>{++calls;protocol::v1::PredictionResult result;result.set_final_result(true);return result;},
    [&](protocol::v1::PredictionResult result,std::uint64_t){delivered.set_value(result.candidates_size());},0ms);
  runtime.Submit({.text="无匹配",.context_version=1,.allow_prediction=true});
  ASSERT_EQ(done.wait_for(1s),std::future_status::ready);EXPECT_EQ(done.get(),0u);EXPECT_EQ(calls.load(),2u);
}
}
}
