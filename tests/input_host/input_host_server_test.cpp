#include "input_host/input_host_server.h"

#include <chrono>
#include <future>
#include <memory>
#include "input_host/prediction/prediction_runtime.h"
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "input_host/ui/candidate_presentation.h"
#include "input_host/ui/recommendation_presentation.h"

namespace zhixu::input_host {
namespace {

using namespace std::chrono_literals;

class FakeInputEngine final : public InputEngine {
 public:
  EngineState ProcessKey(const EngineKey&) override { return {.consumed = true}; }

  CommitOrState SelectCandidate(const std::size_t) override {
    return {.state = {.consumed = true}, .commit_text = "commit"};
  }
};

class RecordingCandidatePresenter final : public ui::CandidatePresenter {
 public:
  void Present(ui::CandidatePresentation presentation) override {
    last_presentation = std::move(presentation);
    ++present_calls;
  }

  void Hide() noexcept override { ++hide_calls; }

  std::optional<ui::CandidatePresentation> last_presentation;
  unsigned int present_calls = 0;
  unsigned int hide_calls = 0;
};

class RecordingRecommendationPresenter final : public ui::RecommendationPresenter {
 public:
  void Present(ui::RecommendationPresentation presentation) override {
    last_presentation = std::move(presentation);
    ++present_calls;
  }
  void Hide() noexcept override { ++hide_calls; }

  std::optional<ui::RecommendationPresentation> last_presentation;
  unsigned int present_calls = 0;
  unsigned int hide_calls = 0;
};

template <typename Message>
protocol::Envelope Request(const protocol::v1::MessageType type, const Message& message,
                           const std::uint64_t request_id) {
  protocol::Envelope request;
  request.set_protocol_version(protocol::kProtocolVersion);
  request.set_message_type(type);
  request.set_session_id("integration-session");
  request.set_request_id(request_id);
  message.SerializeToString(request.mutable_payload());
  return request;
}

protocol::v1::Hello Hello() {
  protocol::v1::Hello hello;
  hello.set_min_version(protocol::kProtocolVersion);
  hello.set_max_version(protocol::kProtocolVersion);
  return hello;
}

bool ConnectWithRetry(ipc::NamedPipeClient& client, const ipc::PipeEndpoint& endpoint) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (client.Connect(endpoint, 50ms)) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

protocol::v1::CaretUpdate Caret() {
  protocol::v1::CaretUpdate caret;
  caret.mutable_caret()->set_left(100);
  caret.mutable_caret()->set_top(200);
  caret.mutable_caret()->set_right(102);
  caret.mutable_caret()->set_bottom(220);
  caret.mutable_work_area()->set_right(1920);
  caret.mutable_work_area()->set_bottom(1080);
  caret.set_dpi(144);
  return caret;
}

protocol::v1::EngineAction Candidates() {
  protocol::v1::EngineAction action;
  action.set_consumed(true);
  auto* candidate = action.mutable_composition()->add_candidates();
  candidate->set_text("ni");
  candidate->set_comment("you");
  candidate->set_index(3);
  return action;
}

TEST(CandidatePresentation, ShowsRegularCandidatesOnlyAtTheMatchingCaretVersion) {
  RecordingCandidatePresenter presenter;
  ui::CandidatePresentationController controller(&presenter);
  protocol::v1::PrivacyDecision privacy;
  privacy.set_allow_prediction(true);

  controller.OnCaretUpdate(Caret(), 7);
  controller.OnEngineAction(Candidates(), privacy, 7);

  ASSERT_EQ(presenter.present_calls, 1U);
  ASSERT_TRUE(presenter.last_presentation.has_value());
  EXPECT_EQ(presenter.last_presentation->caret.left, 100);
  ASSERT_EQ(presenter.last_presentation->candidates.size(), 1U);
  EXPECT_EQ(presenter.last_presentation->candidates[0].text, L"ni");
  EXPECT_EQ(presenter.last_presentation->candidates[0].index, 3U);
  EXPECT_TRUE(presenter.last_presentation->candidates[0].selected);
}

TEST(CandidatePresentation, HidesCandidatesForProtectedOrStaleContexts) {
  RecordingCandidatePresenter presenter;
  ui::CandidatePresentationController controller(&presenter);
  protocol::v1::PrivacyDecision ordinary;
  ordinary.set_allow_prediction(true);
  protocol::v1::PrivacyDecision protected_field;
  protected_field.set_allow_prediction(false);

  controller.OnCaretUpdate(Caret(), 7);
  controller.OnEngineAction(Candidates(), protected_field, 7);
  controller.OnEngineAction(Candidates(), ordinary, 8);
  controller.Invalidate();

  EXPECT_EQ(presenter.present_calls, 0U);
  EXPECT_EQ(presenter.hide_calls, 3U);
}

TEST(CandidatePresentation, ShowsCandidatesAtZeroWidthInsertionCaret) {
  RecordingCandidatePresenter presenter;
  ui::CandidatePresentationController controller(&presenter);
  protocol::v1::PrivacyDecision privacy;
  privacy.set_allow_prediction(true);
  auto caret = Caret();
  caret.mutable_caret()->set_right(100);  // Empty TSF selection: left == right.
  controller.OnCaretUpdate(caret, 7);
  controller.OnEngineAction(Candidates(), privacy, 7);
  ASSERT_EQ(presenter.present_calls, 1U);
  ASSERT_TRUE(presenter.last_presentation.has_value());
  EXPECT_EQ(presenter.last_presentation->caret.left, 100);
  EXPECT_EQ(presenter.last_presentation->caret.right, 100);
  ASSERT_EQ(presenter.last_presentation->candidates.size(), 1U);
  EXPECT_EQ(presenter.last_presentation->candidates[0].text, L"ni");
}

TEST(CandidatePresentation, RejectsReversedCaretWidth) {
  RecordingCandidatePresenter presenter;
  ui::CandidatePresentationController controller(&presenter);
  protocol::v1::PrivacyDecision privacy;
  privacy.set_allow_prediction(true);
  auto caret = Caret();
  caret.mutable_caret()->set_right(99);
  controller.OnCaretUpdate(caret, 7);
  controller.OnEngineAction(Candidates(), privacy, 7);
  EXPECT_EQ(presenter.present_calls, 0U);
}

TEST(CandidatePresentation, PassesCurrentPageAndLastPageStateToTheWindow) {
  RecordingCandidatePresenter presenter;
  ui::CandidatePresentationController controller(&presenter);
  protocol::v1::PrivacyDecision privacy;
  privacy.set_allow_prediction(true);
  controller.OnCaretUpdate(Caret(), 7);
  auto action = Candidates();
  action.mutable_composition()->set_page(2);
  action.mutable_composition()->set_has_next_page(true);
  controller.OnEngineAction(action, privacy, 7);
  ASSERT_TRUE(presenter.last_presentation);
  EXPECT_EQ(presenter.last_presentation->page, 2U);
  EXPECT_TRUE(presenter.last_presentation->has_next_page);
  action.mutable_composition()->set_has_next_page(false);
  controller.OnEngineAction(action, privacy, 7);
  EXPECT_FALSE(presenter.last_presentation->has_next_page);
}

TEST(RecommendationPresentation, ShowsOnlyFinalResultsForTheCurrentCaretVersion) {
  RecordingRecommendationPresenter presenter;
  ui::RecommendationPresentationController controller(&presenter);
  controller.OnCaretUpdate(Caret(), 7);

  protocol::v1::PredictionResult stale;
  stale.set_final_result(true);
  stale.add_candidates()->set_text("stale");
  controller.OnPredictionResult(stale, 6);
  EXPECT_EQ(presenter.present_calls, 0U);

  protocol::v1::PredictionResult result;
  result.set_final_result(true);
  result.add_candidates()->set_text("first");
  result.add_candidates()->set_text("second");
  controller.OnPredictionResult(result, 7);

  ASSERT_EQ(presenter.present_calls, 1U);
  ASSERT_TRUE(presenter.last_presentation.has_value());
  EXPECT_EQ(presenter.last_presentation->recommendations, (std::vector<std::wstring>{L"first", L"second"}));
}

TEST(RecommendationPresentation, HidesVisibleRecommendationsWhenCaretUpdates) {
  RecordingRecommendationPresenter presenter;
  ui::RecommendationPresentationController controller(&presenter);
  controller.OnCaretUpdate(Caret(), 7);

  protocol::v1::PredictionResult result;
  result.set_final_result(true);
  result.add_candidates()->set_text("first");
  controller.OnPredictionResult(result, 7);
  ASSERT_EQ(presenter.present_calls, 1U);

  controller.OnCaretUpdate(Caret(), 8);
  EXPECT_EQ(presenter.hide_calls, 1U);
}

TEST(InputHostPipe, PresentsOnlyCurrentPredictionResultsAndHidesThemOnCaretChange) {
  FakeInputEngine engine;
  RecordingRecommendationPresenter presenter;
  InputHostServer host(engine, nullptr, {}, {}, &presenter);
  constexpr std::wstring_view kRole = L"input-host-prediction-presentation";
  std::jthread server([&](const std::stop_token stop_token) {
    const auto result = host.Serve(kRole, stop_token);
    EXPECT_TRUE(result) << static_cast<int>(result.error());
  });

  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(kRole);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  auto first_caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2);
  first_caret.set_context_version(7);
  ASSERT_TRUE(client.Call(first_caret, 1s));

