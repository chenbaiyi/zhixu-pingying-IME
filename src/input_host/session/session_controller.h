#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "common/protocol/envelope_codec.h"
#include "input_host/engine/input_engine.h"

namespace zhixu::input_host {

struct ConfirmedCommit {
  std::string text;
  std::uint64_t context_version = 0;
  bool allow_learning = false;
  bool allow_prediction = false;
  std::string context_text;
  std::string context_before;
  std::string session_id;
  std::uint64_t commit_id = 0;
  std::string policy_generation;
};

class SessionController {
 public:
  explicit SessionController(InputEngine& engine);

  [[nodiscard]] protocol::v1::EngineAction Handle(const protocol::v1::KeyEvent& event,
                                                   std::uint64_t context_version = 0);
  [[nodiscard]] protocol::v1::EngineAction SelectVisibleCandidate(std::size_t index,
      const protocol::v1::PrivacyDecision& privacy, std::uint64_t context_version);
  [[nodiscard]] std::optional<protocol::v1::EngineAction> AcceptRecommendation(
      std::string text, const protocol::v1::PrivacyDecision& privacy,
      std::uint64_t context_version);
  [[nodiscard]] std::optional<ConfirmedCommit> OnCommitApplied(
      const protocol::v1::CommitApplied& applied);
  void OnCommitRejected(const protocol::v1::CommitRejected& rejected);
  bool Configure(bool fuzzy, unsigned count, bool learning);
  void Reset(bool clear_context = true) noexcept;
  void ClearContext() noexcept { context_text_.clear(); }
  [[nodiscard]] const std::string& ContextText() const noexcept { return context_text_; }

  [[nodiscard]] std::uint64_t context_version() const noexcept;

 private:
  [[nodiscard]] protocol::v1::EngineAction MakeAction(const EngineState& state) const;
  void AttachCommitRequest(protocol::v1::EngineAction* action, std::string text,
                           const protocol::v1::PrivacyDecision& privacy);

  std::string context_text_;
  InputEngine& engine_;
  std::uint64_t context_version_ = 0;
  std::uint64_t next_request_id_ = 1;
  std::optional<std::uint64_t> pending_commit_id_;
  std::optional<ConfirmedCommit> pending_commit_;
  std::size_t candidate_count_ = 0;
  std::uint32_t highlighted_candidate_ = 0;
  std::uint32_t candidate_page_ = 0;
};

}  // namespace zhixu::input_host
