#include "input_host/session/session_controller.h"
#include "common/protocol/key_modifiers.h"

#include <cstddef>
#include <stdexcept>
#include <Windows.h>
#include <bcrypt.h>

namespace zhixu::input_host {
namespace {

constexpr std::uint32_t kFirstCandidateKey = static_cast<std::uint32_t>('1');
constexpr std::uint32_t kLastCandidateKey = static_cast<std::uint32_t>('9');
constexpr std::uint32_t kSpaceKey = 0x20;
constexpr std::uint32_t kTabKey = 0x09;

}  // namespace

SessionController::SessionController(InputEngine& engine) : engine_(engine) {
  // TSF may outlive this backend. Never restart its commit identity space at 1.
  // Keep ample headroom for the monotonic counter; fail before accepting input
  // if Windows cannot supply a fresh identity space.
  std::uint64_t seed=0;
  if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&seed),sizeof(seed),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
    throw std::runtime_error("commit identity initialization failed");
  next_request_id_=(seed & 0x3fffffffffffffffull) + 1;
}

protocol::v1::EngineAction SessionController::Handle(const protocol::v1::KeyEvent& event,
                                                      const std::uint64_t context_version) {
  if (!event.key_down() && event.key_code() != 0x10 && event.key_code() != 0xa0 &&
      event.key_code() != 0xa1) return {};
  // A second Rime commit cannot be stored while TSF still owns the first
  // asynchronous transaction. Fail open without advancing/draining Rime;
  // native editing will invalidate it, or the acknowledgment will unblock it.
  if (pending_commit_id_) return {};
  if (!pending_commit_id_.has_value() && context_version >= context_version_) {
    context_version_ = context_version;
  }
  const auto key_code = event.key_code() >= 0x60 && event.key_code() <= 0x69 &&
      (event.key_symbol() == 0 || (event.key_symbol() >= '0' && event.key_symbol() <= '9'))
      ? static_cast<std::uint32_t>('0') + event.key_code() - 0x60 : event.key_code();
  const bool unmodified = (event.modifiers() & (protocol::kHeldModifiers | protocol::kCapsLock)) == 0;
  const bool selects_first_candidate = unmodified && key_code == kSpaceKey && candidate_count_ != 0;
  if (selects_first_candidate ||
      (unmodified && key_code >= kFirstCandidateKey && key_code <= kLastCandidateKey)) {
    std::size_t index = selects_first_candidate ? highlighted_candidate_ :
        static_cast<std::size_t>(key_code - kFirstCandidateKey);
    if (!selects_first_candidate && candidate_page_ > 0)
      index = key_code <= '6' ? (highlighted_candidate_ / 6) * 6 + index : candidate_count_;
    return SelectVisibleCandidate(index, event.privacy(), context_version);
  }
  if (key_code == kTabKey) {
    return {};
  }

  const auto outcome = engine_.ProcessKeyAction({
      .virtual_key = key_code,
      .key_down = event.key_down(),
      .modifiers = event.modifiers(),
      .key_symbol = event.key_symbol(),
  });
  const auto& state = outcome.state;
  candidate_count_ = state.candidate_text.size();
  candidate_page_ = state.page;
  highlighted_candidate_ = state.highlighted_candidate < candidate_count_ ? state.highlighted_candidate : 0;
  auto action = MakeAction(state);
  if (outcome.commit_text && !pending_commit_id_) AttachCommitRequest(&action, *outcome.commit_text, event.privacy());
  return action;
}

protocol::v1::EngineAction SessionController::SelectVisibleCandidate(std::size_t index,
    const protocol::v1::PrivacyDecision& privacy, std::uint64_t context_version) {
  if (pending_commit_id_) return {};
  if (context_version >= context_version_) context_version_ = context_version;
  const auto selection = engine_.SelectCandidate(index);
  candidate_count_ = selection.state.candidate_text.size();
  candidate_page_ = selection.state.page;
  highlighted_candidate_ = selection.state.highlighted_candidate < candidate_count_ ? selection.state.highlighted_candidate : 0;
  auto action = MakeAction(selection.state);
  if (selection.commit_text) AttachCommitRequest(&action, *selection.commit_text, privacy);
  return action;
}

