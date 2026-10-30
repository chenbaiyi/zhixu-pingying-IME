#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "input_host/session/session_controller.h"

namespace zhixu::input_host {
namespace {

constexpr uint32_t kLetterA = 'A';
constexpr uint32_t kBackspace = 0x08;
constexpr uint32_t kEscape = 0x1B;
constexpr uint32_t kSpace = 0x20;
constexpr uint32_t kPageDown = 0x22;
constexpr uint32_t kToggleAscii = 0x71;
constexpr uint32_t kTogglePunctuation = 0x72;

class FakeInputEngine final : public InputEngine {
 public:
  EngineState ProcessKey(const EngineKey& key) override {
    processed.push_back(key.virtual_key);
    return state;
  }

  CommitOrState SelectCandidate(size_t index) override {
    selected.push_back(index);
    return {.state = state, .commit_text = commit};
  }

  EngineState state{.consumed = true, .preedit = "ni", .candidate_text = {"你", "呢"}};
  std::optional<std::string> commit;
  std::vector<uint32_t> processed;
  std::vector<size_t> selected;
};

protocol::v1::KeyEvent Key(const uint32_t code) {
  protocol::v1::KeyEvent event;
  event.set_key_code(code);
  event.set_key_down(true);
  event.mutable_privacy()->set_allow_composition(true);
  return event;
}

TEST(SessionController, ForwardsEditingAndModeKeysToEngine) {
  FakeInputEngine engine;
  SessionController session(engine);

  for (const auto key : {kLetterA, kBackspace, kEscape, kPageDown, kToggleAscii, kTogglePunctuation}) {
    const auto action = session.Handle(Key(key));
    EXPECT_TRUE(action.consumed());
    EXPECT_EQ(action.composition().preedit(), "ni");
  }
  EXPECT_EQ(engine.processed, (std::vector<uint32_t>{
                                  kLetterA, kBackspace, kEscape, kPageDown, kToggleAscii, kTogglePunctuation}));
  EXPECT_EQ(session.context_version(), 0);
}

TEST(SessionController, ResetDoesNotReuseCommitRequestIds) {
  FakeInputEngine engine;
  engine.commit = "你";
  SessionController session(engine);
  const auto first = session.Handle(Key('1'), 3);
  ASSERT_TRUE(first.has_commit_request());
  session.Reset();
  const auto second = session.Handle(Key('1'), 5);
  ASSERT_TRUE(second.has_commit_request());
  EXPECT_GT(second.commit_request().request_id(), first.commit_request().request_id());
  EXPECT_EQ(second.commit_request().expected_context_version(), 5U);
}

TEST(SessionController, PendingAsyncCommitReleasesNewKeyWithoutAdvancingEngine) {
  FakeInputEngine engine;
  engine.commit = "你";
  SessionController session(engine);
  ASSERT_TRUE(session.Handle(Key('1')).has_commit_request());
  const auto action = session.Handle(Key('A'));
  EXPECT_FALSE(action.consumed());
  EXPECT_FALSE(action.has_commit_request());
  EXPECT_TRUE(engine.processed.empty());
}

TEST(SessionController, SelectingCandidateCreatesOnePendingCommitWithoutChangingVersion) {
  FakeInputEngine engine;
  engine.commit = "\xE4\xB8\xAD\xE6\x96\x87";
  SessionController session(engine);

  const auto first = session.Handle(Key('2'));
  ASSERT_TRUE(first.has_commit_request());
  EXPECT_EQ(first.commit_request().text(), engine.commit);
  EXPECT_NE(first.commit_request().request_id(), 0u);
  EXPECT_EQ(first.commit_request().expected_context_version(), 0);
  EXPECT_EQ(session.context_version(), 0);
  EXPECT_EQ(engine.selected, (std::vector<size_t>{1}));

  const auto second = session.Handle(Key('1'));
  EXPECT_FALSE(second.has_commit_request());
  EXPECT_FALSE(second.consumed());
  EXPECT_EQ(engine.selected, (std::vector<size_t>{1}));
}

TEST(SessionController, SpaceSelectsFirstCandidateBeforeForwardingToEngine) {
  FakeInputEngine engine;
  engine.commit = "\xE4\xBD\xA0";
  SessionController session(engine);

  const auto composing = session.Handle(Key(kLetterA));
  ASSERT_FALSE(composing.has_commit_request());

  const auto action = session.Handle(Key(kSpace));

  ASSERT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().text(), engine.commit);
  EXPECT_NE(action.commit_request().request_id(), 0u);
  EXPECT_EQ(engine.selected, (std::vector<size_t>{0}));
  EXPECT_EQ(engine.processed, (std::vector<uint32_t>{kLetterA}));
}

TEST(SessionController, SynchronizesCommitVersionFromTheTsfKeyEvent) {
  FakeInputEngine engine;
  engine.commit = "commit";
  SessionController session(engine);
  auto event = Key('1');

  const auto action = session.Handle(event, 7);

  ASSERT_TRUE(action.has_commit_request());
  EXPECT_EQ(action.commit_request().expected_context_version(), 7);
  EXPECT_EQ(session.context_version(), 7);
}

TEST(SessionController, CandidateKeyReleaseCannotCreateADuplicateCommit) {
  FakeInputEngine engine;
  engine.commit = "你";
  SessionController session(engine);
  static_cast<void>(session.Handle(Key(kLetterA)));
  auto released = Key(kSpace);
  released.set_key_down(false);
  const auto action = session.Handle(released);
  EXPECT_FALSE(action.consumed());
  EXPECT_FALSE(action.has_commit_request());
  EXPECT_TRUE(engine.selected.empty());
  EXPECT_EQ(engine.processed, (std::vector<uint32_t>{kLetterA}));
}

TEST(SessionController, AltNumberIsNotAnOrdinaryCandidateSelection) {
  FakeInputEngine engine;
  engine.commit = "你";
  SessionController session(engine);
  static_cast<void>(session.Handle(Key(kLetterA)));
  auto modified = Key('1');
  modified.set_modifiers(1);
  const auto action = session.Handle(modified);
  EXPECT_FALSE(action.has_commit_request());
  EXPECT_TRUE(engine.selected.empty());
  EXPECT_EQ(engine.processed, (std::vector<uint32_t>{kLetterA, '1'}));
}

TEST(SessionController, AcceptsOnlyMatchingNextVersionCommitApplied) {
  FakeInputEngine engine;
  engine.commit = "commit";
  SessionController session(engine);
  const auto action = session.Handle(Key('1'));
  ASSERT_TRUE(action.has_commit_request());

  protocol::v1::CommitApplied stale_request;
  stale_request.set_request_id(99);
  stale_request.set_context_version(1);
  static_cast<void>(session.OnCommitApplied(stale_request));
  EXPECT_EQ(session.context_version(), 0);

  protocol::v1::CommitApplied stale_version;
  stale_version.set_request_id(action.commit_request().request_id());
  stale_version.set_context_version(2);
  static_cast<void>(session.OnCommitApplied(stale_version));
  EXPECT_EQ(session.context_version(), 0);

  protocol::v1::CommitApplied applied;
  applied.set_request_id(action.commit_request().request_id());
  applied.set_context_version(1);
  static_cast<void>(session.OnCommitApplied(applied));
  EXPECT_EQ(session.context_version(), 1);

  static_cast<void>(session.OnCommitApplied(applied));
  EXPECT_EQ(session.context_version(), 1);
}

TEST(SessionController, EmitsConfirmedTextAndPrivacyOnlyOnceAfterMatchingCommitApplied) {
  FakeInputEngine engine;
  engine.commit = "commit";
  SessionController session(engine);
  auto event = Key('1');
  event.mutable_privacy()->set_allow_learning(true);
  event.mutable_privacy()->set_allow_prediction(true);
  const auto action = session.Handle(event);

  protocol::v1::CommitApplied stale;
  stale.set_request_id(99);
  stale.set_context_version(1);
  EXPECT_FALSE(session.OnCommitApplied(stale).has_value());

  protocol::v1::CommitApplied applied;
  applied.set_request_id(action.commit_request().request_id());
  applied.set_context_version(1);
  const auto confirmed = session.OnCommitApplied(applied);
  ASSERT_TRUE(confirmed.has_value());
  EXPECT_EQ(confirmed->text, "commit");
  EXPECT_EQ(confirmed->context_version, 1U);
  EXPECT_TRUE(confirmed->allow_learning);
  EXPECT_TRUE(confirmed->allow_prediction);
  EXPECT_FALSE(session.OnCommitApplied(applied).has_value());
}

TEST(SessionController, RejectionClearsOnlyMatchingPendingCommit) {
  FakeInputEngine engine;
  engine.commit = "commit";
  SessionController session(engine);
  const auto action = session.Handle(Key('1'));
  ASSERT_TRUE(action.has_commit_request());

  protocol::v1::CommitRejected stale;
  stale.set_request_id(22);
  session.OnCommitRejected(stale);
  EXPECT_FALSE(session.Handle(Key('2')).has_commit_request());

  protocol::v1::CommitRejected rejected;
  rejected.set_request_id(action.commit_request().request_id());
  session.OnCommitRejected(rejected);
  const auto retry = session.Handle(Key('2'));
  ASSERT_TRUE(retry.has_commit_request());
  EXPECT_EQ(retry.commit_request().request_id(), action.commit_request().request_id()+1);
  EXPECT_EQ(session.context_version(), 0);
}

TEST(SessionController, AccumulatesOnlyConfirmedContextAndClearsOnReset) {
 FakeInputEngine engine; SessionController session(engine);
 auto key=Key('1');key.mutable_privacy()->set_allow_prediction(true);
 std::uint64_t version=0;
 for(const std::string text : {"大地", "一片", "昏暗"}) {
  engine.commit=text;const auto action=session.Handle(key,version);
  protocol::v1::CommitApplied applied;applied.set_request_id(action.commit_request().request_id());applied.set_context_version(++version);
  const auto confirmed=session.OnCommitApplied(applied);ASSERT_TRUE(confirmed);EXPECT_EQ(confirmed->text,text);
 }
 EXPECT_EQ(session.ContextText(), "大地一片昏暗");
 engine.commit="失败";const auto action=session.Handle(key,version);
 protocol::v1::CommitRejected rejected;rejected.set_request_id(action.commit_request().request_id());session.OnCommitRejected(rejected);
 EXPECT_EQ(session.ContextText(), "大地一片昏暗");
 session.Reset();EXPECT_TRUE(session.ContextText().empty());
}

}  // namespace
}  // namespace zhixu::input_host

