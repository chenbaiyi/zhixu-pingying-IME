#include "input_host/engine/rime_engine.h"
#include "input_host/session/session_controller.h"
#include "common/ipc/named_pipe.h"
#include "input_host/input_host_server.h"
#include "tsf/input_host_transport.h"

#include <gtest/gtest.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <crtdbg.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>
#include <thread>

namespace zhixu::input_host {
namespace {

void ExpectNiCandidate(const unsigned int first_key, const unsigned int second_key,
                       const char* label) {
  // Use a fresh profile: a previously deployed dictionary must not mask a
  // first-startup failure. Keep artifacts for diagnosis, never touch user data.
  const auto user_data = std::filesystem::temp_directory_path() /
      ("ZhixuIME-rime-test-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()) + "-" + label);
  std::filesystem::create_directories(user_data);
  SCOPED_TRACE(user_data.string());
  RimeEngine engine({.shared_data_directory = ZHIXU_RIME_TEST_DATA,
                     .user_data_directory = user_data});
  ASSERT_TRUE(engine.available());
  const auto first = engine.ProcessKey({.virtual_key = first_key, .key_down = true});
  const auto second = engine.ProcessKey({.virtual_key = second_key, .key_down = true});
  EXPECT_TRUE(first.consumed);
  EXPECT_TRUE(second.consumed);
  EXPECT_EQ(second.preedit, "ni");
  const std::string expected_candidate = "\xE4\xBD\xA0";  // UTF-8 U+4F60.
  EXPECT_NE(std::find(second.candidate_text.begin(), second.candidate_text.end(), expected_candidate),
            second.candidate_text.end());
}

TEST(RimeEngine, FreshProfileProducesNiCandidateFromLowercaseKeys) {
  ExpectNiCandidate('n', 'i', "lowercase");
}

TEST(RimeEngine, FreshProfileProducesNiCandidateFromWindowsVirtualKeys) {
  ExpectNiCandidate('N', 'I', "virtual-keys");
}

TEST(RimeEngine, SessionSpaceCommitsNiCandidateFromWindowsVirtualKeys) {
  const auto user_data = std::filesystem::temp_directory_path() /
      ("ZhixuIME-rime-test-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()) + "-space-commit");
  std::filesystem::create_directories(user_data);
  SCOPED_TRACE(user_data.string());
  RimeEngine engine({.shared_data_directory = ZHIXU_RIME_TEST_DATA,
                     .user_data_directory = user_data});
  ASSERT_TRUE(engine.available());
  SessionController session(engine);
  const auto key = [](std::uint32_t code) {
    protocol::v1::KeyEvent event;
    event.set_key_code(code);
    event.set_key_down(true);
    event.mutable_privacy()->set_allow_composition(true);
    return event;
  };
  EXPECT_TRUE(session.Handle(key('N'), 0).consumed());
  const auto composition = session.Handle(key('I'), 1);
  ASSERT_GT(composition.composition().candidates_size(), 0);
  const auto selected = session.Handle(key(VK_SPACE), 2);
  EXPECT_TRUE(selected.consumed());
  ASSERT_TRUE(selected.has_commit_request());
  EXPECT_EQ(selected.commit_request().text(), "\xE4\xBD\xA0");
  EXPECT_EQ(selected.commit_request().expected_context_version(), 2U);
  EXPECT_TRUE(selected.composition().preedit().empty());
}

void ExpectEditingKeysClearComposition(const std::initializer_list<std::uint32_t> keys,
                                       const char* label) {
  const auto user_data = std::filesystem::temp_directory_path() /
      ("ZhixuIME-rime-test-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()) + "-" + label);
  std::filesystem::create_directories(user_data);
  SCOPED_TRACE(user_data.string());
  RimeEngine engine({.shared_data_directory = ZHIXU_RIME_TEST_DATA,
                     .user_data_directory = user_data});
  ASSERT_TRUE(engine.available());
  engine.ProcessKey({.virtual_key = 'N', .key_down = true});
  ASSERT_EQ(engine.ProcessKey({.virtual_key = 'I', .key_down = true}).preedit, "ni");
  EngineState state;
  for (const auto key : keys) {
    state = engine.ProcessKey({.virtual_key = key, .key_down = true});
    EXPECT_TRUE(state.consumed);
  }
  EXPECT_TRUE(state.preedit.empty());
  EXPECT_TRUE(state.candidate_text.empty());
  engine.ProcessKey({.virtual_key = 'N', .key_down = true});
  EXPECT_EQ(engine.ProcessKey({.virtual_key = 'I', .key_down = true}).preedit, "ni");
}

TEST(RimeEngine, WindowsBackspaceRemovesCompositionWithoutRestoringOldNi) {
  ExpectEditingKeysClearComposition({VK_BACK, VK_BACK}, "backspace");
}

TEST(RimeEngine, WindowsEscapeCancelsCompositionWithoutRestoringOldNi) {
  ExpectEditingKeysClearComposition({VK_ESCAPE}, "escape");
}

TEST(RimeEngine, WindowsDeleteRemovesCharacterAtCompositionCaret) {
  const auto user_data = std::filesystem::temp_directory_path() /
      ("ZhixuIME-rime-test-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()) + "-delete");
  std::filesystem::create_directories(user_data);
  RimeEngine engine({.shared_data_directory = ZHIXU_RIME_TEST_DATA,
                     .user_data_directory = user_data});
  ASSERT_TRUE(engine.available());
  engine.ProcessKey({.virtual_key = 'N', .key_down = true});
  engine.ProcessKey({.virtual_key = 'I', .key_down = true});
  EXPECT_TRUE(engine.ProcessKey({.virtual_key = VK_LEFT, .key_down = true}).consumed);
  const auto deleted = engine.ProcessKey({.virtual_key = VK_DELETE, .key_down = true});
  EXPECT_TRUE(deleted.consumed);
  EXPECT_EQ(deleted.preedit, "n");
}

TEST(RimeEngine, SessionResetDiscardsOldCompositionBeforeNewNi) {
  const auto user_data = std::filesystem::temp_directory_path() /
      ("ZhixuIME-rime-test-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(GetTickCount64()) + "-reset");
  std::filesystem::create_directories(user_data);
  SCOPED_TRACE(user_data.string());
  RimeEngine engine({.shared_data_directory = ZHIXU_RIME_TEST_DATA,
                     .user_data_directory = user_data});
  ASSERT_TRUE(engine.available());
  SessionController session(engine);
  const auto key = [](std::uint32_t code) {
    protocol::v1::KeyEvent event;
    event.set_key_code(code);
    event.set_key_down(true);
    event.mutable_privacy()->set_allow_composition(true);
    return event;
  };
  session.Handle(key('N'));
  ASSERT_EQ(session.Handle(key('I')).composition().preedit(), "ni");
  session.Reset();
  session.Handle(key('N'));
  EXPECT_EQ(session.Handle(key('I')).composition().preedit(), "ni");
}

// Exercise the distributable data through the real adapter. These assertions
// catch accidentally packaging the synthetic dictionary and page-local index
// mistakes; no test talks to the installed input method or its user profile.
std::atomic_uint deployment_assertions{0};
int CountDeploymentAssertions(const int report_type, char*, int* return_value) {
  if (report_type != _CRT_ASSERT) return FALSE;
  ++deployment_assertions;
  *return_value = 0;  // Report as a test failure rather than a modal debugger dialog.
  return TRUE;
}
int CountWideDeploymentAssertions(const int report_type, wchar_t*, int* return_value) {
  return CountDeploymentAssertions(report_type, nullptr, return_value);
}

class FullPinyin : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    wchar_t precompiled[32768]{};
    const DWORD cached_length = GetEnvironmentVariableW(L"ZHIXU_RIME_TEST_PRECOMPILED", precompiled, 32768);
    if (cached_length > 0 && cached_length < 32768) compiled_profile_ = precompiled;
    else compiled_profile_ = std::filesystem::temp_directory_path() /
        ("ZhixuIME-compiled-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(compiled_profile_);
    const auto started = std::chrono::steady_clock::now();
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, CountDeploymentAssertions);
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, CountWideDeploymentAssertions);
    {
      RimeEngine seed({.shared_data_directory = ZHIXU_RIME_FULL_DATA,
                       .user_data_directory = compiled_profile_, .schema_id = "zhixu_pinyin"});
      ASSERT_TRUE(seed.available());
    }
    _CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, CountDeploymentAssertions);
    _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE, CountWideDeploymentAssertions);
    std::cout << "Full dictionary deployment ms (" << (cached_length > 0 && cached_length < 32768 ? "cached" : "cold") << "): " <<
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << std::endl;
  }
 protected:
  inline static std::filesystem::path compiled_profile_;
  void SetUp() override {
    deployment_assertions = 0;
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, CountDeploymentAssertions);
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, CountWideDeploymentAssertions);
    user_data_ = std::filesystem::temp_directory_path() /
        ("ZhixuIME-full-pinyin-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()) + "-" +
         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(user_data_);
    // Copy compiled assets only. Every test still has a fresh learning database.
    std::filesystem::copy(compiled_profile_ / "build", user_data_ / "build",
                          std::filesystem::copy_options::recursive);
    const auto warm_started = std::chrono::steady_clock::now();
    engine_ = std::make_unique<RimeEngine>(RimeEngineConfig{
        .shared_data_directory = ZHIXU_RIME_FULL_DATA,
        .user_data_directory = user_data_,
        .schema_id = "zhixu_pinyin"});
    ASSERT_TRUE(engine_->available());
    std::cout << "Full dictionary warm deployment ms: " <<
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - warm_started).count() << std::endl;
  }