  protocol::v1::PredictionResult result;
  result.set_final_result(true);
  result.add_candidates()->set_text("current");
  host.OnPredictionResult(result, 7);
  ASSERT_EQ(presenter.present_calls, 1U);

  auto next_caret = Request(protocol::v1::CARET_UPDATE, Caret(), 3);
  next_caret.set_context_version(8);
  ASSERT_TRUE(client.Call(next_caret, 1s));
  EXPECT_EQ(presenter.hide_calls, 1U);
  host.OnPredictionResult(result, 7);
  EXPECT_EQ(presenter.present_calls, 1U);

  client.Close();
  server.request_stop();
}

TEST(InputHostPipe, AcceptsCurrentRecommendationWithAltNumber) {
  FakeInputEngine engine;
  RecordingRecommendationPresenter presenter;
  InputHostServer host(engine, nullptr, {}, {}, &presenter);
  constexpr std::wstring_view kRole = L"input-host-recommendation-accept";
  std::jthread server([&](const std::stop_token stop_token) {
    const auto served = host.Serve(kRole, stop_token);
    EXPECT_TRUE(served) << static_cast<int>(served.error());
  });

  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(kRole);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  auto caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2);
  caret.set_context_version(7);
  ASSERT_TRUE(client.Call(caret, 1s));

  protocol::v1::PredictionResult prediction;
  prediction.set_final_result(true);
  prediction.add_candidates()->set_text("first");
  prediction.add_candidates()->set_text("second");
  host.OnPredictionResult(prediction, 7);

  protocol::v1::KeyEvent accept;
  accept.set_key_code('2');
  accept.set_key_down(true);
  accept.set_modifiers(1U);
  accept.mutable_privacy()->set_allow_prediction(true);
  auto accept_request = Request(protocol::v1::KEY_EVENT, accept, 3);
  accept_request.set_context_version(7);
  const auto response = client.Call(accept_request, 1s);
  ASSERT_TRUE(response);
  protocol::v1::EngineAction action;
  ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().text(), "second");
  EXPECT_EQ(action.commit_request().expected_context_version(), 7U);
  EXPECT_EQ(presenter.hide_calls, 1U);

  client.Close();
  server.request_stop();
}

TEST(InputHostPipe, MouseSelectionIsVersionCheckedAndDeliveredOnlyOnce) {
  FakeInputEngine engine;
  RecordingRecommendationPresenter presenter;
  InputHostServer host(engine, nullptr, {}, {}, &presenter);
  constexpr std::wstring_view role = L"input-host-mouse-selection";
  std::jthread server([&](const std::stop_token stop) {
    const auto served = host.Serve(role, stop);
    EXPECT_TRUE(served);
  });
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  auto caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2);
  caret.set_context_version(7);
  ASSERT_TRUE(client.Call(caret, 1s));
  protocol::v1::PredictionResult prediction;
  prediction.set_final_result(true);
  prediction.add_candidates()->set_text("mouse-selected");
  host.OnPredictionResult(prediction, 7);
  ASSERT_TRUE(presenter.last_presentation);
  const auto old_click = presenter.last_presentation->on_accept;
  ASSERT_TRUE(old_click);
  old_click(0);

  protocol::v1::PollRecommendation poll;
  poll.mutable_privacy()->set_allow_composition(true);
  poll.mutable_privacy()->set_allow_prediction(true);
  auto request = Request(protocol::v1::POLL_RECOMMENDATION, poll, 3);
  request.set_context_version(8);
  const auto stale = client.Call(request, 1s);
  ASSERT_TRUE(stale);
  protocol::v1::EngineAction action;
  ASSERT_TRUE(action.ParseFromString(stale->payload()));
  EXPECT_FALSE(action.has_commit_request());

  ASSERT_TRUE(client.Call(caret, 1s));
  host.OnPredictionResult(prediction, 7);
  const auto current_click = presenter.last_presentation->on_accept;
  old_click(0);  // A queued click from the previous displayed batch is ignored.
  request.set_context_version(7);
  const auto ignored = client.Call(request, 1s);
  ASSERT_TRUE(ignored);
  ASSERT_TRUE(action.ParseFromString(ignored->payload()));
  EXPECT_FALSE(action.has_commit_request());

  current_click(0);
  const auto accepted = client.Call(request, 1s);
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(action.ParseFromString(accepted->payload()));
  ASSERT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().text(), "mouse-selected");
  EXPECT_EQ(action.commit_request().expected_context_version(), 7U);
  const auto duplicate = client.Call(request, 1s);
  ASSERT_TRUE(duplicate);
  ASSERT_TRUE(action.ParseFromString(duplicate->payload()));
  EXPECT_FALSE(action.has_commit_request());
  client.Close();
  server.request_stop();
}

TEST(InputHostPipe, AcceptsTheFirstCurrentRecommendationWithTab) {
  FakeInputEngine engine;
  RecordingRecommendationPresenter presenter;
  InputHostServer host(engine, nullptr, {}, {}, &presenter, true);
  constexpr std::wstring_view kRole = L"input-host-recommendation-tab";
  std::jthread server([&](const std::stop_token stop_token) {
    const auto served = host.Serve(kRole, stop_token);
    EXPECT_TRUE(served) << static_cast<int>(served.error());
  });

  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(kRole);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  auto caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2);
  caret.set_context_version(7);
  ASSERT_TRUE(client.Call(caret, 1s));

  protocol::v1::PredictionResult prediction;
  prediction.set_final_result(true);
  prediction.add_candidates()->set_text("first");
  host.OnPredictionResult(prediction, 7);

  protocol::v1::KeyEvent accept;
  accept.set_key_code(0x09);
  accept.set_key_down(true);
  accept.mutable_privacy()->set_allow_prediction(true);
  auto accept_request = Request(protocol::v1::KEY_EVENT, accept, 3);
  accept_request.set_context_version(7);
  const auto response = client.Call(accept_request, 1s);
  ASSERT_TRUE(response);
  protocol::v1::EngineAction action;
  ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().text(), "first");

  client.Close();
  server.request_stop();
}

TEST(InputHostPipe, TabDoesNotAcceptRecommendationsByDefault) {
  FakeInputEngine engine;
  RecordingRecommendationPresenter presenter;
  InputHostServer host(engine, nullptr, {}, {}, &presenter);
  constexpr std::wstring_view kRole = L"input-host-recommendation-tab-default";
  std::jthread server([&](const std::stop_token stop_token) {
    const auto served = host.Serve(kRole, stop_token);
    EXPECT_TRUE(served) << static_cast<int>(served.error());
  });

  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(kRole);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  auto caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2);
  caret.set_context_version(7);
  ASSERT_TRUE(client.Call(caret, 1s));

  protocol::v1::PredictionResult prediction;
  prediction.set_final_result(true);
  prediction.add_candidates()->set_text("first");
  host.OnPredictionResult(prediction, 7);

  protocol::v1::KeyEvent accept;
  accept.set_key_code(0x09);
  accept.set_key_down(true);
  accept.mutable_privacy()->set_allow_prediction(true);
  auto accept_request = Request(protocol::v1::KEY_EVENT, accept, 3);
  accept_request.set_context_version(7);
  const auto response = client.Call(accept_request, 1s);
  ASSERT_TRUE(response);
  protocol::v1::EngineAction action;
  ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_FALSE(action.has_commit_request());

  client.Close();
  server.request_stop();
}

