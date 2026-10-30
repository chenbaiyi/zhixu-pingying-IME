#include "input_host/input_host_server.h"
#include "common/protocol/recommendation_notification.h"
#include "common/protocol/key_modifiers.h"

#include <string>
#include <algorithm>

namespace zhixu::input_host {
namespace {

constexpr std::uint32_t kAltModifier = 1U;
constexpr std::uint32_t kFirstRecommendationKey = static_cast<std::uint32_t>('1');
constexpr std::uint32_t kLastRecommendationKey = static_cast<std::uint32_t>('6');
constexpr std::uint32_t kTabKey = 0x09;

std::optional<std::size_t> RecommendationIndex(const protocol::v1::KeyEvent& event, const bool tab_enabled) {
  if (!event.key_down()) return std::nullopt;
  if (event.key_code() == kTabKey && tab_enabled && event.modifiers() == 0) return 0U;
  if ((event.modifiers() & protocol::kHeldModifiers) != kAltModifier || event.key_code() < kFirstRecommendationKey ||
      event.key_code() > kLastRecommendationKey) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(event.key_code() - kFirstRecommendationKey);
}

template <typename Message>
bool ParsePayload(const protocol::Envelope& envelope, Message* message) {
  return message->ParseFromString(envelope.payload());
}

template <typename Message>
protocol::Envelope MakeResponse(const protocol::Envelope& request,
                                const protocol::v1::MessageType type,
                                const Message& message) {
  protocol::Envelope response;
  response.set_protocol_version(protocol::kProtocolVersion);
  response.set_message_type(type);
  response.set_session_id(request.session_id());
  response.set_request_id(request.request_id());
  message.SerializeToString(response.mutable_payload());
  return response;
}

}  // namespace

InputHostServer::InputHostServer(InputEngine& engine, ui::CandidatePresenter* const presenter)
    : InputHostServer(engine, presenter, {}) {}

InputHostServer::InputHostServer(InputEngine& engine, ui::CandidatePresenter* const presenter,
                                 PredictionCommitDispatcher prediction_dispatcher,
                                 PredictionInvalidator prediction_invalidator,
                                 ui::RecommendationPresenter* const recommendation_presenter, const bool tab_accept_enabled, std::function<std::string()> policy_generation)
    : policy_generation_(std::move(policy_generation)), tab_accept_enabled_(tab_accept_enabled), session_(engine), presentation_(presenter), prediction_dispatcher_(std::move(prediction_dispatcher)),
      prediction_invalidator_(std::move(prediction_invalidator)),
      recommendation_presentation_(recommendation_presenter) {}

void InputHostServer::ConfigurationChanged() {
  if (prediction_invalidator_) prediction_invalidator_();
  presentation_.Invalidate();
  std::lock_guard lock(recommendation_mutex_);
  recommendation_presentation_.Invalidate(); ClearRecommendationsLocked(); ClearCandidatesLocked();
}

void InputHostServer::ClearCandidatesLocked() {
  ++candidate_batch_; visible_candidate_count_ = 0; pending_candidate_.reset();
  active_candidate_presentation_.reset();
}
void InputHostServer::QueueCandidate(std::uint64_t batch, unsigned int index, int direction) {
  std::lock_guard lock(recommendation_mutex_);
  if (batch != candidate_batch_ || pending_candidate_ || visible_candidate_count_ == 0) return;
  if ((direction == 0 && index >= visible_candidate_count_) ||
      (direction < 0 && candidate_page_ == 0) || (direction > 0 && !candidate_has_next_)) return;
  pending_candidate_ = {batch, candidate_version_, candidate_session_, index, direction};
  visible_candidate_count_ = 0;
  if (notification_window_) PostMessageW(notification_window_, protocol::kRecommendationSelectedMessage, 0, 0);
}
void InputHostServer::PresentCandidates(const protocol::v1::EngineAction& action,
    const protocol::v1::PrivacyDecision& privacy, const protocol::Envelope& request) {
  std::uint64_t batch;
  {
    std::lock_guard lock(recommendation_mutex_);
    ClearCandidatesLocked(); batch = candidate_batch_;
    candidate_version_ = request.context_version(); candidate_session_ = request.session_id();
    visible_candidate_count_ = action.consumed() && (privacy.allow_composition() || privacy.allow_prediction()) ?
        action.composition().candidates_size() : 0;
    candidate_page_ = action.composition().page(); candidate_has_next_ = action.composition().has_next_page();
    if (visible_candidate_count_ != 0 && !action.has_commit_request() &&
        !action.composition().preedit().empty()) {
      active_candidate_presentation_ = {.action = action, .privacy = privacy,
          .context_version = request.context_version(), .session_id = request.session_id()};
    }
  }
  presentation_.OnEngineAction(action, privacy, request.context_version(),
      [this, batch](unsigned int index) { QueueCandidate(batch, index, 0); },
      [this, batch](bool forward) { QueueCandidate(batch, 0, forward ? 1 : -1); });
}

void InputHostServer::OnPredictionResult(protocol::v1::PredictionResult result,
                                         const std::uint64_t context_version) {
  std::lock_guard lock(recommendation_mutex_);
  if (policy_generation_ && (result.policy_generation().empty() || result.policy_generation() != policy_generation_())) {
    recommendation_presentation_.Invalidate(); ClearRecommendationsLocked(); return;
  }
  while (result.candidates_size() > 30) result.mutable_candidates()->RemoveLast();
  RecommendationState state;
  state.context_version = context_version;
  state.policy_generation = result.policy_generation();
  for (const auto& candidate : result.candidates()) {
    if (candidate.text().empty()) { recommendation_presentation_.Invalidate(); ClearRecommendationsLocked(); return; }
    state.texts.push_back(candidate.text());
  }
  state.result = std::move(result);
  pending_recommendation_.reset();
  recommendations_ = std::move(state);
  if (!PresentRecommendationPageLocked()) {
    if (pending_caret_version_ == context_version && recommendations_->result.final_result() &&
        !recommendations_->texts.empty()) {
      recommendations_->awaiting_caret = true;
      recommendations_->caret_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    } else {
      ClearRecommendationsLocked();
    }
  } else {
    pending_caret_version_.reset();
    commit_caret_convergence_.reset();
  }
}

bool InputHostServer::PresentRecommendationPageLocked() {
  if (!recommendations_) return false;
  const auto batch = next_recommendation_batch_++;
  recommendations_->batch_id = batch;
  const auto offset = recommendations_->page * 6;
  return recommendation_presentation_.OnPredictionResult(recommendations_->result, recommendations_->context_version,
      [this, batch, offset](unsigned int index) { OnRecommendationSelected(batch, offset + index); },
      recommendations_->page, [this, batch](int direction) { OnRecommendationPage(batch, direction); });
}

void InputHostServer::ChangeRecommendationPageLocked(const int direction) {
  if (!recommendations_ || recommendations_->awaiting_caret || (direction != -1 && direction != 1)) return;
  const auto pages = (recommendations_->texts.size() + 5) / 6;
  if ((direction < 0 && recommendations_->page == 0) ||
      (direction > 0 && recommendations_->page + 1 >= pages)) return;
  if (direction > 0) ++recommendations_->page; else --recommendations_->page;
  if (!PresentRecommendationPageLocked()) ClearRecommendationsLocked();
}

void InputHostServer::OnRecommendationPage(const std::uint64_t batch, const int direction) {
  std::lock_guard lock(recommendation_mutex_);
  if (!recommendations_ || recommendations_->awaiting_caret || recommendations_->batch_id != batch) return;
  if (policy_generation_ && recommendations_->policy_generation != policy_generation_()) {
    recommendation_presentation_.Invalidate(); ClearRecommendationsLocked(); return;
  }
  ChangeRecommendationPageLocked(direction);
}
void InputHostServer::OnRecommendationSelected(const std::uint64_t batch_id, const std::size_t index) {
  std::lock_guard lock(recommendation_mutex_);
  if(recommendations_ && policy_generation_ && recommendations_->policy_generation!=policy_generation_()){ClearRecommendationsLocked();return;}
  if (!recommendations_ || recommendations_->awaiting_caret || recommendations_->batch_id != batch_id ||
      index >= recommendations_->texts.size()) return;
  pending_recommendation_ = {.text = recommendations_->texts[index],
                              .context_version = recommendations_->context_version, .policy_generation=recommendations_->policy_generation};
  recommendations_.reset();
  recommendation_presentation_.Invalidate();
  if (notification_window_ != nullptr) {
    PostMessageW(notification_window_, protocol::kRecommendationSelectedMessage, 0, 0);
  }
}

void InputHostServer::ClearRecommendationsLocked() {
  recommendations_.reset();
  pending_recommendation_.reset();
  pending_caret_version_.reset();
  commit_caret_convergence_.reset();
}

tl::expected<void, ipc::PipeError> InputHostServer::Serve(const std::wstring_view role,
                                                           const std::stop_token stop_token,
                                                           ipc::ConnectionLease lease) {
  const auto endpoint = ipc::PipeEndpoint::ForCurrentUser(role);
  if (!endpoint) {
    return tl::unexpected(endpoint.error());
  }

  while (!stop_token.stop_requested()) {
    ipc::NamedPipeServer server;
    const auto served = server.Listen(*endpoint,
                                      [this](const protocol::Envelope& request) {
                                        return Handle(request);
                                      },
                                      stop_token, lease);
    if (!served) {
      return tl::unexpected(served.error());
    }
    last_caret_.reset();
    presentation_.Invalidate();
    {
      std::lock_guard lock(recommendation_mutex_);
      recommendation_presentation_.Invalidate();
      ClearRecommendationsLocked();
      ClearCandidatesLocked();
      notification_window_ = nullptr;
    }
    if (prediction_invalidator_) prediction_invalidator_();
    handshake_.Reset();
    session_.Reset();
  }
  return {};
}

protocol::Envelope InputHostServer::Handle(const protocol::Envelope& request) {
  if (request.protocol_version() != protocol::kProtocolVersion) {
    return ErrorResponse(request);
  }