  void TearDown() override {
    engine_.reset();
    _CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, CountDeploymentAssertions);
    _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE, CountWideDeploymentAssertions);
    EXPECT_EQ(deployment_assertions.load(), 0U) << "UTF-8 schema deployment must not assert in MSVC CRT";
    std::error_code cleanup_error;
    if (!HasFailure() && std::filesystem::equivalent(user_data_.parent_path(), std::filesystem::temp_directory_path(), cleanup_error) && !cleanup_error &&
        user_data_.filename().wstring().starts_with(L"ZhixuIME-full-pinyin-")) {
      std::error_code ignored; std::filesystem::remove_all(user_data_, ignored);
    }
  }

  EngineState Type(const std::string& pinyin) {
    EngineState state;
    for (const auto key : pinyin) {
      state = engine_->ProcessKey({.virtual_key = (key == '\'' ? VK_OEM_7 : static_cast<std::uint32_t>(key)), .key_down = true});
    }
    return state;
  }

  std::filesystem::path user_data_;
  std::unique_ptr<RimeEngine> engine_;
};

protocol::v1::KeyEvent EditingKey(std::uint32_t code, std::uint32_t modifiers = 0,
                                 bool down = true) {
  protocol::v1::KeyEvent event;
  event.set_key_code(code);
  event.set_modifiers(modifiers);
  event.set_key_down(down);
  event.mutable_privacy()->set_allow_composition(true);
  return event;
}

// These regressions exercise the shipped schema, not a synthetic abbreviation table.
// Removing its abbrev/fuzz/correction rules must break the corresponding cases.


TEST_F(FullPinyin, JiangnanCandidatesStayVisibleAfterOwnPreeditCaretUpdate) {
  class Presenter final : public ui::CandidatePresenter {
   public:
    void Present(ui::CandidatePresentation value) override { presentation=std::move(value);visible=true; }
    void Hide() noexcept override { visible=false; }
    bool visible=false;
    std::optional<ui::CandidatePresentation> presentation;
  } presenter;
  InputHostServer host(*engine_, &presenter);
  const auto role=L"jiangnan-preedit-caret-test";
  std::jthread server([&](std::stop_token stop){static_cast<void>(host.Serve(role,stop));});
  const auto endpoint=ipc::PipeEndpoint::ForCurrentUser(role);ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(*endpoint,std::chrono::seconds(1)));
  std::uint64_t id=0;
  auto call=[&](protocol::v1::MessageType type,const auto& message){
    protocol::Envelope request;request.set_protocol_version(protocol::kProtocolVersion);
    request.set_session_id("jiangnan-preedit");request.set_request_id(++id);
    request.set_message_type(type);message.SerializeToString(request.mutable_payload());
    return client.Call(request,std::chrono::seconds(1));
  };
  protocol::v1::Hello hello;hello.set_min_version(1);hello.set_max_version(1);
  ASSERT_TRUE(call(protocol::v1::HELLO,hello));
  protocol::v1::CaretUpdate caret;
  caret.mutable_caret()->set_left(100);caret.mutable_caret()->set_right(102);
  caret.mutable_caret()->set_top(200);caret.mutable_caret()->set_bottom(220);
  caret.mutable_work_area()->set_right(1920);caret.mutable_work_area()->set_bottom(1080);
  caret.set_dpi(96);ASSERT_TRUE(call(protocol::v1::CARET_UPDATE,caret));
  protocol::v1::EngineAction action;
  int x=100;
  for(char c:std::string("jiangnan")){
    protocol::v1::KeyEvent key;key.set_key_code(static_cast<unsigned char>(c));
    key.set_key_symbol(static_cast<unsigned char>(c));key.set_key_down(true);
    key.mutable_privacy()->set_allow_composition(true);key.mutable_privacy()->set_allow_prediction(true);
    const auto response=call(protocol::v1::KEY_EVENT,key);ASSERT_TRUE(response);
    ASSERT_TRUE(action.ParseFromString(response->payload()));
    ASSERT_GT(action.composition().candidates_size(),0);
    // TSF applies this preedit to the host, then reports its new insertion caret at the same version.
    x+=10;caret.mutable_caret()->set_left(x);caret.mutable_caret()->set_right(x+2);
    ASSERT_TRUE(call(protocol::v1::CARET_UPDATE,caret));
    EXPECT_TRUE(presenter.visible) << "candidate window disappeared after own edit for key " << c;
    ASSERT_TRUE(presenter.presentation);
    EXPECT_EQ(presenter.presentation->caret.left,x) << "window must track the post-edit caret";
  }
  EXPECT_EQ(action.composition().candidates(0).text(),"江南");
  client.Close();server.request_stop();
}

TEST_F(FullPinyin, JiangnanHasOrdinaryWordCandidates) {
  const auto state = Type("jiangnan");
  ASSERT_TRUE(state.consumed);
  ASSERT_FALSE(state.candidate_text.empty());
  EXPECT_NE(std::find(state.candidate_text.begin(), state.candidate_text.end(), "江南"),
            state.candidate_text.end());
  std::cout << "JIANGNAN_PREEDIT=" << state.preedit << std::endl;
  for (const auto& candidate : state.candidate_text) {
    std::cout << "JIANGNAN_CANDIDATE=" << candidate << std::endl;
  }
}

TEST_F(FullPinyin, AbbreviationsAndMixedSpellingFindWordsWithinFourPages) {
  for (const auto& [input, expected] :
       {std::pair{"tyc", "同义词"}, {"tongyc", "同义词"},
        {"zg", "中国"}, {"zhg", "中国"}, {"zhongg", "中国"}}) {
    SCOPED_TRACE(input);
    auto state = Type(input);
    bool found = false;
    for (int page = 0; page < 4; ++page) {
      ASSERT_LE(state.candidate_text.size(), state.page == 0 ? 8U : 30U);
      if (std::find(state.candidate_text.begin(), state.candidate_text.end(), expected) !=
          state.candidate_text.end()) {
        const auto index = std::find(state.candidate_text.begin(), state.candidate_text.end(), expected) -
                           state.candidate_text.begin();
        const auto selected = engine_->SelectCandidate(static_cast<std::size_t>(index));
        ASSERT_TRUE(selected.commit_text.has_value());
        EXPECT_EQ(*selected.commit_text, expected);
        EXPECT_TRUE(selected.state.preedit.empty()) << "Word must consume the complete input";
        found = true;
        break;
      }
      if (!state.has_next_page) break;
      state = engine_->ProcessKey({.virtual_key = VK_NEXT, .key_down = true});
    }
    EXPECT_TRUE(found) << "Expected word absent from first 32 candidates";
    engine_->ResetComposition();
  }
}

TEST_F(FullPinyin, CommonFuzzySoundsFindWordsWithinFourPages) {
  for (const auto& [input, expected] :
       {std::pair{"zongguo", "中国"}, {"tongyichi", "同义词"},
        {"sanghai", "上海"}, {"shanhai", "上海"},
        {"shengqing", "申请"}, {"shangbang", "上班"}, {"jingtian", "今天"}}) {
    SCOPED_TRACE(input);
    auto state = Type(input);
    bool found = false;
    for (int page = 0; page < 4; ++page) {
      if (std::find(state.candidate_text.begin(), state.candidate_text.end(), expected) !=
          state.candidate_text.end()) {
        const auto index = std::find(state.candidate_text.begin(), state.candidate_text.end(), expected) -
                           state.candidate_text.begin();
        const auto selected = engine_->SelectCandidate(static_cast<std::size_t>(index));
        ASSERT_TRUE(selected.commit_text.has_value());
        EXPECT_EQ(*selected.commit_text, expected);
        EXPECT_TRUE(selected.state.preedit.empty()) << "Word must consume the complete input";
        found = true;
        break;
      }
      if (!state.has_next_page) break;
      state = engine_->ProcessKey({.virtual_key = VK_NEXT, .key_down = true});
    }
    EXPECT_TRUE(found) << "Expected fuzzy word absent from first 32 candidates";
    engine_->ResetComposition();
  }
}

TEST_F(FullPinyin, CommonMisspellingsFindWordsWithinFourPages) {
  for (const auto& [input, expected] :
       {std::pair{"hzi", "只"}, {"zih", "只"}, {"zohngguo", "中国"},
        {"dagn", "当"}, {"niaho", "你好"}, {"shaghai", "上海"},
        {"zhon'guo", "中国"}, {"shangahi", "上海"}}) {
    SCOPED_TRACE(input);
    auto state = Type(input);
    bool found = false;
    for (int page = 0; page < 4; ++page) {
      if (std::find(state.candidate_text.begin(), state.candidate_text.end(), expected) !=
          state.candidate_text.end()) {
        const auto index = std::find(state.candidate_text.begin(), state.candidate_text.end(), expected) -
                           state.candidate_text.begin();
        const auto selected = engine_->SelectCandidate(static_cast<std::size_t>(index));
        ASSERT_TRUE(selected.commit_text.has_value());
        EXPECT_EQ(*selected.commit_text, expected);
        EXPECT_TRUE(selected.state.preedit.empty()) << "Correction must consume the complete input";
        found = true;
        break;
      }
      if (!state.has_next_page) break;
      state = engine_->ProcessKey({.virtual_key = VK_NEXT, .key_down = true});
    }
    EXPECT_TRUE(found) << "Expected corrected word absent from first 32 candidates";
    engine_->ResetComposition();
  }
}