TEST(InputHostPipe, ProcessesCommitAppliedAndAcceptsNewClientAfterDisconnect) {
  FakeInputEngine engine;
  InputHostServer host(engine);
  constexpr std::wstring_view kRole = L"input-host-integration";
  std::jthread server([&](const std::stop_token stop_token) {
    const auto result = host.Serve(kRole, stop_token);
    EXPECT_TRUE(result) << static_cast<int>(result.error());
  });

  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(kRole);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  const auto hello = client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s);
  ASSERT_TRUE(hello);
  EXPECT_EQ(hello->message_type(), protocol::v1::HELLO_ACK);

  protocol::v1::KeyEvent choose_first;
  choose_first.set_key_code('1');
  choose_first.set_key_down(true);
  const auto action_response =
      client.Call(Request(protocol::v1::KEY_EVENT, choose_first, 2), 1s);
  ASSERT_TRUE(action_response);
  protocol::v1::EngineAction action;
  ASSERT_TRUE(action.ParseFromString(action_response->payload()));
  ASSERT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().expected_context_version(), 0U);

  protocol::v1::CommitApplied applied;
  applied.set_request_id(action.commit_request().request_id());
  applied.set_context_version(1);
  ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED, applied, 3), 1s));
  client.Close();

  ipc::NamedPipeClient replacement;
  ASSERT_TRUE(ConnectWithRetry(replacement, *endpoint));
  ASSERT_TRUE(replacement.Call(Request(protocol::v1::HELLO, Hello(), 4), 1s));
  replacement.Close();
  server.request_stop();
}

TEST(InputHostPipe, DispatchesOnlyConfirmedPredictionEligibleCommits) {
  FakeInputEngine engine;
  std::vector<ConfirmedCommit> dispatched;
  unsigned int invalidations = 0;
  InputHostServer host(engine, nullptr,
                       [&dispatched](ConfirmedCommit commit) { dispatched.push_back(std::move(commit)); },
                       [&invalidations] { ++invalidations; });
  constexpr std::wstring_view kRole = L"input-host-prediction-dispatch";
  std::jthread server([&](const std::stop_token stop_token) {
    const auto result = host.Serve(kRole, stop_token);
    EXPECT_TRUE(result) << static_cast<int>(result.error());
  });

  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(kRole);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  const auto invalidations_after_hello = invalidations;

  protocol::v1::KeyEvent choose_first;
  choose_first.set_key_code('1');
  choose_first.set_key_down(true);
  choose_first.mutable_privacy()->set_allow_prediction(true);
  choose_first.mutable_privacy()->set_allow_learning(true);
  const auto action_response = client.Call(Request(protocol::v1::KEY_EVENT, choose_first, 2), 1s);
  ASSERT_TRUE(action_response);
  EXPECT_EQ(invalidations, invalidations_after_hello + 1U);
  protocol::v1::EngineAction action;
  ASSERT_TRUE(action.ParseFromString(action_response->payload()));

  protocol::v1::CommitApplied wrong;
  wrong.set_request_id(action.commit_request().request_id() + 1);
  wrong.set_context_version(1);
  ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED, wrong, 3), 1s));
  EXPECT_TRUE(dispatched.empty());

  protocol::v1::CommitApplied applied;
  applied.set_request_id(action.commit_request().request_id());
  applied.set_context_version(1);
  ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED, applied, 4), 1s));
  ASSERT_EQ(dispatched.size(), 1U);
  EXPECT_EQ(dispatched[0].text, "commit");
  EXPECT_EQ(dispatched[0].context_version, 1U);
  EXPECT_TRUE(dispatched[0].allow_prediction);
  EXPECT_TRUE(dispatched[0].allow_learning);

  ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED, applied, 5), 1s));
  EXPECT_EQ(dispatched.size(), 1U);

  protocol::v1::KeyEvent protected_choose_first;
  protected_choose_first.set_key_code('1');
  protected_choose_first.set_key_down(true);
  protected_choose_first.mutable_privacy()->set_allow_prediction(false);
  const auto protected_action_response =
      client.Call(Request(protocol::v1::KEY_EVENT, protected_choose_first, 6), 1s);
  ASSERT_TRUE(protected_action_response);
  protocol::v1::EngineAction protected_action;
  ASSERT_TRUE(protected_action.ParseFromString(protected_action_response->payload()));
  ASSERT_TRUE(protected_action.has_commit_request());
  protocol::v1::CommitApplied protected_applied;
  protected_applied.set_request_id(protected_action.commit_request().request_id());
  protected_applied.set_context_version(2);
  ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED, protected_applied, 7), 1s));
  EXPECT_EQ(dispatched.size(), 1U);
  client.Close();
  server.request_stop();
}

}  // namespace
class MenuInputEngine final : public InputEngine {
 public:
  EngineState ProcessKey(const EngineKey&) override {
    EngineState state{.consumed = true, .preedit = "ni hao"};
    for (int i = 0; i < 8; ++i) state.candidate_text.push_back("word" + std::to_string(i));
    return state;
  }
  CommitOrState SelectCandidate(std::size_t index) override {
    ++selections;
    if (index == 0) return {.state = ProcessKey({})};
    return {.state = {.consumed = true}, .commit_text = "word" + std::to_string(index)};
  }
  unsigned int selections = 0;
};

TEST(InputHostServer, CandidateMouseSelectionIsOneShotAndRejectsOldBatch) {
  MenuInputEngine engine;
  RecordingCandidatePresenter presenter;
  InputHostServer host(engine, &presenter);
  std::jthread server([&](std::stop_token stop) { static_cast<void>(host.Serve(L"candidate-mouse-test", stop)); });
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(L"candidate-mouse-test");
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  ASSERT_TRUE(client.Call(Request(protocol::v1::CARET_UPDATE, Caret(), 2), 1s));
  protocol::v1::KeyEvent key;
  key.set_key_code('N'); key.set_key_down(true);
  key.mutable_privacy()->set_allow_composition(true);
  key.mutable_privacy()->set_allow_prediction(true);
  ASSERT_TRUE(client.Call(Request(protocol::v1::KEY_EVENT, key, 3), 1s));
  ASSERT_TRUE(presenter.last_presentation);
  const auto old_click = presenter.last_presentation->on_accept;
  ASSERT_TRUE(old_click);
  ASSERT_TRUE(client.Call(Request(protocol::v1::KEY_EVENT, key, 4), 1s));
  old_click(7);
  protocol::v1::PollRecommendation poll; *poll.mutable_privacy() = key.privacy();
  auto response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 5), 1s);
  ASSERT_TRUE(response);
  protocol::v1::EngineAction action; ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_FALSE(action.consumed()); EXPECT_EQ(engine.selections, 0U);
  const auto click = presenter.last_presentation->on_accept;
  click(0); click(0);
  response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 6), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_TRUE(action.consumed()); EXPECT_FALSE(action.has_commit_request());
  EXPECT_EQ(action.composition().preedit(), "ni hao"); EXPECT_EQ(engine.selections, 1U);
  presenter.last_presentation->on_accept(7);
  response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 7), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.has_commit_request()); EXPECT_EQ(action.commit_request().text(), "word7");
  const auto committed_id = action.commit_request().request_id();
  EXPECT_EQ(engine.selections, 2U);
  click(0);
  response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 8), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload())); EXPECT_FALSE(action.consumed());
  protocol::v1::CommitApplied applied;
  applied.set_request_id(committed_id); applied.set_context_version(1);
  ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED, applied, 9), 1s));
  // Legacy keyboard requests may omit the latest version. A mouse snapshot
  // must still match the server's authoritative version before selecting.
  response = client.Call(Request(protocol::v1::KEY_EVENT, key, 10), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.consumed()); ASSERT_EQ(action.composition().candidates_size(), 8);
  presenter.last_presentation->on_accept(7);
  response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 11), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_FALSE(action.has_commit_request()); EXPECT_EQ(engine.selections, 2U);
  client.Close(); server.request_stop();
}