  if (request.message_type() == protocol::v1::HELLO) {
    protocol::Hello hello;
    if (!ParsePayload(request, &hello)) {
      return ErrorResponse(request);
    }
    handshake_.Reset();
    session_.Reset();
    last_caret_.reset();
    presentation_.Invalidate();
    {
      std::lock_guard lock(recommendation_mutex_);
      recommendation_presentation_.Invalidate();
      ClearRecommendationsLocked();
      ClearCandidatesLocked();
      notification_window_ = nullptr;
    }
    if (prediction_invalidator_) prediction_invalidator_();
    const auto acknowledgment = handshake_.Accept(hello);
    if (!acknowledgment) {
      return ErrorResponse(request);
    }
    return MakeResponse(request, protocol::v1::HELLO_ACK, *acknowledgment);
  }

  if (!handshake_.Validate(request.message_type())) {
    return ErrorResponse(request);
  }

  switch (request.message_type()) {
    case protocol::v1::KEY_EVENT: {
      protocol::v1::KeyEvent event;
      if (!ParsePayload(request, &event)) {
        return ErrorResponse(request);
      }
      const auto commit_policy = policy_generation_ ? policy_generation_() : std::string{};
      if (settings_provider_) {
        const auto current = settings_provider_();
        tab_accept_enabled_ = current.tab_accept_enabled;
        const bool excluded = !current.excluded_app_hashes.empty() && (event.application_hash().empty() || std::ranges::find(current.excluded_app_hashes,event.application_hash()) != current.excluded_app_hashes.end());
        if (excluded) { event.mutable_privacy()->set_allow_learning(false); event.mutable_privacy()->set_allow_prediction(false); }
        event.mutable_privacy()->set_allow_learning(event.privacy().allow_learning() && !current.privacy_mode && current.learning_enabled);
        event.mutable_privacy()->set_allow_prediction(event.privacy().allow_prediction() && !current.privacy_mode && current.prediction_enabled);
        if (session_.Configure(current.fuzzy_pinyin,current.candidate_count,event.privacy().allow_learning())) ConfigurationChanged();
      }
      // Page navigation changes only this prediction batch, never the text/caret version.
      const auto held = event.modifiers() & protocol::kHeldModifiers;
      const bool plain_page = held == 0 && (event.key_code() == VK_PRIOR || event.key_code() == VK_NEXT);
      const bool alt_page = held == protocol::kAlt && (event.key_code() == VK_OEM_MINUS || event.key_code() == VK_OEM_PLUS);
      if (event.key_down() && (plain_page || alt_page) &&
          event.privacy().allow_composition() && event.privacy().allow_prediction()) {
        std::lock_guard lock(recommendation_mutex_);
        if (recommendations_ && !recommendations_->awaiting_caret && recommendations_->context_version == request.context_version() &&
            (!policy_generation_ || recommendations_->policy_generation == policy_generation_())) {
          ChangeRecommendationPageLocked((event.key_code() == VK_NEXT || event.key_code() == VK_OEM_PLUS) ? 1 : -1);
          protocol::v1::EngineAction page_action;
          page_action.set_consumed(true);
          return MakeResponse(request, protocol::v1::ENGINE_ACTION, page_action);
        }
      }
      std::optional<std::string> accepted_recommendation;
      {
        std::lock_guard lock(recommendation_mutex_);
        recommendation_presentation_.Invalidate();
        const auto index = RecommendationIndex(event, tab_accept_enabled_);
        if (index.has_value() && recommendations_.has_value() && !recommendations_->awaiting_caret &&
            recommendations_->context_version == request.context_version() &&
            *index + recommendations_->page * 6 < recommendations_->texts.size() && (!policy_generation_ || recommendations_->policy_generation==policy_generation_())) {
          accepted_recommendation = recommendations_->texts[*index + recommendations_->page * 6];
        }
        ClearRecommendationsLocked();
        ClearCandidatesLocked();
      }
      if (prediction_invalidator_) prediction_invalidator_();
      std::optional<protocol::v1::EngineAction> accepted;
      if (accepted_recommendation.has_value()) {
        accepted = session_.AcceptRecommendation(std::move(*accepted_recommendation), event.privacy(),
                                                  request.context_version());
      }
      const auto action = accepted.has_value() ? *accepted : session_.Handle(event, request.context_version());
      if (action.has_commit_request()) pending_commit_policy_ = commit_policy;
      PresentCandidates(action, event.privacy(), request);
      return MakeResponse(request, protocol::v1::ENGINE_ACTION, action);
    }
    case protocol::v1::CARET_UPDATE: {
      protocol::v1::CaretUpdate caret;
      if (!ParsePayload(request, &caret)) return ErrorResponse(request);
      const bool invalid = caret.caret().right() < caret.caret().left() ||
          caret.caret().bottom() <= caret.caret().top() ||
          caret.work_area().right() <= caret.work_area().left() ||
          caret.work_area().bottom() <= caret.work_area().top();
      const bool first_caret = !last_caret_;
      const bool changed = invalid || (last_caret_ &&
          (last_caret_->context_version != request.context_version() ||
           last_caret_->session_id != request.session_id() ||
           last_caret_->update.SerializeAsString() != caret.SerializeAsString()));
      std::optional<ActiveCandidatePresentation> active;
      // Keep classification and a permitted anchor update in the same critical
      // section: publication must not turn a pending batch visible between them.
      std::unique_lock lock(recommendation_mutex_);
      const bool converging_commit_caret = !invalid && commit_caret_convergence_ &&
          commit_caret_convergence_->context_version == request.context_version() &&
          commit_caret_convergence_->session_id == request.session_id() &&
          std::chrono::steady_clock::now() < commit_caret_convergence_->deadline &&
          (commit_caret_convergence_->foreground_window == 0 ||
           commit_caret_convergence_->foreground_window == caret.foreground_window()) &&
          (!last_caret_ || (last_caret_->session_id == request.session_id() &&
                           last_caret_->update.foreground_window() == caret.foreground_window() &&
                           last_caret_->update.notification_window() == caret.notification_window()));
      const bool rejected_first_commit_anchor = first_caret && commit_caret_convergence_ &&
                                                !converging_commit_caret;
      const bool cancel_prediction = (changed || rejected_first_commit_anchor) &&
                                     !converging_commit_caret;
      // Own preedit writes also move the insertion caret. Re-anchor only this
      // still-current composition, issuing fresh callbacks for its new location.
      if ((changed || first_caret) && !invalid && active_candidate_presentation_ &&
          active_candidate_presentation_->context_version == request.context_version() &&
          active_candidate_presentation_->session_id == request.session_id() &&
          session_.context_version() == request.context_version() &&
          (!last_caret_ || last_caret_->update.foreground_window() == caret.foreground_window())) {
        active = active_candidate_presentation_;
      }
      if (cancel_prediction) {
        // Runtime publication holds its token lock before this mutex. Cancel
        // outside this mutex, then clear any publication that finished meanwhile.
        lock.unlock();
        if (prediction_invalidator_) prediction_invalidator_();
        if (!active) session_.ClearContext();
        lock.lock();
      }
      if (converging_commit_caret && commit_caret_convergence_->foreground_window == 0)
        commit_caret_convergence_->foreground_window = caret.foreground_window();
      last_caret_ = {.update = caret, .context_version = request.context_version(),
                      .session_id = request.session_id()};
      presentation_.OnCaretUpdate(caret, request.context_version());
      recommendation_presentation_.OnCaretUpdate(caret, request.context_version());
      if (first_caret || changed) {
        const bool show_waiting = converging_commit_caret && recommendations_ &&
            recommendations_->awaiting_caret &&
            pending_caret_version_ == request.context_version() &&
            recommendations_->context_version == request.context_version() &&
            std::chrono::steady_clock::now() < recommendations_->caret_deadline &&
            (!policy_generation_ || recommendations_->policy_generation == policy_generation_());
        if (show_waiting) {
          recommendations_->awaiting_caret = false;
          pending_caret_version_.reset();
          if (!PresentRecommendationPageLocked()) ClearRecommendationsLocked();
          else commit_caret_convergence_.reset();
        } else if (!converging_commit_caret) {
          ClearRecommendationsLocked();
        }
      }
      if (changed || candidate_version_ != request.context_version()) ClearCandidatesLocked();
      notification_window_ = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(caret.notification_window()));
      lock.unlock();
      if (active) PresentCandidates(active->action, active->privacy, request);
      return MakeResponse(request, protocol::v1::CARET_UPDATE, caret);
    }
    case protocol::v1::SESSION_INVALIDATED: {
      protocol::v1::SessionInvalidated invalidation;
      if (!ParsePayload(request, &invalidation)) return ErrorResponse(request);
      last_caret_.reset();
      presentation_.Invalidate();
      {
        std::lock_guard lock(recommendation_mutex_);
        recommendation_presentation_.Invalidate();
        ClearRecommendationsLocked();
        ClearCandidatesLocked();
      }
      session_.Reset(invalidation.reason() != protocol::v1::CONTINUED_INPUT && invalidation.reason() != protocol::v1::ESCAPE);
      if (prediction_invalidator_) prediction_invalidator_();
      return MakeResponse(request, protocol::v1::SESSION_INVALIDATED, invalidation);
    }
    case protocol::v1::POLL_RECOMMENDATION: {
      protocol::v1::PollRecommendation poll;
      if (!ParsePayload(request, &poll)) return ErrorResponse(request);
      const auto commit_policy = policy_generation_ ? policy_generation_() : std::string{};
      if (settings_provider_) {
        const auto s=settings_provider_();
        const bool excluded=!s.excluded_app_hashes.empty() && (poll.application_hash().empty() || std::ranges::find(s.excluded_app_hashes,poll.application_hash()) != s.excluded_app_hashes.end());
        poll.mutable_privacy()->set_allow_learning(poll.privacy().allow_learning() && !s.privacy_mode && s.learning_enabled && !excluded);
        poll.mutable_privacy()->set_allow_prediction(poll.privacy().allow_prediction() && !s.privacy_mode && s.prediction_enabled && !excluded);
        if (session_.Configure(s.fuzzy_pinyin,s.candidate_count,poll.privacy().allow_learning())) ConfigurationChanged();
      }
      std::optional<PendingRecommendation> pending;
      std::optional<PendingCandidate> candidate;
      {
        std::lock_guard lock(recommendation_mutex_);
        candidate = std::move(pending_candidate_); pending_candidate_.reset();
        pending = std::move(pending_recommendation_);
        pending_recommendation_.reset();
      }
      protocol::v1::EngineAction action;
      if (candidate && candidate->context_version == request.context_version() &&
          candidate->session_id == request.session_id() && candidate->batch == candidate_batch_ &&
          candidate->context_version == session_.context_version() &&
          poll.privacy().allow_composition()) {
        if (prediction_invalidator_) prediction_invalidator_();
        if (candidate->page_direction) {
          protocol::v1::KeyEvent key;
          key.set_key_code(candidate->page_direction > 0 ? VK_NEXT : VK_PRIOR);
          key.set_key_down(true); *key.mutable_privacy() = poll.privacy();
          action = session_.Handle(key, request.context_version());
        } else action = session_.SelectVisibleCandidate(candidate->index, poll.privacy(), request.context_version());
        PresentCandidates(action, poll.privacy(), request);
      } else if (pending && (!policy_generation_ || pending->policy_generation==policy_generation_()) && pending->context_version == request.context_version() &&
          poll.privacy().allow_composition() && poll.privacy().allow_prediction()) {
        if (prediction_invalidator_) prediction_invalidator_();
        const auto accepted = session_.AcceptRecommendation(std::move(pending->text), poll.privacy(),
                                                            request.context_version());
        if (accepted) action = *accepted;
      }
      if (action.has_commit_request()) pending_commit_policy_ = commit_policy;
      return MakeResponse(request, protocol::v1::ENGINE_ACTION, action);
    }
    case protocol::v1::COMMIT_APPLIED: {
      protocol::v1::CommitApplied applied;
      if (!ParsePayload(request, &applied)) {
        return ErrorResponse(request);
      }
      auto confirmed = session_.OnCommitApplied(applied);
      if (confirmed) {
        confirmed->policy_generation = pending_commit_policy_;
        if (policy_generation_ && (pending_commit_policy_.empty() || pending_commit_policy_ != policy_generation_())) {
          confirmed->allow_learning=false; confirmed->allow_prediction=false; session_.ClearContext();
        }
      }
      if (confirmed.has_value()) {
        // TSF reports the caret moved by this successful edit after acknowledging
        // the commit. Its first valid update anchors the new prediction; only
        // subsequent caret changes are external invalidations.
        const auto foreground = last_caret_ ? last_caret_->update.foreground_window() : 0;
        last_caret_.reset();
        std::lock_guard lock(recommendation_mutex_);
        recommendation_presentation_.Invalidate();
        ClearRecommendationsLocked();
        pending_caret_version_ = confirmed->context_version;
        commit_caret_convergence_ = {.context_version = confirmed->context_version,
            .session_id = request.session_id(), .foreground_window = foreground,
            .deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500)};
      }
      if (confirmed.has_value()) confirmed->session_id = request.session_id();
      if (confirmed.has_value() && (confirmed->allow_prediction || confirmed->allow_learning) && prediction_dispatcher_) {
        prediction_dispatcher_(std::move(*confirmed));
      }
      return MakeResponse(request, protocol::v1::COMMIT_APPLIED, applied);
    }
    case protocol::v1::COMMIT_REJECTED: {
      protocol::v1::CommitRejected rejected;
      if (!ParsePayload(request, &rejected)) {
        return ErrorResponse(request);
      }
      session_.OnCommitRejected(rejected);
      return MakeResponse(request, protocol::v1::COMMIT_REJECTED, rejected);
    }
    default:
      return ErrorResponse(request);
  }
}

protocol::Envelope InputHostServer::ErrorResponse(const protocol::Envelope& request) const {
  protocol::Envelope response;
  response.set_protocol_version(protocol::kProtocolVersion);
  response.set_message_type(protocol::v1::MESSAGE_TYPE_UNSPECIFIED);
  response.set_session_id(request.session_id());
  response.set_request_id(request.request_id());
  return response;
}

}  // namespace zhixu::input_host
