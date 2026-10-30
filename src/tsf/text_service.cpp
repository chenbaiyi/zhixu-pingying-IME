#include "tsf/text_service.h"

namespace zhixu::tsf {

CommitOutcome TextService::ApplyCommit(const CommitIdentity identity,
                                       const bool edit_session_succeeded) {
  if (!edit_session_succeeded || identity.expected_context_version != context_version_ ||
      applied_request_ids_.contains(identity.request_id)) {
    return {.kind = CommitOutcomeKind::Rejected,
            .request_id = identity.request_id,
            .context_version = context_version_};
  }

  applied_request_ids_.insert(identity.request_id);
  ++context_version_;
  return {.kind = CommitOutcomeKind::Applied,
          .request_id = identity.request_id,
          .context_version = context_version_};
}

void TextService::Invalidate(const InvalidationReason) noexcept {
  ++context_version_;
}

std::uint64_t TextService::context_version() const noexcept {
  return context_version_;
}

}  // namespace zhixu::tsf