TEST(CandidatePresentation, CaretMoveHidesPreviouslyPresentedCandidates) {
  RecordingCandidatePresenter presenter;
  ui::CandidatePresentationController controller(&presenter);
  protocol::v1::PrivacyDecision privacy;
  privacy.set_allow_prediction(true);
  controller.OnCaretUpdate(Caret(), 7);
  controller.OnEngineAction(Candidates(), privacy, 7);
  ASSERT_EQ(presenter.present_calls, 1U);
  auto moved = Caret();
  moved.mutable_caret()->set_left(200);
  moved.mutable_caret()->set_right(202);
  controller.OnCaretUpdate(moved, 7);
  EXPECT_EQ(presenter.hide_calls, 1U);
}


TEST(RecommendationPresentation, PreservesCitationsButDoesNotAttributeGeneratedText) {
  RecordingRecommendationPresenter presenter;
  ui::RecommendationPresentationController controller(&presenter);
  controller.OnCaretUpdate(Caret(), 7);
  protocol::v1::PredictionResult result;
  result.set_final_result(true);
  auto* quotation = result.add_candidates();
  quotation->set_text("quote");
  quotation->set_kind(protocol::v1::QUOTATION);
  quotation->set_source("Book");
  quotation->set_author("Author");
  quotation->set_chapter("Chapter");
  auto* generated = result.add_candidates();
  generated->set_text("generated");
  generated->set_kind(protocol::v1::MODEL_GENERATED);
  generated->set_source("misleading source");
  ASSERT_TRUE(controller.OnPredictionResult(result, 7));
  ASSERT_TRUE(presenter.last_presentation);
  EXPECT_EQ(presenter.last_presentation->recommendations,
            (std::vector<std::wstring>{L"quote", L"generated"}));
  EXPECT_EQ(presenter.last_presentation->provenance,
            (std::vector<std::wstring>{L"Book · Author · Chapter", L""}));
}

TEST(RecommendationPresentation, InvalidCaretCannotPresentARecommendation) {
  RecordingRecommendationPresenter presenter;
  ui::RecommendationPresentationController controller(&presenter);
  auto caret = Caret();
  caret.mutable_work_area()->set_right(caret.work_area().left());
  controller.OnCaretUpdate(caret, 7);
  protocol::v1::PredictionResult result;
  result.set_final_result(true);
  result.add_candidates()->set_text("text");
  EXPECT_FALSE(controller.OnPredictionResult(result, 7));
  EXPECT_EQ(presenter.present_calls, 0U);
}


TEST(InputHostServer, MovedOrInvalidCaretRevokesSavedAndQueuedCandidateClicks) {
  for (bool queued : {false, true}) for (bool invalid : {false, true}) {
    SCOPED_TRACE(queued ? "queued before move" : "callback after move");
    SCOPED_TRACE(invalid ? "invalid caret" : "moved caret");
    MenuInputEngine engine;
    RecordingCandidatePresenter presenter;
    InputHostServer host(engine, &presenter);
    std::jthread server([&](std::stop_token stop) {
      static_cast<void>(host.Serve(L"candidate-caret-change", stop));
    });
    const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(L"candidate-caret-change");
    ASSERT_TRUE(endpoint);
    ipc::NamedPipeClient client;
    ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
    ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
    ASSERT_TRUE(client.Call(Request(protocol::v1::CARET_UPDATE, Caret(), 2), 1s));
    protocol::v1::KeyEvent key;
    key.set_key_code('N'); key.set_key_down(true);
    key.mutable_privacy()->set_allow_composition(true);
    key.mutable_privacy()->set_allow_prediction(true);
    ASSERT_TRUE(client.Call(Request(protocol::v1::KEY_EVENT, key, 3), 1s));
    ASSERT_TRUE(presenter.last_presentation);
    const auto click = presenter.last_presentation->on_accept;
    ASSERT_TRUE(click);
    if (queued) click(7);
    auto caret = Caret();
    caret.mutable_caret()->set_left(200); caret.mutable_caret()->set_right(202);
    if (invalid) caret.mutable_work_area()->set_right(0);
    ASSERT_TRUE(client.Call(Request(protocol::v1::CARET_UPDATE, caret, 4), 1s));
    if (!queued) click(7);
    protocol::v1::PollRecommendation poll; *poll.mutable_privacy() = key.privacy();
    const auto response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 5), 1s);
    ASSERT_TRUE(response);
    protocol::v1::EngineAction action;
    ASSERT_TRUE(action.ParseFromString(response->payload()));
    EXPECT_FALSE(action.has_commit_request());
    EXPECT_EQ(engine.selections, 0U);
    client.Close(); server.request_stop();
  }
}

TEST(InputHostPipe, OnlyChangedCaretInvalidatesAnInFlightPrediction) {
  FakeInputEngine engine;
  unsigned int invalidations = 0;
  InputHostServer host(engine, nullptr, {}, [&] { ++invalidations; });
  std::jthread server([&](std::stop_token stop) {
    static_cast<void>(host.Serve(L"prediction-caret-invalidation", stop));
  });
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(L"prediction-caret-invalidation");
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  const auto after_hello = invalidations;
  auto caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2);
  caret.set_context_version(7);
  ASSERT_TRUE(client.Call(caret, 1s));
  ASSERT_TRUE(client.Call(caret, 1s));
  EXPECT_EQ(invalidations, after_hello); // Post-commit/identical caret does not cancel useful work.
  auto changed = Caret();
  changed.mutable_caret()->set_left(200); changed.mutable_caret()->set_right(202);
  caret = Request(protocol::v1::CARET_UPDATE, changed, 3);
  caret.set_context_version(7);
  ASSERT_TRUE(client.Call(caret, 1s));
  EXPECT_EQ(invalidations, after_hello + 1U);
  ASSERT_TRUE(client.Call(caret, 1s));
  EXPECT_EQ(invalidations, after_hello + 1U);
  client.Close(); server.request_stop();
}


TEST(InputHostPipe, DelayedPredictionDoesNotReappearAfterCaretMove) {
  for (bool moved : {false, true}) {
    SCOPED_TRACE(moved ? "changed caret" : "identical post-commit caret");
    FakeInputEngine engine;
    RecordingRecommendationPresenter presenter;
    std::unique_ptr<PredictionRuntime> runtime;
    InputHostServer host(engine, nullptr, {}, [&] { if (runtime) runtime->Invalidate(); }, &presenter);
    std::promise<void> started, release;
    auto started_future = started.get_future();
    auto release_future = release.get_future();
    runtime = std::make_unique<PredictionRuntime>(
        [&](const ConfirmedCommit&) -> std::optional<protocol::v1::PredictionResult> {
          started.set_value();
          release_future.wait_for(2s); // Bounded even if a test assertion exits early.
          protocol::v1::PredictionResult result;
          result.set_final_result(true); result.add_candidates()->set_text("late");
          return result;
        }, [&](protocol::v1::PredictionResult result, std::uint64_t version) {
          host.OnPredictionResult(std::move(result), version);
        }, 0ms);
    std::jthread server([&](std::stop_token stop) {
      static_cast<void>(host.Serve(L"prediction-delayed-caret", stop));
    });
    const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(L"prediction-delayed-caret");
    ASSERT_TRUE(endpoint);
    ipc::NamedPipeClient client;
    ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
    ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
    auto caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2);
    caret.set_context_version(7);
    ASSERT_TRUE(client.Call(caret, 1s));
    runtime->Submit({.text="commit",.context_version=7,.allow_prediction=true});
    ASSERT_EQ(started_future.wait_for(1s), std::future_status::ready);
    auto next = Caret();
    if (moved) { next.mutable_caret()->set_left(200); next.mutable_caret()->set_right(202); }
    caret = Request(protocol::v1::CARET_UPDATE, next, 3);
    caret.set_context_version(7);
    ASSERT_TRUE(client.Call(caret, 1s));
    release.set_value();
    runtime.reset(); // Joins the actual prediction worker, making publication assertions deterministic.
    EXPECT_EQ(presenter.present_calls, moved ? 0U : 1U);
    client.Close(); server.request_stop();
  }
}


