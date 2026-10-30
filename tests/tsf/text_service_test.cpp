#include <gtest/gtest.h>

#include <optional>

#include "tsf/edit_session.h"
#include "tsf/text_service.h"

namespace zhixu::tsf {
namespace {

TEST(TextService, FailedEditSessionRejectsCommitWithoutAdvancingContext) {
  TextService service;

  const auto outcome = service.ApplyCommit({.request_id = 7, .expected_context_version = 0}, false);

  EXPECT_EQ(outcome.kind, CommitOutcomeKind::Rejected);
  EXPECT_EQ(outcome.request_id, 7);
  EXPECT_EQ(outcome.context_version, 0);
  EXPECT_EQ(service.context_version(), 0);
}

TEST(TextService, SuccessfulMatchingEditSessionAdvancesContextOnce) {
  TextService service;

  const auto outcome = service.ApplyCommit({.request_id = 7, .expected_context_version = 0}, true);

  EXPECT_EQ(outcome.kind, CommitOutcomeKind::Applied);
  EXPECT_EQ(outcome.request_id, 7);
  EXPECT_EQ(outcome.context_version, 1);
  EXPECT_EQ(service.context_version(), 1);
}

TEST(TextService, DuplicateCommitAcknowledgementDoesNotAdvanceContextTwice) {
  TextService service;
  ASSERT_EQ(service.ApplyCommit({.request_id = 7, .expected_context_version = 0}, true).kind,
            CommitOutcomeKind::Applied);

  const auto duplicate = service.ApplyCommit({.request_id = 7, .expected_context_version = 0}, true);

  EXPECT_EQ(duplicate.kind, CommitOutcomeKind::Rejected);
  EXPECT_EQ(duplicate.context_version, 1);
  EXPECT_EQ(service.context_version(), 1);
}

TEST(TextService, ReusedCommitRequestIdCannotAdvanceCurrentContext) {
  TextService service;
  ASSERT_EQ(service.ApplyCommit({.request_id = 7, .expected_context_version = 0}, true).kind,
            CommitOutcomeKind::Applied);

  const auto duplicate = service.ApplyCommit({.request_id = 7, .expected_context_version = 1}, true);

  EXPECT_EQ(duplicate.kind, CommitOutcomeKind::Rejected);
  EXPECT_EQ(service.context_version(), 1);
}

TEST(TextService, ExternalEditInvalidatesOldPredictionCommit) {
  TextService service;
  service.Invalidate(InvalidationReason::ExternalEdit);

  const auto stale = service.ApplyCommit({.request_id = 11, .expected_context_version = 0}, true);

  EXPECT_EQ(service.context_version(), 1);
  EXPECT_EQ(stale.kind, CommitOutcomeKind::Rejected);
  EXPECT_EQ(stale.context_version, 1);
}

class CapturingCommitEventSink final : public CommitEventSink {
 public:
  void OnCommitApplied(const CommitOutcome& outcome) override { applied = outcome; }
  void OnCommitRejected(const CommitOutcome& outcome, const HRESULT error) override {
    rejected = outcome;
    rejection_error = error;
  }

  std::optional<CommitOutcome> applied;
  std::optional<CommitOutcome> rejected;
  HRESULT rejection_error = S_OK;
};

TEST(CommitEditSession, InvalidRequestNotifiesRejectedWithoutAdvancingContext) {
  TextService service;
  CapturingCommitEventSink sink;

  const HRESULT result = RequestTextCommit(nullptr, TF_CLIENTID_NULL, service,
                                           {.request_id = 13, .expected_context_version = 0},
                                           L"candidate", sink);

  EXPECT_EQ(result, E_INVALIDARG);
  ASSERT_TRUE(sink.rejected.has_value());
  EXPECT_EQ(sink.rejected->kind, CommitOutcomeKind::Rejected);
  EXPECT_EQ(sink.rejected->request_id, 13);
  EXPECT_EQ(sink.rejected->context_version, 0);
  EXPECT_EQ(sink.rejection_error, E_INVALIDARG);
  EXPECT_EQ(service.context_version(), 0);
}

}  // namespace
}  // namespace zhixu::tsf
