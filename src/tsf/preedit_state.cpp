#include "tsf/preedit_state.h"

namespace zhixu::tsf {

PreeditOperation PreeditState::Next(const std::string_view preedit,
                                    const bool has_commit_request) const noexcept {
  if (has_commit_request) {
    return PreeditOperation::Commit;
  }
  return preedit.empty() ? PreeditOperation::Clear : PreeditOperation::Update;
}

void PreeditState::Apply(const std::string_view preedit) {
  text_.assign(preedit);
}

bool PreeditState::active() const noexcept { return !text_.empty(); }

const std::string& PreeditState::text() const noexcept { return text_; }

}  // namespace zhixu::tsf