TEST(InputHostPipe, FirstPostCommitCaretPreservesFreshPredictionButLaterMoveCancelsIt) {
  for (bool external_move : {false, true}) {
    SCOPED_TRACE(external_move ? "later external move" : "first post-commit caret");
    FakeInputEngine engine;
    RecordingRecommendationPresenter presenter;
    std::unique_ptr<PredictionRuntime> runtime;
    InputHostServer host(engine, nullptr,
        [&](ConfirmedCommit commit) { runtime->Submit(std::move(commit)); },
        [&] { if (runtime) runtime->Invalidate(); }, &presenter);
    std::promise<void> started, release;
    auto started_future=started.get_future(), release_future=release.get_future();
    runtime=std::make_unique<PredictionRuntime>(
        [&](const ConfirmedCommit&) -> std::optional<protocol::v1::PredictionResult> {
          started.set_value(); release_future.wait_for(2s);
          protocol::v1::PredictionResult result;
          result.set_final_result(true); result.add_candidates()->set_text("fresh");
          return result;
        }, [&](protocol::v1::PredictionResult result, std::uint64_t version) {
          host.OnPredictionResult(std::move(result), version);
        }, 0ms);
    std::jthread server([&](std::stop_token stop) {
      static_cast<void>(host.Serve(L"prediction-post-commit-caret",stop));
    });
    const auto endpoint=ipc::PipeEndpoint::ForCurrentUser(L"prediction-post-commit-caret");
    ASSERT_TRUE(endpoint);
    ipc::NamedPipeClient client;
    ASSERT_TRUE(ConnectWithRetry(client,*endpoint));
    ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO,Hello(),1),1s));
    ASSERT_TRUE(client.Call(Request(protocol::v1::CARET_UPDATE,Caret(),2),1s));
    protocol::v1::KeyEvent choose;
    choose.set_key_code('1'); choose.set_key_down(true);
    choose.mutable_privacy()->set_allow_composition(true);
    choose.mutable_privacy()->set_allow_prediction(true);
    const auto response=client.Call(Request(protocol::v1::KEY_EVENT,choose,3),1s);
    ASSERT_TRUE(response);
    protocol::v1::EngineAction action;
    ASSERT_TRUE(action.ParseFromString(response->payload()));
    ASSERT_TRUE(action.has_commit_request());
    protocol::v1::CommitApplied applied;
    applied.set_request_id(action.commit_request().request_id()); applied.set_context_version(1);
    ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED,applied,4),1s));
    ASSERT_EQ(started_future.wait_for(1s),std::future_status::ready);
    auto after_commit=Caret();
    after_commit.mutable_caret()->set_left(200); after_commit.mutable_caret()->set_right(202);
    auto caret=Request(protocol::v1::CARET_UPDATE,after_commit,5); caret.set_context_version(1);
    ASSERT_TRUE(client.Call(caret,1s));
    if(external_move){
      // TSF reports a genuine selection move with a new document generation.
      protocol::v1::SessionInvalidated moved;moved.set_reason(protocol::v1::CARET_MOVED);
      auto invalidation=Request(protocol::v1::SESSION_INVALIDATED,moved,6);invalidation.set_context_version(2);
      ASSERT_TRUE(client.Call(invalidation,1s));
      after_commit.mutable_caret()->set_left(300); after_commit.mutable_caret()->set_right(302);
      caret=Request(protocol::v1::CARET_UPDATE,after_commit,7); caret.set_context_version(2);
      ASSERT_TRUE(client.Call(caret,1s));
    }
    release.set_value(); runtime.reset();
    EXPECT_EQ(presenter.present_calls,external_move?0U:1U);
    client.Close(); server.request_stop();
  }
}


TEST(InputHostServer, IdenticalCaretUpdatePreservesQueuedCandidateSelection) {
  MenuInputEngine engine;
  RecordingCandidatePresenter presenter;
  InputHostServer host(engine,&presenter);
  std::jthread server([&](std::stop_token stop){static_cast<void>(host.Serve(L"candidate-same-caret-click",stop));});
  const auto endpoint=ipc::PipeEndpoint::ForCurrentUser(L"candidate-same-caret-click");ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;ASSERT_TRUE(ConnectWithRetry(client,*endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO,Hello(),1),1s));
  const auto caret=Request(protocol::v1::CARET_UPDATE,Caret(),2);
  ASSERT_TRUE(client.Call(caret,1s));
  protocol::v1::KeyEvent key;key.set_key_code('N');key.set_key_down(true);
  key.mutable_privacy()->set_allow_composition(true);key.mutable_privacy()->set_allow_prediction(true);
  ASSERT_TRUE(client.Call(Request(protocol::v1::KEY_EVENT,key,3),1s));
  ASSERT_TRUE(presenter.last_presentation);ASSERT_TRUE(presenter.last_presentation->on_accept);
  presenter.last_presentation->on_accept(7);
  ASSERT_TRUE(client.Call(caret,1s)); // No position, DPI, version, session or foreground change.
  protocol::v1::PollRecommendation poll;*poll.mutable_privacy()=key.privacy();
  const auto response=client.Call(Request(protocol::v1::POLL_RECOMMENDATION,poll,4),1s);ASSERT_TRUE(response);
  protocol::v1::EngineAction action;ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().text(),"word7");EXPECT_EQ(engine.selections,1U);
  client.Close();server.request_stop();
}

TEST(InputHostPipe, PolicyChangeRejectsOldPublicationAndMouseAcceptance) {
 FakeInputEngine engine;RecordingRecommendationPresenter presenter;std::string generation="one";
 InputHostServer host(engine,nullptr,{}, {}, &presenter,false,[&]{return generation;});
 std::jthread server([&](std::stop_token stop){static_cast<void>(host.Serve(L"corpus-policy-test",stop));});
 const auto endpoint=ipc::PipeEndpoint::ForCurrentUser(L"corpus-policy-test");ASSERT_TRUE(endpoint);
 ipc::NamedPipeClient client;ASSERT_TRUE(ConnectWithRetry(client,*endpoint));
 ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO,Hello(),1),1s));
 ASSERT_TRUE(client.Call(Request(protocol::v1::CARET_UPDATE,Caret(),2),1s));
 protocol::v1::PredictionResult result;result.set_final_result(true);result.set_policy_generation("one");result.add_candidates()->set_text("old");
 host.OnPredictionResult(result,0);ASSERT_EQ(presenter.present_calls,1u);
 generation="two";host.OnRecommendationSelected(1,0);
 protocol::v1::PollRecommendation poll;poll.mutable_privacy()->set_allow_composition(true);poll.mutable_privacy()->set_allow_prediction(true);
 auto response=client.Call(Request(protocol::v1::POLL_RECOMMENDATION,poll,3),1s);ASSERT_TRUE(response);
 protocol::v1::EngineAction action;ASSERT_TRUE(action.ParseFromString(response->payload()));EXPECT_FALSE(action.has_commit_request());
 host.OnPredictionResult(result,0);EXPECT_EQ(presenter.present_calls,1u);
 client.Close();server.request_stop();
}

protocol::v1::PredictionResult PageResults(unsigned int count) {
  protocol::v1::PredictionResult result;
  result.set_final_result(true);
  for (unsigned int i = 0; i < count; ++i) result.add_candidates()->set_text("option-" + std::to_string(i + 1));
  return result;
}

TEST(RecommendationPresentation, PresentsRequestedPageAndCapsAtThirty) {
  RecordingRecommendationPresenter presenter;
  ui::RecommendationPresentationController controller(&presenter);
  controller.OnCaretUpdate(Caret(), 7);
  int direction = 0;
  EXPECT_TRUE(controller.OnPredictionResult(PageResults(35), 7, {}, 4, [&](int value) { direction = value; }));
  ASSERT_TRUE(presenter.last_presentation);
  EXPECT_EQ(presenter.last_presentation->page, 4U);
  EXPECT_EQ(presenter.last_presentation->page_count, 5U);
  EXPECT_EQ(presenter.last_presentation->recommendations,
            (std::vector<std::wstring>{L"option-25", L"option-26", L"option-27", L"option-28", L"option-29", L"option-30"}));
  ASSERT_TRUE(presenter.last_presentation->on_page);
  presenter.last_presentation->on_page(-1);
  EXPECT_EQ(direction, -1);
  EXPECT_FALSE(controller.OnPredictionResult(PageResults(7), 7, {}, 3));
}

