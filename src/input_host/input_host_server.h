#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "common/ipc/named_pipe.h"
#include "common/settings/settings_store.h"
#include "common/protocol/protocol_handshake.h"
#include "input_host/session/session_controller.h"
#include "input_host/ui/candidate_presentation.h"
#include "input_host/ui/recommendation_presentation.h"

namespace zhixu::input_host {

class InputHostServer {
 public:
  using PredictionCommitDispatcher = std::function<void(ConfirmedCommit)>;
  using PredictionInvalidator = std::function<void()>;

  // The dispatcher must enqueue work and return promptly; pipe request handling cannot wait for prediction.
  explicit InputHostServer(InputEngine& engine, ui::CandidatePresenter* presenter = nullptr);
  InputHostServer(InputEngine& engine, ui::CandidatePresenter* presenter,
                  PredictionCommitDispatcher prediction_dispatcher,
                  PredictionInvalidator prediction_invalidator = {},
                  ui::RecommendationPresenter* recommendation_presenter = nullptr,
                  bool tab_accept_enabled = false, std::function<std::string()> policy_generation = {});

  void SetSettingsProvider(std::function<settings::Settings()> provider) { settings_provider_ = std::move(provider); }
  void OnPredictionResult(protocol::v1::PredictionResult result, std::uint64_t context_version);
  void OnRecommendationSelected(std::uint64_t batch_id, std::size_t index);
  void OnRecommendationPage(std::uint64_t batch_id, int direction);

  [[nodiscard]] tl::expected<void, ipc::PipeError> Serve(std::wstring_view role,
                                                          std::stop_token stop_token,
                                                          ipc::ConnectionLease lease = {});

 private:
  struct PendingCandidate {
    std::uint64_t batch, context_version;
    std::string session_id;
    unsigned int index;
    int page_direction = 0;
  };
  struct ActiveCandidatePresentation {
    protocol::v1::EngineAction action;
    protocol::v1::PrivacyDecision privacy;
    std::uint64_t context_version;
    std::string session_id;
  };
  std::optional<ActiveCandidatePresentation> active_candidate_presentation_;
  void PresentCandidates(const protocol::v1::EngineAction& action,
      const protocol::v1::PrivacyDecision& privacy, const protocol::Envelope& request);
  void QueueCandidate(std::uint64_t batch, unsigned int index, int page_direction);
  void ClearCandidatesLocked();
  void ConfigurationChanged();
  std::uint64_t candidate_batch_ = 0, candidate_version_ = 0;
  std::string candidate_session_;
  std::size_t visible_candidate_count_ = 0;
  bool candidate_has_next_ = false;
  std::uint32_t candidate_page_ = 0;
  std::optional<PendingCandidate> pending_candidate_;
  struct RecommendationState {
    std::vector<std::string> texts;
    protocol::v1::PredictionResult result;
    std::uint32_t page = 0;
    std::uint64_t context_version = 0;
    std::uint64_t batch_id = 0;
    std::string policy_generation;
    bool awaiting_caret = false;
    std::chrono::steady_clock::time_point caret_deadline{};
  };
  struct PendingRecommendation {
    std::string text;
    std::uint64_t context_version = 0;
    std::string policy_generation;
  };
  void ClearRecommendationsLocked();
  bool PresentRecommendationPageLocked();
  void ChangeRecommendationPageLocked(int direction);

  [[nodiscard]] protocol::Envelope Handle(const protocol::Envelope& request);
  [[nodiscard]] protocol::Envelope ErrorResponse(const protocol::Envelope& request) const;

  struct CaretSnapshot {
    protocol::v1::CaretUpdate update;
    std::uint64_t context_version;
    std::string session_id;
  };
  std::optional<CaretSnapshot> last_caret_;
  std::function<std::string()> policy_generation_;
  bool tab_accept_enabled_ = false;
  SessionController session_;
  ui::CandidatePresentationController presentation_;
  std::mutex recommendation_mutex_;
  ui::RecommendationPresentationController recommendation_presentation_;
  std::optional<RecommendationState> recommendations_;
  // Protected by recommendation_mutex_; only a confirmed current commit may wait.
  std::optional<std::uint64_t> pending_caret_version_;
  struct CommitCaretConvergence {
    std::uint64_t context_version;
    std::string session_id;
    std::uint64_t foreground_window;
    std::chrono::steady_clock::time_point deadline;
  };
  // Separate from pending result delivery: the first caret must not consume it.
  std::optional<CommitCaretConvergence> commit_caret_convergence_;
  std::optional<PendingRecommendation> pending_recommendation_;
  std::uint64_t next_recommendation_batch_ = 1;
  HWND notification_window_ = nullptr;
  PredictionCommitDispatcher prediction_dispatcher_;
  PredictionInvalidator prediction_invalidator_;
  protocol::ProtocolHandshake handshake_;
  std::function<settings::Settings()> settings_provider_;
  std::string pending_commit_policy_;
};

}  // namespace zhixu::input_host
