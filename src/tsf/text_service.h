#pragma once

#include <cstdint>
#include <unordered_set>

namespace zhixu::tsf {

enum class InvalidationReason {
  FocusChanged,
  CaretMoved,
  ContinuedInput,
  ExternalEdit,
  Escape,
  Disconnected,
};

struct CommitIdentity {
  std::uint64_t request_id = 0;
  std::uint64_t expected_context_version = 0;
};

enum class CommitOutcomeKind {
  Applied,
  Rejected,
};

struct CommitOutcome {
  CommitOutcomeKind kind = CommitOutcomeKind::Rejected;
  std::uint64_t request_id = 0;
  std::uint64_t context_version = 0;
};

class TextService {
 public:
  [[nodiscard]] CommitOutcome ApplyCommit(CommitIdentity identity,
                                          bool edit_session_succeeded);
  void Invalidate(InvalidationReason reason) noexcept;
  [[nodiscard]] std::uint64_t context_version() const noexcept;

 private:
  std::uint64_t context_version_ = 0;
  std::unordered_set<std::uint64_t> applied_request_ids_;
};

}  // namespace zhixu::tsf