TEST(InputHostPipe, RecommendationPagingKeepsVersionAndAltAcceptsCurrentPage) {
  FakeInputEngine engine;
  RecordingRecommendationPresenter presenter;
  InputHostServer host(engine, nullptr, {}, {}, &presenter);
  constexpr auto role = L"recommendation-paging-current-page";
  std::jthread server([&](std::stop_token stop) { EXPECT_TRUE(host.Serve(role, stop)); });
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  auto caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2);
  caret.set_context_version(7);
  ASSERT_TRUE(client.Call(caret, 1s));
  host.OnPredictionResult(PageResults(13), 7);
  ASSERT_TRUE(presenter.last_presentation);
  EXPECT_EQ(presenter.last_presentation->page_count, 3U);
  auto old_accept = presenter.last_presentation->on_accept;
  auto old_page = presenter.last_presentation->on_page;
  ASSERT_TRUE(old_page);
  old_page(1);
  EXPECT_EQ(presenter.last_presentation->page, 1U);
  EXPECT_EQ(presenter.last_presentation->recommendations,
            (std::vector<std::wstring>{L"option-7", L"option-8", L"option-9", L"option-10", L"option-11", L"option-12"}));
  old_page(1); // Old UI callbacks cannot advance the new page.
  old_accept(0);
  EXPECT_EQ(presenter.last_presentation->page, 1U);
  protocol::v1::PollRecommendation poll;
  poll.mutable_privacy()->set_allow_composition(true);
  poll.mutable_privacy()->set_allow_prediction(true);
  auto poll_request = Request(protocol::v1::POLL_RECOMMENDATION, poll, 3);
  poll_request.set_context_version(7);
  auto response = client.Call(poll_request, 1s);
  ASSERT_TRUE(response);
  protocol::v1::EngineAction action;
  ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_FALSE(action.has_commit_request());
  protocol::v1::KeyEvent key;
  key.set_key_down(true);
  key.set_key_code(VK_OEM_PLUS); key.set_modifiers(1);
  key.mutable_privacy()->set_allow_composition(true);
  key.mutable_privacy()->set_allow_prediction(true);
  auto key_request = Request(protocol::v1::KEY_EVENT, key, 4);
  key_request.set_context_version(7);
  response = client.Call(key_request, 1s);
  ASSERT_TRUE(response);
  ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_TRUE(action.consumed());
  EXPECT_FALSE(action.has_commit_request());
  EXPECT_EQ(presenter.last_presentation->page, 2U);
  EXPECT_EQ(presenter.last_presentation->recommendations, (std::vector<std::wstring>{L"option-13"}));
  response = client.Call(key_request, 1s); // Last-page boundary cannot move the application caret.
  ASSERT_TRUE(response);
  ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_TRUE(action.consumed());
  EXPECT_EQ(presenter.last_presentation->page, 2U);
  key.set_key_code(VK_OEM_MINUS); key.set_modifiers(1);
  key_request = Request(protocol::v1::KEY_EVENT, key, 5); key_request.set_context_version(7);
  response = client.Call(key_request, 1s); ASSERT_TRUE(response);
  ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_TRUE(action.consumed()); EXPECT_FALSE(action.has_commit_request());
  EXPECT_EQ(presenter.last_presentation->page, 1U);
  key.set_key_code('6'); key.set_modifiers(1);
  key_request = Request(protocol::v1::KEY_EVENT, key, 5);
  key_request.set_context_version(7);
  response = client.Call(key_request, 1s);
  ASSERT_TRUE(response);
  ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().text(), "option-12");
  EXPECT_EQ(action.commit_request().expected_context_version(), 7U);
  client.Close(); server.request_stop();
}

TEST(InputHostPipe, MousePagingSelectsGlobalIndexAndInvalidationRevokesCallbacks) {
  FakeInputEngine engine;
  RecordingRecommendationPresenter presenter;
  InputHostServer host(engine, nullptr, {}, {}, &presenter);
  constexpr auto role = L"recommendation-mouse-page";
  std::jthread server([&](std::stop_token stop) { EXPECT_TRUE(host.Serve(role, stop)); });
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  auto caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2); caret.set_context_version(7);
  ASSERT_TRUE(client.Call(caret, 1s));
  host.OnPredictionResult(PageResults(35), 7);
  ASSERT_TRUE(presenter.last_presentation);
  EXPECT_EQ(presenter.last_presentation->page_count, 5U);
  for (int i = 0; i < 4; ++i) { auto page = presenter.last_presentation->on_page; ASSERT_TRUE(page); page(1); }
  EXPECT_EQ(presenter.last_presentation->page, 4U);
  auto click = presenter.last_presentation->on_accept;
  ASSERT_TRUE(click); click(5);
  protocol::v1::PollRecommendation poll; poll.mutable_privacy()->set_allow_composition(true); poll.mutable_privacy()->set_allow_prediction(true);
  auto request = Request(protocol::v1::POLL_RECOMMENDATION, poll, 3); request.set_context_version(7);
  auto response = client.Call(request, 1s); ASSERT_TRUE(response);
  protocol::v1::EngineAction action; ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.has_commit_request()); EXPECT_EQ(action.commit_request().text(), "option-30");
  // New presentation invalidates every callback from an old prediction batch.
  host.OnPredictionResult(PageResults(7), 7);
  auto expired_page = presenter.last_presentation->on_page;
  auto next_caret = caret; next_caret.set_context_version(8);
  ASSERT_TRUE(client.Call(next_caret, 1s));
  const auto count = presenter.present_calls;
  expired_page(1); click(0);
  EXPECT_EQ(presenter.present_calls, count);
  client.Close(); server.request_stop();
}

}  // namespace zhixu::input_host

namespace zhixu::input_host {
namespace {
TEST(RecommendationPresentation, IdenticalCaretKeepsVisibleBatch) {
  RecordingRecommendationPresenter presenter;
  ui::RecommendationPresentationController controller(&presenter);
  controller.OnCaretUpdate(Caret(), 1);
  protocol::v1::PredictionResult result;
  result.set_final_result(true); result.add_candidates()->set_text("黄河之水天上来");
  ASSERT_TRUE(controller.OnPredictionResult(result, 1));
  controller.OnCaretUpdate(Caret(), 1);
  EXPECT_EQ(presenter.hide_calls, 0u);
  EXPECT_EQ(presenter.present_calls, 1u);
}

TEST(InputHostPipe, DuplicateCaretPreservesRecommendationAndAltAcceptance) {
  FakeInputEngine engine;
  RecordingRecommendationPresenter presenter;
  InputHostServer host(engine, nullptr, {}, {}, &presenter);
  std::jthread loop([&](std::stop_token stop) { static_cast<void>(host.Serve(L"comma-duplicate", stop)); });
  const auto endpoint=ipc::PipeEndpoint::ForCurrentUser(L"comma-duplicate"); ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client; ASSERT_TRUE(ConnectWithRetry(client,*endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO,Hello(),1),1s));
  auto caret=Request(protocol::v1::CARET_UPDATE,Caret(),2); caret.set_context_version(1);
  ASSERT_TRUE(client.Call(caret,1s));
  protocol::v1::PredictionResult result;result.set_final_result(true);result.add_candidates()->set_text("黄河之水天上来");
  host.OnPredictionResult(result,1); ASSERT_EQ(presenter.present_calls,1u);
  const auto hides=presenter.hide_calls;
  caret.set_request_id(3); ASSERT_TRUE(client.Call(caret,1s));
  EXPECT_EQ(presenter.hide_calls,hides);
  protocol::v1::KeyEvent key;key.set_key_code('1');key.set_key_down(true);key.set_modifiers(1);
  key.mutable_privacy()->set_allow_composition(true);key.mutable_privacy()->set_allow_prediction(true);
  auto request=Request(protocol::v1::KEY_EVENT,key,4);request.set_context_version(1);
  const auto response=client.Call(request,1s);ASSERT_TRUE(response);
  protocol::v1::EngineAction action;ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.has_commit_request());EXPECT_EQ(action.commit_request().text(),"黄河之水天上来");
  client.Close();loop.request_stop();
}