TEST_F(FullPinyin, AdjacentLetterTranspositionsFindWordsWithinFourPages) {
  for (const auto& [input, expected] :
       {std::pair{"ubyao", "不要"}, {"bingubshi", "并不是"}, {"bing'ub'shi", "并不是"},
        {"fna", "翻"}, {"zhunag", "装"}, {"hzongguo", "中国"},
        {"zhognguo", "中国"}, {"zhuagn", "装"}, {"zuhang", "装"}, {"zhaung", "装"}}) {
    SCOPED_TRACE(input);
    auto state = Type(input);
    bool found = false;
    for (int page = 0; page < 4; ++page) {
      const auto candidate = std::find(state.candidate_text.begin(), state.candidate_text.end(), expected);
      if (candidate != state.candidate_text.end()) {
        std::cout << "TRANSPOSE_RANK " << input << " => " << expected << " rank="
                  << (page == 0 ? 0 : 8 + (page - 1) * 30) + (candidate - state.candidate_text.begin()) + 1 << std::endl;
        const auto selected = engine_->SelectCandidate(
            static_cast<std::size_t>(candidate - state.candidate_text.begin()));
        ASSERT_TRUE(selected.commit_text.has_value());
        EXPECT_EQ(*selected.commit_text, expected);
        EXPECT_TRUE(selected.state.preedit.empty()) << "Correction must consume the complete input";
        found = true;
        break;
      }
      if (!state.has_next_page) break;
      state = engine_->ProcessKey({.virtual_key = VK_NEXT, .key_down = true});
    }
    EXPECT_TRUE(found) << "Expected adjacent-transposition correction within first 32 candidates";
    engine_->ResetComposition();
  }
}

TEST_F(FullPinyin, ExactSpellingAndExplicitSeparatorsKeepTheirFirstChoices) {
  for (const auto& [input, expected] :
       {std::pair{"nihao", "你好"}, {"zhongguo", "中国"}, {"shanghai", "上海"},
        {"tongyici", "同义词"}, {"xi'ai", "喜爱"}, {"xia", "下"},
        {"bingbushi", "并不是"}, {"buyao", "不要"}, {"nashi", "那是"}, {"tian", "天"},
        {"zhege", "这个"}}) {
    SCOPED_TRACE(input);
    const auto state = Type(input);
    ASSERT_FALSE(state.candidate_text.empty());
    EXPECT_EQ(state.candidate_text.front(), expected);
    engine_->ResetComposition();
  }
}

TEST_F(FullPinyin, TranspositionRulesDoNotCascadeWithinASyllable) {
  auto state = Type("hzunag");  // zhuang with both zh and an swapped.
  for (int page = 0; page < 4; ++page) {
    EXPECT_EQ(std::count(state.candidate_text.begin(), state.candidate_text.end(), "装"), 0);
    if (!state.has_next_page) break;
    state = engine_->ProcessKey({.virtual_key = VK_NEXT, .key_down = true});
  }
}

TEST_F(FullPinyin, SpellingLatencyAndFirstChoicesAreMeasuredOnAFreshProfile) {
  std::vector<long long> samples;
  for (int repetition = 0; repetition < 10; ++repetition) {
    for (const auto* input : {"nihao", "zhongguo", "shanghai", "tongyici", "xi'ai",
                             "weixin", "saomazhifu", "womenyiqiqugongyuan"}) {
      engine_->ResetComposition();
      EngineState state;
      for (const auto character : std::string(input)) {
        const auto started = std::chrono::steady_clock::now();
        state = engine_->ProcessKey({
            .virtual_key = character == '\'' ? VK_OEM_7 : static_cast<std::uint32_t>(character),
            .key_down = true});
        samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
        ASSERT_TRUE(state.consumed);
      }
      if (repetition == 0) {
        ASSERT_FALSE(state.candidate_text.empty());
        std::cout << "SPELLING_BASELINE " << input << " => " << state.candidate_text.front() << std::endl;
      }
    }
  }
  ASSERT_FALSE(samples.empty());
  std::sort(samples.begin(), samples.end());
  std::cout << "SPELLING_LATENCY_US count=" << samples.size()
            << " p50=" << samples[samples.size() / 2]
            << " p95=" << samples[samples.size() * 95 / 100]
            << " max=" << samples.back() << std::endl;
}

TEST_F(FullPinyin, InvalidContinuousSpellingKeepsEveryKeyWithoutAutomaticCommit) {
  std::string expected;
  for (const auto key : std::string("bingubshi")) {
    expected.push_back(key);
    const auto result = engine_->ProcessKeyAction({
        .virtual_key = static_cast<std::uint32_t>(key - 'a' + 'A'),
        .key_down = true, .key_symbol = static_cast<std::uint32_t>(key)});
    std::string raw = result.state.preedit;
    raw.erase(std::remove(raw.begin(), raw.end(), ' '), raw.end());
    std::cout << "INVALID_INPUT prefix=" << expected << " preedit=" << result.state.preedit
              << " cursor=" << result.state.cursor.value_or(0) << " consumed=" << result.state.consumed << std::endl;
    EXPECT_TRUE(result.state.consumed);
    EXPECT_FALSE(result.commit_text.has_value());
    EXPECT_EQ(raw, expected);
  }
}

// Same real engine and server as production; unique pipe, no foreground app or live IME.
// Exercise the default TSF key budget, not a permissive test-only timeout.
TEST_F(FullPinyin, FuzzyPipePreservesInvalidInputThroughDefaultTsfKeyDeadline) {
  InputHostServer host(*engine_);
  const auto role = L"invalid-spelling-" + std::to_wstring(GetCurrentProcessId());
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  ASSERT_TRUE(endpoint.has_value());
  std::jthread worker([&](std::stop_token stop) { EXPECT_TRUE(host.Serve(role, stop)); });
  tsf::InputHostTransport transport(role);
  bool connected = false;
  for (int attempt = 0; attempt < 50 && !connected; ++attempt) {
    connected = transport.Connect(std::chrono::seconds(1));
    if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  tsf::KeyEventSink keys(&transport);
  std::string expected;
  if (connected) {
    for (const auto key : std::string("bingubshi")) {
      expected.push_back(key);
      const auto started = std::chrono::steady_clock::now();
      const auto action = keys.Handle({.virtual_key = static_cast<WPARAM>(key - 'a' + 'A'),
          .key_symbol = static_cast<std::uint32_t>(key)});
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started).count();
      std::cout << "INVALID_PIPE prefix=" << expected << " action=" << action.has_value()
                << " us=" << elapsed << std::endl;
      EXPECT_TRUE(action.has_value()) << expected;
      if (!action) continue;
      EXPECT_TRUE(action->consumed());
      EXPECT_FALSE(action->has_commit_request());
      auto raw = action->composition().preedit();
      raw.erase(std::remove(raw.begin(), raw.end(), ' '), raw.end());
      EXPECT_EQ(raw, expected);
    }
  }
  transport.Disconnect();
  worker.request_stop();
  worker.join();
  EXPECT_TRUE(connected);
}

TEST_F(FullPinyin, ReturnCommitsRawPinyinThroughSession) {
  SessionController session(*engine_);
  session.Handle(EditingKey('N'));
  session.Handle(EditingKey('I'));
  const auto action = session.Handle(EditingKey(VK_RETURN));
  ASSERT_TRUE(action.has_commit_request());
  EXPECT_TRUE(action.consumed());
  EXPECT_EQ(action.commit_request().text(), "ni");
  EXPECT_TRUE(action.composition().preedit().empty());
}

TEST_F(FullPinyin, ChineseCommaCommitsCandidateAndPunctuationTogether) {
  SessionController session(*engine_);
  session.Handle(EditingKey('N'));
  session.Handle(EditingKey('I'));
  const auto action = session.Handle(EditingKey(VK_OEM_COMMA));
  ASSERT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().text(), "你，");
  EXPECT_TRUE(action.composition().preedit().empty());
}

TEST_F(FullPinyin, ChineseCommaAfterCommittedChineseDoesNotAssert) {
  ASSERT_FALSE(Type("ni").candidate_text.empty());
  const auto selected = engine_->SelectCandidate(0);
  ASSERT_TRUE(selected.commit_text.has_value());
  ASSERT_EQ(*selected.commit_text, "\xE4\xBD\xA0");
  const auto comma = engine_->ProcessKeyAction({.virtual_key = VK_OEM_COMMA, .key_down = true});
  EXPECT_TRUE(comma.state.consumed);
  ASSERT_TRUE(comma.commit_text.has_value());
  EXPECT_EQ(*comma.commit_text, "\xEF\xBC\x8C");
  EXPECT_EQ(deployment_assertions.load(), 0U);
}