#include "tsf/text_service.h"
TEST(RuntimeRecovery, RecreatedBackendDoesNotCollideWithTheLoadedApplicationsCommitHistory) {
  using namespace zhixu::input_host;
  class Engine final : public InputEngine { public: EngineState ProcessKey(const EngineKey&) override{return{};} CommitOrState SelectCandidate(std::size_t) override{return{};} } engine;
  zhixu::tsf::TextService application;
  zhixu::protocol::v1::PrivacyDecision privacy;privacy.set_allow_composition(true);
  std::uint64_t previous=0;
  for(unsigned i=0;i<4;++i) {
    SessionController backend(engine);
    auto action=backend.AcceptRecommendation("public",privacy,application.context_version());ASSERT_TRUE(action);
    const auto id=action->commit_request().request_id();const auto version=action->commit_request().expected_context_version();
    EXPECT_NE(id,previous);EXPECT_NE(id,0u);
    const auto outcome=application.ApplyCommit({id,version},true);
    EXPECT_EQ(outcome.kind,zhixu::tsf::CommitOutcomeKind::Applied);
    EXPECT_EQ(application.ApplyCommit({id,version},true).kind,zhixu::tsf::CommitOutcomeKind::Rejected);
    EXPECT_EQ(application.ApplyCommit({id,application.context_version()},true).kind,zhixu::tsf::CommitOutcomeKind::Rejected);
    previous=id;
  }
}