TEST(InputHostPipe, EarlyResultWaitsForCommittedCaretAndNeverSurvivesInvalidation) {
  for(int scenario=0;scenario<8;++scenario) {
    SCOPED_TRACE(scenario); // success, focus, policy, expiry, invisible key/click, external edit, continued input
    FakeInputEngine engine;RecordingRecommendationPresenter presenter;
    std::string policy="one";
    InputHostServer host(engine,nullptr,{}, {},&presenter,false,[&]{return policy;});
    std::jthread loop([&](std::stop_token stop){static_cast<void>(host.Serve(L"comma-early",stop));});
    const auto endpoint=ipc::PipeEndpoint::ForCurrentUser(L"comma-early");ASSERT_TRUE(endpoint);
    ipc::NamedPipeClient client;ASSERT_TRUE(ConnectWithRetry(client,*endpoint));
    ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO,Hello(),1),1s));
    ASSERT_TRUE(client.Call(Request(protocol::v1::CARET_UPDATE,Caret(),2),1s));
    protocol::v1::KeyEvent choose;choose.set_key_code('1');choose.set_key_down(true);
    choose.mutable_privacy()->set_allow_composition(true);choose.mutable_privacy()->set_allow_prediction(true);
    const auto chosen=client.Call(Request(protocol::v1::KEY_EVENT,choose,3),1s);ASSERT_TRUE(chosen);
    protocol::v1::EngineAction action;ASSERT_TRUE(action.ParseFromString(chosen->payload()));ASSERT_TRUE(action.has_commit_request());
    protocol::v1::CommitApplied applied;applied.set_request_id(action.commit_request().request_id());applied.set_context_version(1);
    ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED,applied,4),1s));
    protocol::v1::PredictionResult result;result.set_final_result(true);result.set_policy_generation("one");result.add_candidates()->set_text("黄河之水天上来");
    host.OnPredictionResult(result,1);ASSERT_EQ(presenter.present_calls,0u);
    if(scenario==1){protocol::v1::SessionInvalidated invalidation;invalidation.set_reason(protocol::v1::FOCUS_CHANGED);ASSERT_TRUE(client.Call(Request(protocol::v1::SESSION_INVALIDATED,invalidation,5),1s));}
    if(scenario==2)policy="two";
    if(scenario==4) {
      choose.set_modifiers(1);auto key=Request(protocol::v1::KEY_EVENT,choose,5);key.set_context_version(1);
      const auto response=client.Call(key,1s);ASSERT_TRUE(response);
      protocol::v1::EngineAction invisible;ASSERT_TRUE(invisible.ParseFromString(response->payload()));EXPECT_FALSE(invisible.has_commit_request());
    }
    if(scenario==5) {
      host.OnRecommendationSelected(1,0);
      protocol::v1::PollRecommendation poll;poll.mutable_privacy()->set_allow_composition(true);poll.mutable_privacy()->set_allow_prediction(true);
      auto request=Request(protocol::v1::POLL_RECOMMENDATION,poll,5);request.set_context_version(1);
      const auto response=client.Call(request,1s);ASSERT_TRUE(response);
      protocol::v1::EngineAction invisible;ASSERT_TRUE(invisible.ParseFromString(response->payload()));EXPECT_FALSE(invisible.has_commit_request());
      protocol::v1::SessionInvalidated invalidation;invalidation.set_reason(protocol::v1::FOCUS_CHANGED);
      ASSERT_TRUE(client.Call(Request(protocol::v1::SESSION_INVALIDATED,invalidation,8),1s));
    }
    if(scenario==6 || scenario==7) {
      protocol::v1::SessionInvalidated invalidation;
      invalidation.set_reason(scenario==6?protocol::v1::EXTERNAL_EDIT:protocol::v1::CONTINUED_INPUT);
      ASSERT_TRUE(client.Call(Request(protocol::v1::SESSION_INVALIDATED,invalidation,5),1s));
    }
    if(scenario==3)std::this_thread::sleep_for(550ms);
    auto caret=Request(protocol::v1::CARET_UPDATE,Caret(),6);caret.set_context_version(1);ASSERT_TRUE(client.Call(caret,1s));
    EXPECT_EQ(presenter.present_calls,scenario==0?1u:0u);
    if(scenario==0){caret.set_request_id(7);ASSERT_TRUE(client.Call(caret,1s));EXPECT_EQ(presenter.hide_calls,0u);}
    client.Close();loop.request_stop();
  }
}
}
}

namespace zhixu::input_host {
struct LayoutHandleTag {
  using type=protocol::Envelope(InputHostServer::*)(const protocol::Envelope&);
  friend type LayoutHandle(LayoutHandleTag);
};
template<class Tag, typename Tag::type Member> struct LayoutAccess {
  friend typename Tag::type LayoutHandle(Tag){return Member;}
};
template struct LayoutAccess<LayoutHandleTag,&InputHostServer::Handle>;
namespace {
struct DirectLayoutClient {
 InputHostServer& host;
 std::optional<protocol::Envelope> Call(const protocol::Envelope& request,std::chrono::milliseconds){
  return (host.*LayoutHandle(LayoutHandleTag{}))(request);
 }
};
void ConfirmLayoutCommit(DirectLayoutClient& client,std::uint64_t foreground=0){
 ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO,Hello(),1),1s));
 if(foreground){auto caret=Caret();caret.set_foreground_window(foreground);ASSERT_TRUE(client.Call(Request(protocol::v1::CARET_UPDATE,caret,10),1s));}
 protocol::v1::KeyEvent choose;choose.set_key_code('1');choose.set_key_down(true);
 choose.mutable_privacy()->set_allow_composition(true);choose.mutable_privacy()->set_allow_prediction(true);
 const auto reply=client.Call(Request(protocol::v1::KEY_EVENT,choose,2),1s);ASSERT_TRUE(reply);
 protocol::v1::EngineAction action;ASSERT_TRUE(action.ParseFromString(reply->payload()));ASSERT_TRUE(action.has_commit_request());
 protocol::v1::CommitApplied applied;applied.set_request_id(action.commit_request().request_id());applied.set_context_version(1);
 ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED,applied,3),1s));
}
protocol::v1::PredictionResult LayoutResult(){protocol::v1::PredictionResult r;r.set_final_result(true);r.add_candidates()->set_text("黄河之水天上来");return r;}
TEST(LayoutRegression, TwoSameVersionCommitCaretsDoNotCancelPendingPrediction){
 FakeInputEngine engine;RecordingRecommendationPresenter presenter;unsigned cancellations=0;
 InputHostServer host(engine,nullptr,[](ConfirmedCommit){},[&]{++cancellations;},&presenter);
 DirectLayoutClient client{host};ConfirmLayoutCommit(client);const auto before=cancellations;
 auto first=Request(protocol::v1::CARET_UPDATE,Caret(),4);first.set_context_version(1);ASSERT_TRUE(client.Call(first,1s));
 auto second=first;protocol::v1::CaretUpdate moved=Caret();moved.mutable_caret()->set_left(104);moved.mutable_caret()->set_right(104);moved.SerializeToString(second.mutable_payload());second.set_request_id(5);
 ASSERT_TRUE(client.Call(second,1s));
 EXPECT_EQ(cancellations,before)<<"Same committed version is a layout update, not a user edit";
 host.OnPredictionResult(LayoutResult(),1);EXPECT_EQ(presenter.present_calls,1u);
}
TEST(LayoutRegression, EarlyResultSurvivesFirstAndSecondCommitCaret){
 FakeInputEngine engine;RecordingRecommendationPresenter presenter;
 InputHostServer host(engine,nullptr,{}, {},&presenter);DirectLayoutClient client{host};ConfirmLayoutCommit(client);
 host.OnPredictionResult(LayoutResult(),1);EXPECT_EQ(presenter.present_calls,0u);
 auto first=Request(protocol::v1::CARET_UPDATE,Caret(),4);first.set_context_version(1);ASSERT_TRUE(client.Call(first,1s));
 EXPECT_EQ(presenter.present_calls,1u);
 // Once visible, a later physical move must revoke the old callbacks.
 auto moved=Caret();moved.mutable_caret()->set_left(110);moved.mutable_caret()->set_right(110);
 auto second=Request(protocol::v1::CARET_UPDATE,moved,5);second.set_context_version(1);ASSERT_TRUE(client.Call(second,1s));
 EXPECT_GE(presenter.hide_calls,1u);
}
TEST(LayoutRegression, ExplicitEditAndIdentityChangesStillCancel){
 for(int scenario=0;scenario<5;++scenario){
  SCOPED_TRACE(scenario);FakeInputEngine engine;RecordingRecommendationPresenter presenter;unsigned cancellations=0;
  InputHostServer host(engine,nullptr,[](ConfirmedCommit){},[&]{++cancellations;},&presenter);DirectLayoutClient client{host};ConfirmLayoutCommit(client);
  auto caret=Caret();caret.set_foreground_window(10);
  auto first=Request(protocol::v1::CARET_UPDATE,caret,4);first.set_context_version(1);ASSERT_TRUE(client.Call(first,1s));const auto before=cancellations;
  if(scenario==0){protocol::v1::SessionInvalidated edit;edit.set_reason(protocol::v1::CARET_MOVED);auto req=Request(protocol::v1::SESSION_INVALIDATED,edit,5);req.set_context_version(2);ASSERT_TRUE(client.Call(req,1s));}
  else{
   if(scenario==1)caret.set_foreground_window(20);
   if(scenario==2)caret.mutable_caret()->set_bottom(caret.caret().top());
   if(scenario==3)std::this_thread::sleep_for(550ms);
   caret.mutable_caret()->set_left(110);caret.mutable_caret()->set_right(110);
   auto next=Request(protocol::v1::CARET_UPDATE,caret,5);next.set_context_version(scenario==4?2:1);ASSERT_TRUE(client.Call(next,1s));
  }
  EXPECT_GT(cancellations,before);
 }
}
TEST(LayoutRegression, FirstCommitCaretRejectsWrongIdentityOrExpiredWindow){
 for(int scenario=0;scenario<3;++scenario){
  SCOPED_TRACE(scenario);FakeInputEngine engine;RecordingRecommendationPresenter presenter;unsigned cancellations=0;
  InputHostServer host(engine,nullptr,[](ConfirmedCommit){},[&]{++cancellations;},&presenter);DirectLayoutClient client{host};ConfirmLayoutCommit(client,10);
  host.OnPredictionResult(LayoutResult(),1);ASSERT_EQ(presenter.present_calls,0u);const auto before=cancellations;
  auto caret=Caret();caret.set_foreground_window(scenario==0?20:10);
  if(scenario==2)std::this_thread::sleep_for(550ms);
  auto first=Request(protocol::v1::CARET_UPDATE,caret,5);first.set_context_version(1);if(scenario==1)first.set_session_id("another-session");
  ASSERT_TRUE(client.Call(first,1s));EXPECT_GT(cancellations,before);EXPECT_EQ(presenter.present_calls,0u);
 }
}
}
}