TEST_F(FullPinyin, BackspaceAfterPartialSelectionReopensSafely) {
  // A buggy STL iterator check may terminate this test process. Do not spawn
  // a second modal error window while reproducing the already captured crash.
  struct ErrorReportingRestore {
    UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    unsigned int behavior = _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ~ErrorReportingRestore() {
      SetErrorMode(mode);
      _set_abort_behavior(behavior, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
  } reporting;
  const auto initial = Type("nishurufa");
  const std::string ni = "\xE4\xBD\xA0";
  const auto candidate = std::find(initial.candidate_text.begin(), initial.candidate_text.end(), ni);
  ASSERT_NE(candidate, initial.candidate_text.end()) << "Needs a prefix-only candidate, not a committed sentence";
  const auto partial = engine_->SelectCandidate(
      static_cast<std::size_t>(std::distance(initial.candidate_text.begin(), candidate)));
  ASSERT_TRUE(partial.state.consumed);
  ASSERT_FALSE(partial.commit_text.has_value());
  ASSERT_NE(partial.state.preedit.find(ni), std::string::npos);
  const auto reverted = engine_->ProcessKeyAction({.virtual_key = VK_BACK, .key_down = true});
  EXPECT_TRUE(reverted.state.consumed);
  EXPECT_FALSE(reverted.commit_text.has_value());
  EXPECT_EQ(reverted.state.preedit.find(ni), std::string::npos);
  EXPECT_FALSE(reverted.state.candidate_text.empty());
  EXPECT_EQ(deployment_assertions.load(), 0U);
}

TEST_F(FullPinyin, BackspaceAfterMultiplePartialSelectionsKeepsEarlierSelection) {
  const std::string ni = "\xE4\xBD\xA0";
  const std::string input_method = "\xE8\xBE\x93\xE5\x85\xA5\xE6\xB3\x95";
  auto state = Type("nishurufani");
  for (const auto& prefix : {ni, input_method}) {
    auto candidate = std::find(state.candidate_text.begin(), state.candidate_text.end(), prefix);
    for (int page = 0; candidate == state.candidate_text.end() && state.has_next_page && page < 8; ++page) {
      state = engine_->ProcessKey({.virtual_key = VK_NEXT, .key_down = true});
      candidate = std::find(state.candidate_text.begin(), state.candidate_text.end(), prefix);
    }
    ASSERT_NE(candidate, state.candidate_text.end());
    auto partial = engine_->SelectCandidate(
        static_cast<std::size_t>(std::distance(state.candidate_text.begin(), candidate)));
    ASSERT_TRUE(partial.state.consumed);
    ASSERT_FALSE(partial.commit_text.has_value());
    state = std::move(partial.state);
  }
  ASSERT_NE(state.preedit.find(ni), std::string::npos);
  ASSERT_NE(state.preedit.find(input_method), std::string::npos);
  const auto reverted = engine_->ProcessKeyAction({.virtual_key = VK_BACK, .key_down = true});
  EXPECT_TRUE(reverted.state.consumed);
  EXPECT_FALSE(reverted.commit_text.has_value());
  EXPECT_NE(reverted.state.preedit.find(ni), std::string::npos);
  EXPECT_EQ(reverted.state.preedit.find(input_method), std::string::npos);
  EXPECT_FALSE(reverted.state.candidate_text.empty());
  EXPECT_EQ(deployment_assertions.load(), 0U);
}

TEST_F(FullPinyin, DigitSeparatorPunctuationAfterChineseWordsIsSafe) {
  struct Punctuation { std::uint32_t key; std::uint32_t modifiers; const char* expected; };
  const Punctuation punctuation[] = {
      {VK_OEM_COMMA, 0, "\xEF\xBC\x8C"},
      {VK_OEM_PERIOD, 0, "\xE3\x80\x82"},
      {VK_OEM_1, 4, "\xEF\xBC\x9A"}};
  for (const auto pinyin : {"ni", "hao", "shurufa"}) {
    for (const auto& item : punctuation) {
      SCOPED_TRACE(pinyin);
      SCOPED_TRACE(item.key);
      ASSERT_FALSE(Type(pinyin).candidate_text.empty());
      ASSERT_TRUE(engine_->SelectCandidate(0).commit_text.has_value());
      const auto result = engine_->ProcessKeyAction(
          {.virtual_key = item.key, .key_down = true, .modifiers = item.modifiers});
      EXPECT_TRUE(result.state.consumed);
      ASSERT_TRUE(result.commit_text.has_value());
      EXPECT_EQ(*result.commit_text, item.expected);
      EXPECT_EQ(deployment_assertions.load(), 0U);
    }
  }
}

TEST_F(FullPinyin, ShiftTapSwitchesModeAndResetRetainsIt) {
  SessionController session(*engine_);
  session.Handle(EditingKey(VK_SHIFT, 4));
  EXPECT_TRUE(session.Handle(EditingKey(VK_SHIFT, 0, false)).composition().ascii_mode());
  session.Reset();
  const auto letter = session.Handle(EditingKey('N'));
  EXPECT_TRUE(letter.composition().ascii_mode());
  EXPECT_FALSE(letter.consumed());
  EXPECT_TRUE(letter.composition().preedit().empty());
}

TEST_F(FullPinyin, CompositionCursorFollowsLeftAndRight) {
  SessionController session(*engine_);
  session.Handle(EditingKey('N'));
  EXPECT_EQ(session.Handle(EditingKey('I')).composition().cursor(), 2U);
  EXPECT_EQ(session.Handle(EditingKey(VK_LEFT)).composition().cursor(), 1U);
  EXPECT_EQ(session.Handle(EditingKey(VK_RIGHT)).composition().cursor(), 2U);
}

TEST_F(FullPinyin, HomeAndEndMoveCompositionCaretRatherThanCandidatePage) {
  SessionController session(*engine_);
  static_cast<void>(session.Handle(EditingKey('N')));
  static_cast<void>(session.Handle(EditingKey('I')));
  static_cast<void>(session.Handle(EditingKey(VK_DOWN)));
  EXPECT_EQ(session.Handle(EditingKey(VK_HOME)).composition().cursor(), 0U);
  EXPECT_EQ(session.Handle(EditingKey(VK_END)).composition().cursor(), 2U);
}

TEST_F(FullPinyin, SpaceCommitsCandidateHighlightedByDownArrow) {
  SessionController session(*engine_);
  static_cast<void>(session.Handle(EditingKey('N')));
  const auto initial = session.Handle(EditingKey('I'));
  ASSERT_GT(initial.composition().candidates_size(), 1);
  const auto expected = initial.composition().candidates(1).text();
  static_cast<void>(session.Handle(EditingKey(VK_DOWN)));
  const auto chosen = session.Handle(EditingKey(VK_SPACE));
  ASSERT_TRUE(chosen.has_commit_request());
  EXPECT_EQ(chosen.commit_request().text(), expected);
}

TEST_F(FullPinyin, ShiftLettersCommitOldCompositionWithoutLosingUppercase) {
  SessionController session(*engine_);
  static_cast<void>(session.Handle(EditingKey('N')));
  static_cast<void>(session.Handle(EditingKey('I')));
  const auto action = session.Handle(EditingKey('A', 4));
  ASSERT_TRUE(action.has_commit_request());
  EXPECT_TRUE(action.consumed());
  EXPECT_EQ(action.commit_request().text(), "你A");
  EXPECT_TRUE(action.composition().preedit().empty());
}

TEST_F(FullPinyin, CapsLockKeepsNativeEnglishCaseAndCanReturnToChinese) {
  SessionController session(*engine_);
  EXPECT_TRUE(session.Handle(EditingKey(VK_CAPITAL, 8)).composition().ascii_mode());
  EXPECT_FALSE(session.Handle(EditingKey('N', 8)).consumed());
  EXPECT_FALSE(session.Handle(EditingKey('N', 8 | 4)).consumed());
  EXPECT_FALSE(session.Handle(EditingKey(VK_CAPITAL)).composition().ascii_mode());
  EXPECT_EQ(session.Handle(EditingKey('N')).composition().preedit(), "n");
}

TEST_F(FullPinyin, PunctuationToggleAndShiftPunctuationReachCommitPath) {
  SessionController session(*engine_);
  EXPECT_TRUE(session.Handle(EditingKey(VK_OEM_PERIOD, 2)).composition().ascii_punctuation());
  EXPECT_FALSE(session.Handle(EditingKey(VK_OEM_COMMA)).consumed());
  EXPECT_FALSE(session.Handle(EditingKey(VK_OEM_PERIOD, 2)).composition().ascii_punctuation());
  const auto question = session.Handle(EditingKey(VK_OEM_2, 4));
  ASSERT_TRUE(question.has_commit_request());
  EXPECT_EQ(question.commit_request().text(), "？");
}

TEST_F(FullPinyin, DecimalSeparatorStaysAsciiAfterNativeDigits) {
  SessionController session(*engine_);
  EXPECT_FALSE(session.Handle(EditingKey('1')).consumed());
  const auto separator = session.Handle(EditingKey(VK_OEM_PERIOD));
  ASSERT_TRUE(separator.has_commit_request());
  EXPECT_EQ(separator.commit_request().text(), ".");
}

TEST_F(FullPinyin, ShiftSpaceTogglesWidthAndApplicationShortcutsDoNotChangeInput) {
  SessionController session(*engine_);
  EXPECT_TRUE(session.Handle(EditingKey(VK_SPACE, 4)).composition().full_shape());
  EXPECT_FALSE(session.Handle(EditingKey(VK_SPACE, 4)).composition().full_shape());
  static_cast<void>(session.Handle(EditingKey('N')));
  for (const auto key : {'A', 'C', 'V', 'X', 'Z', 'F'}) {
    const auto action = session.Handle(EditingKey(key, 2));
    EXPECT_FALSE(action.consumed());
    EXPECT_EQ(action.composition().preedit(), "n");
    EXPECT_FALSE(action.has_commit_request());
  }
}

TEST_F(FullPinyin, FreshProfileHasCommonCharactersAndPhrases) {
  for (const auto& [pinyin, expected] :
       {std::pair{"ni", "你"}, {"hao", "好"}, {"zhong", "中"},
        {"guo", "国"}, {"nihao", "你好"}, {"shurufa", "输入法"},
        {"jisuanji", "计算机"}, {"chongqing", "重庆"},
        {"zhonghuarenmingongheguo", "中华人民共和国"}}) {
    SCOPED_TRACE(pinyin);
    const auto state = Type(pinyin);
    ASSERT_TRUE(state.consumed);
    EXPECT_NE(std::find(state.candidate_text.begin(), state.candidate_text.end(), expected),
              state.candidate_text.end());
    engine_->ResetComposition();
  }
}

TEST_F(FullPinyin, BuildsAndCommitsASentenceAbsentFromTheDictionary) {
  const auto state = Type("womenyiqiqugongyuan");
  ASSERT_FALSE(state.candidate_text.empty());
  EXPECT_EQ(state.candidate_text.front(), "我们一起去公园");
  const auto selected = engine_->SelectCandidate(0);
  ASSERT_TRUE(selected.commit_text.has_value());
  EXPECT_EQ(*selected.commit_text, "我们一起去公园");
  EXPECT_TRUE(selected.state.preedit.empty());
}

TEST_F(FullPinyin, CommitsALongerSentenceFromContinuousPinyin) {
  const auto state = Type("womenjintianyiqiqugongyuanranhouhuijiachifan");
  ASSERT_FALSE(state.candidate_text.empty());
  EXPECT_EQ(state.candidate_text.front(), "我们今天一起去公园然后回家吃饭");
  const auto selected = engine_->SelectCandidate(0);
  ASSERT_TRUE(selected.commit_text.has_value());
  EXPECT_EQ(*selected.commit_text, "我们今天一起去公园然后回家吃饭");
}

TEST_F(FullPinyin, UpgradedProfileLoadsBundledFullPinyin) {
  engine_.reset();
  const auto upgrade_profile = user_data_ / "upgrade";
  std::filesystem::create_directories(upgrade_profile);
  {
    RimeEngine old_engine({.shared_data_directory = ZHIXU_RIME_TEST_DATA,
                          .user_data_directory = upgrade_profile});
    ASSERT_TRUE(old_engine.available());
    old_engine.ProcessKey({.virtual_key = 'N', .key_down = true});
    ASSERT_FALSE(old_engine.ProcessKey({.virtual_key = 'I', .key_down = true}).candidate_text.empty());
  }
  engine_ = std::make_unique<RimeEngine>(RimeEngineConfig{
      .shared_data_directory = ZHIXU_RIME_FULL_DATA,
      .user_data_directory = upgrade_profile,
      .schema_id = "zhixu_pinyin"});
  ASSERT_TRUE(engine_->available());
  const auto state = Type("shurufa");
  ASSERT_FALSE(state.candidate_text.empty());
  EXPECT_EQ(state.candidate_text.front(), "输入法");
}

TEST_F(FullPinyin, WindowsMinusEqualsPageAndSpaceSelectsVisibleFirstCandidate) {
  SessionController session(*engine_);
  const auto key = [](const std::uint32_t code) {
    protocol::v1::KeyEvent event;
    event.set_key_code(code);
    event.set_key_down(true);
    return event;
  };
  for (const auto code : {'S', 'H', 'I'}) static_cast<void>(session.Handle(key(code)));
  const auto second = session.Handle(key(VK_OEM_PLUS));
  ASSERT_EQ(second.composition().page(), 1U);
  const auto first = session.Handle(key(VK_OEM_MINUS));
  EXPECT_EQ(first.composition().page(), 0U);
  const auto next = session.Handle(key(VK_OEM_PLUS));
  const auto selected = session.Handle(key(VK_SPACE));
  ASSERT_TRUE(selected.has_commit_request());
  EXPECT_EQ(selected.commit_request().text(), next.composition().candidates(0).text());
}

TEST_F(FullPinyin, WindowsApostropheDisambiguatesXian) {
  Type("xi");
  ASSERT_TRUE(engine_->ProcessKey({.virtual_key = VK_OEM_7, .key_down = true}).consumed);
  const auto state = Type("an");
  ASSERT_FALSE(state.candidate_text.empty());
  EXPECT_EQ(state.candidate_text.front(), "西安");
}

TEST_F(FullPinyin, ModernDailyWordsAreAvailableAsFirstCandidates) {
  for (const auto& [pinyin, expected] :
       {std::pair{"weixin", "微信"}, {"saomazhifu", "扫码支付"},
        {"yidongzhifu", "移动支付"}, {"shoujihao", "手机号"},
        {"wangyueche", "网约车"}, {"kuaididanhao", "快递单号"},
        {"jiqixuexi", "机器学习"}, {"yunjisuan", "云计算"},
        {"yuanchengbangong", "远程办公"}, {"zhongwenshurufa", "中文输入法"},
        {"houxuanfanye", "候选翻页"}, {"shuzirenminbi", "数字人民币"},
        {"gongxiangdanche", "共享单车"}}) {
    SCOPED_TRACE(pinyin);
    const auto state = Type(pinyin);
    ASSERT_FALSE(state.candidate_text.empty());
    EXPECT_EQ(state.candidate_text.front(), expected);
    engine_->ResetComposition();
  }
}

TEST_F(FullPinyin, BuildsDailySentencesUsingModernWords) {
  for (const auto& [pinyin, expected] :
       {std::pair{"woyongweixinsaomazhifu", "我用微信扫码支付"},
        {"qingbayanzhengmafageiwo", "请把验证码发给我"},
        {"qingfageiwokuaididanhao", "请发给我快递单号"},
        {"womenmingtianyuanchengbangong", "我们明天远程办公"}}) {
    SCOPED_TRACE(pinyin);
    const auto state = Type(pinyin);
    ASSERT_FALSE(state.candidate_text.empty());
    EXPECT_EQ(state.candidate_text.front(), expected);
    const auto selected = engine_->SelectCandidate(0);
    ASSERT_TRUE(selected.commit_text.has_value());
    EXPECT_EQ(*selected.commit_text, expected);
  }
}

TEST_F(FullPinyin, InvalidVisibleCandidateNumberKeepsTheComposition) {
  const auto state = Type("shi");
  ASSERT_EQ(state.candidate_text.size(), 8U);
  const auto selected = engine_->SelectCandidate(8);
  EXPECT_TRUE(selected.state.consumed);
  EXPECT_FALSE(selected.commit_text.has_value());
  EXPECT_EQ(selected.state.preedit, state.preedit);
  EXPECT_EQ(selected.state.candidate_text, state.candidate_text);
  EXPECT_EQ(selected.state.page, state.page);
}

TEST_F(FullPinyin, LetterKeyReleaseDoesNotAppendToComposition) {
  const auto before = Type("ni");
  const auto released = engine_->ProcessKey({.virtual_key = 'I', .key_down = false});
  EXPECT_FALSE(released.consumed);
  EXPECT_EQ(released.preedit, before.preedit);
  EXPECT_EQ(released.candidate_text, before.candidate_text);
}

TEST_F(FullPinyin, AltNumberDoesNotSelectOrClearTheVisibleMenu) {
  SessionController session(*engine_);
  protocol::v1::KeyEvent key;
  key.set_key_down(true);
  for (const auto letter : {'S', 'H'}) {
    key.set_key_code(letter);
    static_cast<void>(session.Handle(key));
  }
  key.set_key_code('I');
  const auto before = session.Handle(key);
  key.set_key_code('1');
  key.set_modifiers(1);  // Protocol Alt, not Rime's Shift bit.
  const auto modified = session.Handle(key);
  EXPECT_FALSE(modified.consumed());
  EXPECT_FALSE(modified.has_commit_request());
  EXPECT_EQ(modified.composition().preedit(), before.composition().preedit());
  EXPECT_EQ(modified.composition().candidates_size(), before.composition().candidates_size());
}

TEST_F(FullPinyin, UmlautAndPolyphonicWordsKeepTheirPinyinReadings) {
  for (const auto& [pinyin, expected] :
       {std::pair{"nvsheng", "女生"}, {"lvse", "绿色"}, {"lvyou", "旅游"},
        {"yinhang", "银行"}, {"xingwei", "行为"}, {"changcheng", "长城"}}) {
    SCOPED_TRACE(pinyin);
    const auto state = Type(pinyin);
    ASSERT_FALSE(state.candidate_text.empty());
    EXPECT_EQ(state.candidate_text.front(), expected);
    engine_->ResetComposition();
  }
}

TEST_F(FullPinyin, PagingAtFirstPageIsConsumedWithoutChangingComposition) {
  const auto first = Type("shi");
  const auto previous = engine_->ProcessKey({.virtual_key = VK_PRIOR, .key_down = true});
  EXPECT_TRUE(previous.consumed);
  EXPECT_EQ(previous.page, 0U);
  EXPECT_EQ(previous.candidate_text, first.candidate_text);
}

TEST_F(FullPinyin, WindowsPageKeysNavigateAndNumberSelectsCurrentPage) {
  SessionController session(*engine_);
  const auto key = [](const std::uint32_t code) {
    protocol::v1::KeyEvent event;
    event.set_key_code(code);
    event.set_key_down(true);
    event.mutable_privacy()->set_allow_composition(true);
    return event;
  };
  static_cast<void>(session.Handle(key('S')));
  static_cast<void>(session.Handle(key('H')));
  const auto first = session.Handle(key('I'));
  ASSERT_EQ(first.composition().page(), 0U);
  ASSERT_EQ(first.composition().candidates_size(), 8);
  ASSERT_TRUE(first.composition().has_next_page());
  const auto second = session.Handle(key(VK_NEXT));
  ASSERT_TRUE(second.consumed());
  ASSERT_EQ(second.composition().page(), 1U);
  ASSERT_GT(second.composition().candidates_size(), 0);
  EXPECT_EQ(first.composition().candidates(0).text(), second.composition().candidates(0).text());
  const auto previous = session.Handle(key(VK_PRIOR));
  ASSERT_EQ(previous.composition().page(), 0U);
  EXPECT_EQ(previous.composition().candidates(0).text(), first.composition().candidates(0).text());
  const auto next = session.Handle(key(VK_NEXT));
  const auto selected = session.Handle(key('1'), 7);
  ASSERT_TRUE(selected.has_commit_request());
  EXPECT_EQ(selected.commit_request().text(), next.composition().candidates(0).text());
  EXPECT_EQ(selected.commit_request().expected_context_version(), 7U);
}

TEST_F(FullPinyin, LastPageDoesNotWrapAndOutOfPageSelectionDoesNotCommit) {
  auto state = Type("shi");
  std::size_t pages = 0;
  while (state.has_next_page && pages < 200) {
    const auto previous_page = state.page;
    const auto previous_highlight = state.highlighted_candidate;
    state = engine_->ProcessKey({.virtual_key = VK_NEXT, .key_down = true});
    ASSERT_TRUE(state.page > previous_page ||
                (state.page == previous_page && state.highlighted_candidate > previous_highlight));
    ++pages;
  }
  ASSERT_LT(pages, 200U);
  ASSERT_GT(pages, 0U);
  EXPECT_FALSE(engine_->SelectCandidate(state.candidate_text.size()).commit_text.has_value());
  const auto past_end = engine_->ProcessKey({.virtual_key = VK_NEXT, .key_down = true});
  EXPECT_TRUE(past_end.consumed);
  EXPECT_EQ(past_end.page, state.page);
  EXPECT_EQ(past_end.candidate_text, state.candidate_text);
}

// Explicit development probe only: never contact the user's live IME in CTest.
TEST(RimeEngine, DISABLED_LiveInputHostSpaceCommitsNi) {
  wchar_t enabled[2]{};
  if (GetEnvironmentVariableW(L"ZHIXU_LIVE_PIPE_PROBE", enabled, 2) != 1 || enabled[0] != L'1') {
    GTEST_SKIP() << "Requires an explicitly restarted development InputHost and opt-in.";
  }
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(L"tsf-input");
  ASSERT_TRUE(endpoint.has_value());
  ipc::NamedPipeClient client;
  ASSERT_TRUE(client.Connect(*endpoint, std::chrono::seconds(3)).has_value());
  const auto request = [](protocol::v1::MessageType type,
                          const google::protobuf::MessageLite& payload,
                          std::uint64_t id, std::uint64_t version) {
    protocol::Envelope envelope;
    envelope.set_protocol_version(protocol::kProtocolVersion);
    envelope.set_session_id("development-live-ni-probe");
    envelope.set_request_id(id);
    envelope.set_context_version(version);
    envelope.set_message_type(type);
    payload.SerializeToString(envelope.mutable_payload());
    return envelope;
  };
  protocol::Hello hello;
  hello.set_min_version(protocol::kProtocolVersion);
  hello.set_max_version(protocol::kProtocolVersion);
  const auto acknowledgment = client.Call(request(protocol::v1::HELLO, hello, 1, 0),
      std::chrono::seconds(2));
  ASSERT_TRUE(acknowledgment.has_value());
  ASSERT_EQ(acknowledgment->message_type(), protocol::v1::HELLO_ACK);
  protocol::v1::EngineAction final_action;
  const std::uint32_t keys[] = {'N', 'I', VK_SPACE};
  for (std::size_t index = 0; index < 3; ++index) {
    SCOPED_TRACE(index);
    protocol::v1::KeyEvent event;
    event.set_key_code(keys[index]);
    event.set_key_down(true);
    event.mutable_privacy()->set_allow_composition(true);
    const auto envelope = request(protocol::v1::KEY_EVENT, event, index + 2, index);
    const auto response = client.Call(envelope, std::chrono::seconds(2));
    ASSERT_TRUE(response.has_value());
    ASSERT_EQ(response->message_type(), protocol::v1::ENGINE_ACTION);
    ASSERT_EQ(response->session_id(), envelope.session_id());
    ASSERT_EQ(response->request_id(), envelope.request_id());
    ASSERT_TRUE(final_action.ParseFromString(response->payload()));
    EXPECT_TRUE(final_action.consumed());
    if (index == 1) {
      ASSERT_GT(final_action.composition().candidates_size(), 0);
      bool has_ni = false;
      for (const auto& candidate : final_action.composition().candidates()) {
        has_ni |= candidate.text() == "\xE4\xBD\xA0";
      }
      EXPECT_TRUE(has_ni) << "Live candidate menu must contain the known synthetic Ni entry.";
    }
  }
  ASSERT_TRUE(final_action.has_commit_request());
  EXPECT_TRUE(final_action.commit_request().text() == "\xE4\xBD\xA0");
}

}  // namespace
TEST_F(FullPinyin, GridSecondRowNumberCommitsTheVisibleText) {
  SessionController session(*engine_);
  for (const auto key : {'S','H','I'}) static_cast<void>(session.Handle(EditingKey(key)));
  const auto page = session.Handle(EditingKey(VK_NEXT));
  ASSERT_EQ(page.composition().page(), 1U);
  ASSERT_EQ(page.composition().candidates_size(), 30);
  static_cast<void>(session.Handle(EditingKey(VK_DOWN)));
  const auto selected = session.Handle(EditingKey('2'));
  ASSERT_TRUE(selected.has_commit_request());
  EXPECT_EQ(selected.commit_request().text(), page.composition().candidates(7).text());
}

TEST_F(FullPinyin, WanxiangProfessionalWordsAndPoetryAreAvailable) {
  for (const auto& [code, text] : {std::pair{"erlvjiawan", "二氯甲烷"},
      {"kabotini", "卡博替尼"}, {"chunmianbujuexiao", "春眠不觉晓"},
      {"baiqucai", "白屈菜"}, {"aletaidiqu", "阿勒泰地区"}}) {
    SCOPED_TRACE(code);
    auto state = Type(code);
    bool found = false;
    for (int page = 0; page < 10; ++page) {
      found |= std::find(state.candidate_text.begin(), state.candidate_text.end(), text) != state.candidate_text.end();
      if (found || !state.has_next_page) break;
      state = engine_->ProcessKey({.virtual_key = VK_NEXT, .key_down = true});
    }
    EXPECT_TRUE(found);
    engine_->ResetComposition();
  }
}

TEST_F(FullPinyin, UpgradePreservesAnActuallyLearnedLegacyPhrase) {
  const std::string code = "zhixuqianyizhuanyongci", expected = "知序迁移专用词";
  const auto fresh = Type(code);
  ASSERT_FALSE(fresh.candidate_text.empty()); ASSERT_NE(fresh.candidate_text.front(), expected);
  engine_.reset();
  const auto legacy_shared = user_data_ / "legacy-shared";
  const auto legacy_profile = user_data_ / "legacy-profile";
  std::filesystem::create_directories(legacy_shared);
  std::filesystem::create_directories(legacy_profile);
  for (const auto* name : {"pinyin_simp.dict.yaml", "zhixu_pinyin.schema.yaml", "default.yaml"})
    std::filesystem::copy_file(std::filesystem::path(ZHIXU_RIME_FULL_DATA) / name, legacy_shared / name);
  std::ofstream(legacy_shared / "zhixu_pinyin.dict.yaml") <<
      "---\nname: zhixu_pinyin\nversion: 'legacy'\nsort: by_weight\nuse_preset_vocabulary: false\nimport_tables:\n  - pinyin_simp\n  - learning_probe\n...\n";
  std::ofstream(legacy_shared / "learning_probe.dict.yaml") <<
      "---\nname: learning_probe\nversion: '1'\nsort: by_weight\n...\n知序迁移专用词\tzhi xu qian yi zhuan yong ci\t10000\n";
  {
    RimeEngine legacy({.shared_data_directory = legacy_shared, .user_data_directory = legacy_profile,
                       .schema_id = "zhixu_pinyin"});
    ASSERT_TRUE(legacy.available());
    EngineState state;
    for (const char key : code) state = legacy.ProcessKey({.virtual_key = static_cast<unsigned int>(key), .key_down = true});
    ASSERT_FALSE(state.candidate_text.empty()); ASSERT_EQ(state.candidate_text.front(), expected);
    ASSERT_EQ(legacy.SelectCandidate(0).commit_text, expected);
  }
  ASSERT_TRUE(std::filesystem::exists(legacy_profile / "pinyin_simp.userdb"));
  // Seed only compiled resources, preserving the legacy learning database.
  std::filesystem::copy(compiled_profile_ / "build", legacy_profile / "build",
      std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
  engine_ = std::make_unique<RimeEngine>(RimeEngineConfig{
      .shared_data_directory = ZHIXU_RIME_FULL_DATA, .user_data_directory = legacy_profile,
      .schema_id = "zhixu_pinyin"});
  ASSERT_TRUE(engine_->available());
  const auto state = Type(code);
  ASSERT_FALSE(state.candidate_text.empty()); EXPECT_EQ(state.candidate_text.front(), expected);
}

}  // namespace zhixu::input_host

namespace zhixu::input_host { namespace {
TEST_F(FullPinyin, ReportedZhngowKeepsEveryKey) {
  std::string expected;
  for (const char key : std::string("zhngow")) {
    expected.push_back(key);
    const auto result = engine_->ProcessKeyAction({.virtual_key = static_cast<std::uint32_t>(key - 'a' + 'A'), .key_down = true, .key_symbol = static_cast<std::uint32_t>(key)});
    std::string raw = result.state.preedit;
    raw.erase(std::remove(raw.begin(), raw.end(), ' '), raw.end());
    EXPECT_TRUE(result.state.consumed) << expected;
    EXPECT_FALSE(result.commit_text.has_value()) << expected;
    EXPECT_EQ(raw, expected);
  }
}

TEST_F(FullPinyin, KeypadSelectsSameCandidateAsNumberRow) {
  SessionController session(*engine_);
  session.Handle(EditingKey('N'));
  const auto action = session.Handle(EditingKey('I'));
  ASSERT_GT(action.composition().candidates_size(), 0);
  const auto expected = action.composition().candidates(0).text();
  const auto chosen = session.Handle(EditingKey(VK_NUMPAD1));
  ASSERT_TRUE(chosen.has_commit_request());
  EXPECT_EQ(chosen.commit_request().text(), expected);
}
} }

namespace zhixu::input_host { namespace {
TEST_F(FullPinyin, ReportedZhngowKeepsEveryKeyThroughTsfPipe) {
  InputHostServer host(*engine_);
  const auto role = L"invalid-spelling-" + std::to_wstring(GetCurrentProcessId());
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  ASSERT_TRUE(endpoint.has_value());
  std::jthread worker([&](std::stop_token stop) { EXPECT_TRUE(host.Serve(role, stop)); });
  tsf::InputHostTransport transport(role);
  bool connected = false;
  for (int attempt = 0; attempt < 50 && !connected; ++attempt) {
    connected = transport.Connect(std::chrono::seconds(1));
    if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  tsf::KeyEventSink keys(&transport);
  std::string expected;
  if (connected) {
    for (const auto key : std::string("zhngow")) {
      expected.push_back(key);
      const auto started = std::chrono::steady_clock::now();
      const auto action = keys.Handle({.virtual_key = static_cast<WPARAM>(key - 'a' + 'A'),
          .key_symbol = static_cast<std::uint32_t>(key)});
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started).count();
      std::cout << "INVALID_PIPE prefix=" << expected << " action=" << action.has_value()
                << " us=" << elapsed << std::endl;
      EXPECT_TRUE(action.has_value()) << expected;
      if (!action) continue;
      EXPECT_TRUE(action->consumed());
      EXPECT_FALSE(action->has_commit_request());
      auto raw = action->composition().preedit();
      raw.erase(std::remove(raw.begin(), raw.end(), ' '), raw.end());
      EXPECT_EQ(raw, expected);
    }
  }
  transport.Disconnect();
  worker.request_stop();
  worker.join();
  EXPECT_TRUE(connected);
}


} }

namespace zhixu::input_host {
namespace {
TEST_F(FullPinyin, NasalVowelRotationKeepsUserAndSupportAheadOfSentences) {
  for(const auto* input:{"yngohu","yngo'hu","yonghu","yong'hu","yognhu","ynoghu"}) {
    SCOPED_TRACE(input);engine_->ResetComposition();const auto state=Type(input);
    ASSERT_GE(state.candidate_text.size(),2u);EXPECT_EQ(state.candidate_text[0],"用户");EXPECT_EQ(state.candidate_text[1],"拥护");
    const auto selected=engine_->SelectCandidate(0);ASSERT_TRUE(selected.commit_text);EXPECT_EQ(*selected.commit_text,"用户");EXPECT_TRUE(selected.state.preedit.empty());
  }
}
TEST_F(FullPinyin, NasalVowelRotationIsGeneralAndConsumesTheWholeWord) {
  for(const auto& [input,word]:{std::pair{"shngahai","上海"},{"chngajiang","长江"},{"gngozuo","工作"}}) {
    SCOPED_TRACE(input);engine_->ResetComposition();const auto state=Type(input);
    const auto found=std::find(state.candidate_text.begin(),state.candidate_text.end(),word);
    ASSERT_NE(found,state.candidate_text.end());
    const auto selected=engine_->SelectCandidate(static_cast<std::size_t>(found-state.candidate_text.begin()));
    ASSERT_TRUE(selected.commit_text);EXPECT_EQ(*selected.commit_text,word);EXPECT_TRUE(selected.state.preedit.empty());
  }
}
}
}

namespace zhixu::input_host {
namespace {
TEST_F(FullPinyin, UmlautUEAndVEAcceptBothDictionaryConventions) {
  for (const auto& [input, word] : {
           std::pair{"hulue", "忽略"}, {"hulve", "忽略"},
           {"hu'lue", "忽略"}, {"hu'lve", "忽略"},
           {"celue", "策略"}, {"celve", "策略"},
           {"nuedai", "虐待"}, {"nvedai", "虐待"}}) {
    SCOPED_TRACE(input);
    engine_->ResetComposition();
    const auto state = Type(input);
    const auto found = std::find(state.candidate_text.begin(),
                                 state.candidate_text.end(), word);
    ASSERT_NE(found, state.candidate_text.end());
    const auto selected = engine_->SelectCandidate(
        static_cast<std::size_t>(found - state.candidate_text.begin()));
    ASSERT_TRUE(selected.commit_text);
    EXPECT_EQ(*selected.commit_text, word);
    EXPECT_TRUE(selected.state.preedit.empty());
  }
}
}
}

namespace zhixu::input_host {
namespace {
TEST_F(FullPinyin, ExpandedPagesKeepTheirCandidatesOnRoundTrip) {
  const auto first=Type("shi");
  ASSERT_EQ(first.candidate_text.size(),8u);
  const auto second=engine_->ProcessKey({.virtual_key=VK_NEXT,.key_down=true});
  ASSERT_EQ(second.page,1u);ASSERT_EQ(second.candidate_text.size(),30u);
  EXPECT_TRUE(std::equal(first.candidate_text.begin(),first.candidate_text.end(),second.candidate_text.begin()));
  for(unsigned row=1;row<5;++row) {
    const auto moved=engine_->ProcessKey({.virtual_key=VK_NEXT,.key_down=true});
    EXPECT_EQ(moved.page,1u);EXPECT_EQ(moved.highlighted_candidate,row*6);
    EXPECT_EQ(moved.candidate_text,second.candidate_text);
  }
  const auto third=engine_->ProcessKey({.virtual_key=VK_NEXT,.key_down=true});
  ASSERT_EQ(third.page,2u);ASSERT_EQ(third.candidate_text.size(),30u);
  EXPECT_EQ(engine_->ProcessKey({.virtual_key=VK_PRIOR,.key_down=true}).candidate_text,second.candidate_text);
  for(int row=0;row<4;++row)static_cast<void>(engine_->ProcessKey({.virtual_key=VK_PRIOR,.key_down=true}));
  EXPECT_EQ(engine_->ProcessKey({.virtual_key=VK_PRIOR,.key_down=true}).candidate_text,first.candidate_text);
}
TEST_F(FullPinyin, LastGridCellCommitsExactlyAndNewSpellingIsCompact) {
  Type("shi");
  const auto grid=engine_->ProcessKey({.virtual_key=VK_NEXT,.key_down=true});
  ASSERT_EQ(grid.candidate_text.size(),30u);
  const auto selected=engine_->SelectCandidate(29);
  ASSERT_TRUE(selected.commit_text);EXPECT_EQ(*selected.commit_text,grid.candidate_text[29]);
  EXPECT_TRUE(selected.state.preedit.empty());
  const auto next=Type("shi");EXPECT_EQ(next.page,0u);EXPECT_EQ(next.candidate_text.size(),8u);
}
}
}

namespace zhixu::input_host {
TEST_F(FullPinyin, RuntimeSettingsChangeCompactCountWithoutChangingGridRows) {
  engine_->Configure(true, 3, false);
  auto state=Type("shi"); ASSERT_EQ(state.candidate_text.size(),3u);
  auto third=state.candidate_text[2];
  EXPECT_EQ(engine_->SelectCandidate(2).commit_text, third);
  engine_->Configure(true,9,false);
  state=Type("shi"); ASSERT_EQ(state.candidate_text.size(),9u);
  const auto ninth=state.candidate_text[8]; EXPECT_EQ(engine_->SelectCandidate(8).commit_text,ninth);
  state=Type("shi"); state=engine_->ProcessKey({.virtual_key=VK_NEXT,.key_down=true});
  EXPECT_EQ(state.candidate_text.size(),30u); EXPECT_EQ(state.highlighted_candidate,0u);
  state=engine_->ProcessKey({.virtual_key=VK_NEXT,.key_down=true}); EXPECT_EQ(state.highlighted_candidate,6u);
}
TEST_F(FullPinyin, RuntimeFuzzySwitchChangesRecallAndPreservesExactPinyin) {
  engine_->Configure(false,9,false);
  auto state=Type("zongguo");
  for(int i=0;i<4;++i){ EXPECT_EQ(std::count(state.candidate_text.begin(),state.candidate_text.end(),"中国"),0);state=engine_->ProcessKey({.virtual_key=VK_NEXT,.key_down=true}); }
  engine_->ResetComposition(); engine_->Configure(true,9,false); state=Type("zongguo");
  bool found=false;
  for(int i=0;i<4;++i){found=found || std::find(state.candidate_text.begin(),state.candidate_text.end(),"中国")!=state.candidate_text.end();state=engine_->ProcessKey({.virtual_key=VK_NEXT,.key_down=true});} EXPECT_TRUE(found);
  engine_->ResetComposition(); engine_->Configure(false,8,false); EXPECT_EQ(Type("zhongguo").candidate_text.front(),"中国");
}
}

namespace zhixu::input_host {
TEST_F(FullPinyin, RuntimePrivacyChangePreservesTypedSpelling) {
  SessionController session(*engine_); session.Configure(true,8,true);
  auto key=[](unsigned code){protocol::v1::KeyEvent k;k.set_key_code(code>='a' && code<='z' ? code-'a'+'A' : code);k.set_key_down(true);k.mutable_privacy()->set_allow_composition(true);return k;};
  for(char c:std::string("zhong"))session.Handle(key(c));
  session.Configure(true,8,false);
  protocol::v1::EngineAction state;
  for(char c:std::string("guo"))state=session.Handle(key(c));
  ASSERT_GT(state.composition().candidates_size(),0);
  EXPECT_EQ(state.composition().candidates(0).text(),"中国");
  const auto selected=session.Handle(key(VK_SPACE));ASSERT_TRUE(selected.has_commit_request());EXPECT_EQ(selected.commit_request().text(),"中国");
}
TEST_F(FullPinyin, RuntimeSpellingAndCountWaitUntilCurrentCompositionEnds) {
  engine_->Configure(true,8,false);Type("zhong");
  engine_->Configure(false,3,false);
  auto state=Type("guo");ASSERT_FALSE(state.candidate_text.empty());EXPECT_EQ(state.candidate_text.front(),"中国");
  engine_->SelectCandidate(0);engine_->Configure(false,3,false);
  state=Type("shi");EXPECT_EQ(state.candidate_text.size(),3u);
}
TEST_F(FullPinyin, RuntimeLearningOffThenOnPreservesTheExistingUserDictionary) {
  auto contains=[&](const std::string& text){
    const auto db=user_data_/"pinyin_simp.userdb";if(!std::filesystem::exists(db))return false;
    for(const auto& f:std::filesystem::directory_iterator(db))if(f.is_regular_file()){
      std::ifstream in(f.path(),std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(in)),{});
      if(bytes.find(text)!=std::string::npos)return true;
    }return false;
  };
  engine_->Configure(true,8,false);Type("zhongguo");ASSERT_EQ(engine_->SelectCandidate(0).commit_text,"中国");engine_.reset();
  EXPECT_FALSE(contains("中国"));
  auto reopen=[&]{engine_=std::make_unique<RimeEngine>(RimeEngineConfig{.shared_data_directory=ZHIXU_RIME_FULL_DATA,.user_data_directory=user_data_,.schema_id="zhixu_pinyin"});ASSERT_TRUE(engine_->available());};
  reopen();engine_->Configure(true,8,true);Type("zhongguo");ASSERT_EQ(engine_->SelectCandidate(0).commit_text,"中国");engine_.reset();
  EXPECT_TRUE(contains("中国"));
  reopen();engine_->Configure(true,8,false);Type("shanghai");ASSERT_EQ(engine_->SelectCandidate(0).commit_text,"上海");engine_.reset();
  EXPECT_FALSE(contains("上海")); EXPECT_TRUE(contains("中国"));
  reopen();
}
}