std::optional<protocol::v1::EngineAction> SessionController::AcceptRecommendation(
    std::string text, const protocol::v1::PrivacyDecision& privacy,
    const std::uint64_t context_version) {
  if (text.empty() || pending_commit_id_.has_value()) return std::nullopt;
  if (context_version >= context_version_) context_version_ = context_version;
  protocol::v1::EngineAction action;
  action.set_consumed(true);
  AttachCommitRequest(&action, std::move(text), privacy);
  return action;
}

std::optional<ConfirmedCommit> SessionController::OnCommitApplied(
    const protocol::v1::CommitApplied& applied) {
  if (!pending_commit_id_.has_value() || applied.request_id() != *pending_commit_id_ ||
      applied.context_version() != context_version_ + 1 || !pending_commit_.has_value()) return std::nullopt;

  pending_commit_->context_before = context_text_;
  if (pending_commit_->allow_prediction || pending_commit_->allow_learning) {
    context_text_ += pending_commit_->text;
    std::size_t count = 0, cut = context_text_.size();
    for (std::size_t i=context_text_.size(); i>0; --i) {
      if ((static_cast<unsigned char>(context_text_[i-1]) & 0xc0) != 0x80) {
        if (++count > 256) break;
        cut=i-1;
      }
    }
    if (cut) context_text_.erase(0,cut);
  } else { context_text_.clear(); }
  pending_commit_->context_text = context_text_;
  pending_commit_->commit_id = applied.request_id();
  context_version_ = applied.context_version();
  pending_commit_id_.reset();
  pending_commit_->context_version = context_version_;
  auto confirmed = std::move(pending_commit_);
  pending_commit_.reset();
  return confirmed;
}

void SessionController::OnCommitRejected(const protocol::v1::CommitRejected& rejected) {
  if (pending_commit_id_.has_value() && rejected.request_id() == *pending_commit_id_) {
    pending_commit_id_.reset();
    pending_commit_.reset();
  }
}

bool SessionController::Configure(bool fuzzy, unsigned count, bool learning) {
  if (pending_commit_id_ || !engine_.Configure(fuzzy,count,learning)) return false;
  context_text_.clear();
  const auto state=engine_.ProcessKeyAction({}).state;
  candidate_count_=state.candidate_text.size(); candidate_page_=state.page;
  highlighted_candidate_=state.highlighted_candidate < candidate_count_ ? state.highlighted_candidate : 0;
  return true;
}

void SessionController::Reset(const bool clear_context) noexcept {
  if (clear_context) context_text_.clear();
  engine_.ResetComposition();
  context_version_ = 0;
  // TSF deduplicates applied IDs across caret/focus invalidations.
  pending_commit_id_.reset();
  pending_commit_.reset();
  candidate_count_ = 0;
  candidate_page_ = 0;
  highlighted_candidate_ = 0;
}

std::uint64_t SessionController::context_version() const noexcept {
  return context_version_;
}

protocol::v1::EngineAction SessionController::MakeAction(const EngineState& state) const {
  protocol::v1::EngineAction action;
  action.set_consumed(state.consumed);

  auto* composition = action.mutable_composition();
  composition->set_preedit(state.preedit);
  composition->set_highlighted_candidate(state.highlighted_candidate);
  if (state.cursor) composition->set_cursor(*state.cursor);
  composition->set_page(state.page);
  composition->set_has_next_page(state.has_next_page);
  composition->set_ascii_mode(state.ascii_mode);
  composition->set_full_shape(state.full_shape);
  composition->set_ascii_punctuation(state.ascii_punctuation);
  composition->set_traditional(state.traditional);
  for (std::size_t index = 0; index < state.candidate_text.size(); ++index) {
    auto* candidate = composition->add_candidates();
    candidate->set_index(static_cast<std::uint32_t>(index));
    candidate->set_text(state.candidate_text[index]);
  }

  return action;
}

void SessionController::AttachCommitRequest(protocol::v1::EngineAction* const action,
                                            std::string text,
                                            const protocol::v1::PrivacyDecision& privacy) {
  auto* request = action->mutable_commit_request();
  request->set_request_id(next_request_id_++);
  request->set_expected_context_version(context_version_);
  request->set_text(text);
  pending_commit_id_ = request->request_id();
  pending_commit_ = {.text = std::move(text),
                     .allow_learning = privacy.allow_learning(),
                     .allow_prediction = privacy.allow_prediction()};
}

}  // namespace zhixu::input_host