namespace zhixu::input_host { namespace {
TEST(InputHostPipe, ReloadsTabAcceptanceFromSettingsWithoutRestart) {
  FakeInputEngine engine;
  RecordingRecommendationPresenter presenter;
  InputHostServer host(engine, nullptr, {}, {}, &presenter, false);
  host.SetSettingsProvider([]{ auto s=settings::Settings::Defaults();s.tab_accept_enabled=true;return s; });
  constexpr std::wstring_view kRole = L"input-host-runtime-tab";
  std::jthread server([&](const std::stop_token stop_token) {
    const auto served = host.Serve(kRole, stop_token);
    EXPECT_TRUE(served) << static_cast<int>(served.error());
  });

  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(kRole);
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  auto caret = Request(protocol::v1::CARET_UPDATE, Caret(), 2);
  caret.set_context_version(7);
  ASSERT_TRUE(client.Call(caret, 1s));

  protocol::v1::PredictionResult prediction;
  prediction.set_final_result(true);
  prediction.add_candidates()->set_text("first");
  host.OnPredictionResult(prediction, 7);

  protocol::v1::KeyEvent accept;
  accept.set_key_code(0x09);
  accept.set_key_down(true);
  accept.mutable_privacy()->set_allow_prediction(true);
  auto accept_request = Request(protocol::v1::KEY_EVENT, accept, 3);
  accept_request.set_context_version(7);
  const auto response = client.Call(accept_request, 1s);
  ASSERT_TRUE(response);
  protocol::v1::EngineAction action;
  ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().text(), "first");

  client.Close();
  server.request_stop();
}


}}

namespace zhixu::input_host { namespace {
TEST(InputHostServer, PredictionDisabledStillAllowsCandidateMouseSelection) {
  MenuInputEngine engine;
  RecordingCandidatePresenter presenter;
  InputHostServer host(engine, &presenter);
  std::jthread server([&](std::stop_token stop) { static_cast<void>(host.Serve(L"candidate-without-prediction", stop)); });
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(L"candidate-without-prediction");
  ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;
  ASSERT_TRUE(ConnectWithRetry(client, *endpoint));
  ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO, Hello(), 1), 1s));
  ASSERT_TRUE(client.Call(Request(protocol::v1::CARET_UPDATE, Caret(), 2), 1s));
  protocol::v1::KeyEvent key;
  key.set_key_code('N'); key.set_key_down(true);
  key.mutable_privacy()->set_allow_composition(true);
  key.mutable_privacy()->set_allow_prediction(false);
  ASSERT_TRUE(client.Call(Request(protocol::v1::KEY_EVENT, key, 3), 1s));
  ASSERT_TRUE(presenter.last_presentation);
  const auto old_click = presenter.last_presentation->on_accept;
  ASSERT_TRUE(old_click);
  ASSERT_TRUE(client.Call(Request(protocol::v1::KEY_EVENT, key, 4), 1s));
  old_click(7);
  protocol::v1::PollRecommendation poll; *poll.mutable_privacy() = key.privacy();
  auto response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 5), 1s);
  ASSERT_TRUE(response);
  protocol::v1::EngineAction action; ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_FALSE(action.consumed()); EXPECT_EQ(engine.selections, 0U);
  const auto click = presenter.last_presentation->on_accept;
  click(0); click(0);
  response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 6), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_TRUE(action.consumed()); EXPECT_FALSE(action.has_commit_request());
  EXPECT_EQ(action.composition().preedit(), "ni hao"); EXPECT_EQ(engine.selections, 1U);
  presenter.last_presentation->on_accept(7);
  response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 7), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.has_commit_request()); EXPECT_EQ(action.commit_request().text(), "word7");
  const auto committed_id = action.commit_request().request_id();
  EXPECT_EQ(engine.selections, 2U);
  click(0);
  response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 8), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload())); EXPECT_FALSE(action.consumed());
  protocol::v1::CommitApplied applied;
  applied.set_request_id(committed_id); applied.set_context_version(1);
  ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED, applied, 9), 1s));
  // Legacy keyboard requests may omit the latest version. A mouse snapshot
  // must still match the server's authoritative version before selecting.
  response = client.Call(Request(protocol::v1::KEY_EVENT, key, 10), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload()));
  ASSERT_TRUE(action.consumed()); ASSERT_EQ(action.composition().candidates_size(), 8);
  presenter.last_presentation->on_accept(7);
  response = client.Call(Request(protocol::v1::POLL_RECOMMENDATION, poll, 11), 1s);
  ASSERT_TRUE(response); ASSERT_TRUE(action.ParseFromString(response->payload()));
  EXPECT_FALSE(action.has_commit_request()); EXPECT_EQ(engine.selections, 2U);
  client.Close(); server.request_stop();
}


}}

namespace zhixu::input_host { namespace {
TEST(InputHostPipe, ChangedSettingsBeforeAcknowledgmentSuppressLearningAndPrediction) {
  FakeInputEngine engine; std::string policy="before"; std::vector<ConfirmedCommit> observed;
  InputHostServer host(engine,nullptr,[&](ConfirmedCommit c){observed.push_back(std::move(c));},{},nullptr,false,[&]{return policy;});
  std::jthread server([&](std::stop_token stop){static_cast<void>(host.Serve(L"settings-ack-race",stop));});
  const auto endpoint=ipc::PipeEndpoint::ForCurrentUser(L"settings-ack-race");ASSERT_TRUE(endpoint);
  ipc::NamedPipeClient client;ASSERT_TRUE(ConnectWithRetry(client,*endpoint));ASSERT_TRUE(client.Call(Request(protocol::v1::HELLO,Hello(),1),1s));
  protocol::v1::KeyEvent key;key.set_key_code('1');key.set_key_down(true);key.mutable_privacy()->set_allow_learning(true);key.mutable_privacy()->set_allow_prediction(true);
  const auto response=client.Call(Request(protocol::v1::KEY_EVENT,key,2),1s);ASSERT_TRUE(response);
  protocol::v1::EngineAction action;ASSERT_TRUE(action.ParseFromString(response->payload()));ASSERT_TRUE(action.has_commit_request());
  policy="excluded-now";
  protocol::v1::CommitApplied applied;applied.set_request_id(action.commit_request().request_id());applied.set_context_version(1);
  ASSERT_TRUE(client.Call(Request(protocol::v1::COMMIT_APPLIED,applied,3),1s));EXPECT_TRUE(observed.empty());
  client.Close();server.request_stop();
}
}}