namespace zhixu::input_host {
TEST_F(FullPinyin, ContextScoringHandlesReportedSentencesWithoutWholeSentenceEntries) {
  engine_->Configure(true,7,false);
  for(const auto& pair:std::initializer_list<std::pair<const char*,const char*>>{
      {"jianchabingshicehou","检查并实测后"},
      {"nideganjueyouyiju","你的感觉有依据"},
      {"qingnibahuiyijiluzhenglihaofageiwo","请你把会议记录整理好发给我"},
      {"tazhengzaiyanjiushengmingdeqiyuan","他正在研究生命的起源"}}) {
    engine_->ResetComposition();
    const auto state=Type(pair.first);
    ASSERT_FALSE(state.candidate_text.empty());
    ASSERT_EQ(state.candidate_text.front(),pair.second) << pair.first;
    const auto selected=engine_->SelectCandidate(0);
    EXPECT_EQ(selected.commit_text,pair.second);
    EXPECT_TRUE(selected.state.preedit.empty());
  }
}

TEST_F(FullPinyin, LearnedFuzzySentenceDoesNotSuppressExactContextSentence) {
  engine_->Configure(true,7,true);
  const auto learned=Type("jianchabinshichehou");
  const auto found=std::find(learned.candidate_text.begin(),learned.candidate_text.end(),"检查宾士车后");
  ASSERT_NE(found,learned.candidate_text.end());
  ASSERT_EQ(engine_->SelectCandidate(found-learned.candidate_text.begin()).commit_text,"检查宾士车后");
  engine_->ResetComposition();
  const auto exact=Type("jianchabinshichehou");
  ASSERT_FALSE(exact.candidate_text.empty());
  EXPECT_EQ(exact.candidate_text.front(),"检查宾士车后");
  engine_->ResetComposition();
  const auto corrected=Type("jianchabingshicehou");
  ASSERT_FALSE(corrected.candidate_text.empty());
  EXPECT_EQ(corrected.candidate_text.front(),"检查并实测后");
}

TEST_F(FullPinyin, XiehouPrefixOffersFullExpressionAndCommitsSelectedIndex) {
  engine_->Configure(true,7,false);
  for(const auto& pair:std::initializer_list<std::pair<const char*,const char*>>{
      {"baxianguohai","八仙过海——各显神通"},
      {"waishengdadenglong","外甥打灯笼——照旧"}}) {
    engine_->ResetComposition();
    const auto state=Type(pair.first);
    const auto found=std::find(state.candidate_text.begin(),state.candidate_text.end(),pair.second);
    ASSERT_NE(found,state.candidate_text.end());
    const auto selected=engine_->SelectCandidate(found-state.candidate_text.begin());
    EXPECT_EQ(selected.commit_text,pair.second);
    EXPECT_TRUE(selected.state.preedit.empty());
  }
}

TEST_F(FullPinyin, MalformedUserModelCannotOverrideValidatedSharedModel) {
  engine_.reset();
  char image[44]{};
  std::memcpy(image,"Rime::Grammar/1.0",17);
  const std::uint32_t units=0x7fffffff, offset=0x80000000;
  std::memcpy(image+36,&units,4);
  std::memcpy(image+40,&offset,4);
  std::ofstream bad(user_data_/"wanxiang-lts-zh-hans.gram",std::ios::binary);
  bad.write(image,sizeof(image));bad.close();
  engine_=std::make_unique<RimeEngine>(RimeEngineConfig{
      .shared_data_directory=ZHIXU_RIME_FULL_DATA,.user_data_directory=user_data_,.schema_id="zhixu_pinyin"});
  ASSERT_TRUE(engine_->available());
  const auto state=Type("nideganjueyouyiju");
  EXPECT_FALSE(state.candidate_text.empty());
}

TEST(RimeEngine, TruncatedOptionalGrammarFallsBackToNormalTyping) {
  const auto root=std::filesystem::temp_directory_path()/
      ("ZhixuIME-grammar-fallback-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
  std::filesystem::create_directories(root/"shared");
  std::filesystem::copy(ZHIXU_RIME_TEST_DATA,root/"shared",std::filesystem::copy_options::recursive);
  std::ofstream(root/"shared"/"wanxiang-lts-zh-hans.gram",std::ios::binary)<<"Rime::Grammar/1.0";
  std::filesystem::create_directories(root/"user");
  RimeEngine engine({.shared_data_directory=root/"shared",.user_data_directory=root/"user"});
  ASSERT_TRUE(engine.available());
  engine.ProcessKey({.virtual_key='N',.key_down=true});
  const auto state=engine.ProcessKey({.virtual_key='I',.key_down=true});
  EXPECT_NE(std::find(state.candidate_text.begin(),state.candidate_text.end(),"你"),state.candidate_text.end());
}
}
