#include "tsf/edit_session.h"
#include "tsf/debug_trace.h"

#include <new>
#include <utility>

namespace zhixu::tsf {

namespace {

HRESULT SetCaretAtRangeEnd(ITfContext* context, ITfRange* range, TfEditCookie cookie,
                          std::optional<std::size_t> cursor = std::nullopt) {
  ITfRange* caret = nullptr;
  HRESULT result = range->Clone(&caret);
  if (FAILED(result)) return result;
  if (cursor) {
    LONG shifted = 0;
    result = caret->ShiftStart(cookie, static_cast<LONG>(*cursor), &shifted, nullptr);
    if (SUCCEEDED(result) && shifted != static_cast<LONG>(*cursor)) result = E_ABORT;
    if (SUCCEEDED(result)) result = caret->Collapse(cookie, TF_ANCHOR_START);
  } else {
    result = caret->Collapse(cookie, TF_ANCHOR_END);
  }
  if (SUCCEEDED(result)) {
    TF_SELECTION selection{.range = caret, .style = {.ase = TF_AE_NONE, .fInterimChar = FALSE}};
    result = context->SetSelection(cookie, 1, &selection);
  }
  caret->Release();
  TraceTsfResult("set-caret-after-text", result);
  return result;
}

class SelectionQuery final : public ITfEditSession {
 public:
  explicit SelectionQuery(ITfContext* context) : context_(context) { context_->AddRef(); }
  ~SelectionQuery() { context_->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
    *object = static_cast<ITfEditSession*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
    TF_SELECTION selection{};
    ULONG fetched = 0;
    HRESULT result = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (SUCCEEDED(result) && fetched == 1 && selection.range != nullptr) {
      BOOL empty = TRUE;
      result = selection.range->IsEmpty(cookie, &empty);
      selected = empty == FALSE;
    } else if (SUCCEEDED(result)) result = E_FAIL;
    if (selection.range) selection.range->Release();
    return result;
  }
  bool selected = false;
 private:
  std::atomic<ULONG> references_{1};
  ITfContext* context_;
};

class CompositionEndSession final : public ITfEditSession {
 public:
  CompositionEndSession(ITfContext* context, ITfComposition** composition, ITfCompositionSink* sink, CommitEventSink& edits)
      : context_(context), composition_(composition), sink_(sink), edits_(edits), target_(*composition),
        generation_(edits.CurrentEditGeneration()) {
    context_->AddRef();
    sink_->AddRef();
    if (target_) target_->AddRef();
  }
  ~CompositionEndSession() { if (target_) target_->Release(); sink_->Release(); context_->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
    *object = static_cast<ITfEditSession*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
    if (!target_) return S_OK;
    ITfComposition* current = target_;
    current->AddRef();
    edits_.OnOwnTextWrittenInContext(context_, generation_);
    const HRESULT result = current->EndComposition(cookie);
    if (SUCCEEDED(result) && *composition_ == current) ReleaseComposition(*composition_);
    current->Release();
    return result;
  }
 private:
  std::atomic<ULONG> references_{1};
  ITfContext* context_;
  ITfComposition** composition_;
  ITfCompositionSink* sink_;
  CommitEventSink& edits_;
  ITfComposition* target_;
  std::uint64_t generation_;
};

void NotifyRequestRejected(TextService& text_service, const CommitIdentity identity,
                           CommitEventSink& sink, const HRESULT error) noexcept {
  CommitOutcome outcome{.kind = CommitOutcomeKind::Rejected,
                        .request_id = identity.request_id,
                        .context_version = text_service.context_version()};
  try {
    outcome = text_service.ApplyCommit(identity, false);
  } catch (...) {
    // Preserve the current context version when state allocation fails.
  }
  try {
    sink.OnCommitRejected(outcome, error);
  } catch (...) {
    // Notifications cannot be allowed to unwind through TSF callbacks.
  }
}

}  // namespace

CommitEditSession::CommitEditSession(ITfContext* const context, TextService& text_service,
                                     const CommitIdentity identity, std::wstring text,
                                     CommitEventSink& sink, ITfComposition** const composition)
    : context_(context),
      text_service_(text_service),
      identity_(identity),
      text_(std::move(text)),
      sink_(sink),
      composition_(composition) {
  context_->AddRef();
  sink_.RetainForEditSession();
}

CommitEditSession::~CommitEditSession() {
  context_->Release();
  sink_.ReleaseForEditSession();
}

HRESULT CommitEditSession::QueryInterface(REFIID iid, void** const object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (iid != IID_IUnknown && iid != IID_ITfEditSession) {
    return E_NOINTERFACE;
  }
  *object = static_cast<ITfEditSession*>(this);
  AddRef();
  return S_OK;
}

ULONG CommitEditSession::AddRef() {
  return ++references_;
}

ULONG CommitEditSession::Release() {
  const ULONG remaining = --references_;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

HRESULT CommitEditSession::DoEditSession(const TfEditCookie edit_cookie) {
  // Validate before modifying application text; async sessions may execute
  // after focus/caret invalidation.
  if (identity_.expected_context_version != text_service_.context_version()) {
    NotifyRequestRejected(text_service_, identity_, sink_, E_ABORT);
    return E_ABORT;
  }
  HRESULT result = E_FAIL;
  if (composition_ != nullptr && *composition_ != nullptr) {
    ITfRange* range = nullptr;
    result = (*composition_)->GetRange(&range);
    if (SUCCEEDED(result)) {
      result = range->SetText(edit_cookie, 0, text_.c_str(), static_cast<LONG>(text_.size()));
      TraceTsfResult("commit-set-text", result);
      if (SUCCEEDED(result)) {
        sink_.OnOwnTextWrittenInContext(context_, identity_.expected_context_version);
        sink_.OnCommittedRangeWritten(context_, range, text_);
        result = SetCaretAtRangeEnd(context_, range, edit_cookie);
      }
      range->Release();
    }
    if (SUCCEEDED(result)) {
      result = (*composition_)->EndComposition(edit_cookie);
      if (SUCCEEDED(result)) {
        ReleaseComposition(*composition_);
      }
    }
  } else {
    ITfInsertAtSelection* insertion = nullptr;
    result = context_->QueryInterface(IID_ITfInsertAtSelection,
                                      reinterpret_cast<void**>(&insertion));
    if (SUCCEEDED(result)) {
      ITfRange* inserted = nullptr;
      result = insertion->InsertTextAtSelection(edit_cookie, 0, text_.c_str(),
                                                 static_cast<LONG>(text_.size()), &inserted);
      if (SUCCEEDED(result) && inserted != nullptr) {
        sink_.OnOwnTextWrittenInContext(context_, identity_.expected_context_version);
        sink_.OnCommittedRangeWritten(context_, inserted, text_);
        result = SetCaretAtRangeEnd(context_, inserted, edit_cookie);
      } else if (SUCCEEDED(result)) result = E_FAIL;
      if (inserted != nullptr) inserted->Release();
      insertion->Release();
    }
  }

  try {
    const CommitOutcome outcome = text_service_.ApplyCommit(identity_, SUCCEEDED(result));
    if (outcome.kind == CommitOutcomeKind::Applied) {
      sink_.OnCommitAppliedInContext(outcome, context_, edit_cookie);
    } else {
      sink_.OnCommitRejected(outcome, result);
    }
  } catch (const std::bad_alloc&) {
    const CommitOutcome outcome{.kind = CommitOutcomeKind::Rejected,
                                .request_id = identity_.request_id,
                                .context_version = text_service_.context_version()};
    sink_.OnCommitRejected(outcome, E_OUTOFMEMORY);
    return E_OUTOFMEMORY;
  }
  return result;
}

HRESULT RequestTextCommit(ITfContext* const context, const TfClientId client_id,
                          TextService& text_service, const CommitIdentity identity,
                          std::wstring text, CommitEventSink& sink) {
  return RequestTextCommit(context, client_id, text_service, identity, std::move(text), sink,
                           nullptr);
}

HRESULT RequestTextCommit(ITfContext* const context, const TfClientId client_id,
                          TextService& text_service, const CommitIdentity identity,
                          std::wstring text, CommitEventSink& sink,
                          ITfComposition** const composition) {
  if (context == nullptr || text.empty()) {
    NotifyRequestRejected(text_service, identity, sink, E_INVALIDARG);
    return E_INVALIDARG;
  }
  auto* const edit_session =
      new (std::nothrow) CommitEditSession(context, text_service, identity, std::move(text), sink,
                                           composition);
  if (edit_session == nullptr) {
    NotifyRequestRejected(text_service, identity, sink, E_OUTOFMEMORY);
    return E_OUTOFMEMORY;
  }
  HRESULT session_result = E_FAIL;
  const HRESULT request_result = context->RequestEditSession(
      client_id, edit_session, TF_ES_READWRITE | TF_ES_ASYNCDONTCARE, &session_result);
  edit_session->Release();
  if (FAILED(request_result)) {
    NotifyRequestRejected(text_service, identity, sink, request_result);
    return request_result;
  }
  return session_result;
}

PreeditEditSession::PreeditEditSession(ITfContext* const context,
                                       ITfComposition** const composition, std::wstring text,
                                       ITfCompositionSink* const sink, CommitEventSink& edit_sink,
                                       std::optional<std::size_t> cursor, TextService* text_service)
    : context_(context), composition_(composition), text_(std::move(text)), sink_(sink), edit_sink_(edit_sink),
      cursor_(cursor), text_service_(text_service),
      expected_context_version_(text_service ? text_service->context_version() : 0) {
  context_->AddRef();
  if (sink_ != nullptr) sink_->AddRef();
}

PreeditEditSession::~PreeditEditSession() {
  context_->Release();
  if (sink_ != nullptr) sink_->Release();
}

HRESULT PreeditEditSession::QueryInterface(REFIID iid, void** const object) {
  if (object == nullptr) return E_POINTER;
  *object = nullptr;
  if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
  *object = static_cast<ITfEditSession*>(this);
  AddRef();
  return S_OK;
}

ULONG PreeditEditSession::AddRef() { return ++references_; }

ULONG PreeditEditSession::Release() {
  const ULONG remaining = --references_;
  if (remaining == 0) delete this;
  return remaining;
}

HRESULT PreeditEditSession::DoEditSession(const TfEditCookie edit_cookie) {
  TraceTsfResult("preedit-execute", S_OK);
  if (composition_ == nullptr || sink_ == nullptr) return E_POINTER;
  if (text_service_ && text_service_->context_version() != expected_context_version_) return E_ABORT;
  if (cursor_ && *cursor_ > text_.size()) return E_INVALIDARG;

  if (*composition_ != nullptr) {
    ITfRange* range = nullptr;
    HRESULT result = (*composition_)->GetRange(&range);
    if (SUCCEEDED(result)) {
      result = range->SetText(edit_cookie, 0, text_.c_str(), static_cast<LONG>(text_.size()));
      TraceTsfResult("preedit-update-text", result);
      if (SUCCEEDED(result)) {
        edit_sink_.OnOwnTextWrittenInContext(context_, expected_context_version_);
        result = SetCaretAtRangeEnd(context_, range, edit_cookie, cursor_);
      }
      range->Release();
    }
    if (SUCCEEDED(result) && text_.empty()) {
      result = (*composition_)->EndComposition(edit_cookie);
      if (SUCCEEDED(result)) ReleaseComposition(*composition_);
    }
    return result;
  }
  if (text_.empty()) return S_OK;

  ITfInsertAtSelection* insertion = nullptr;
  HRESULT result = context_->QueryInterface(IID_ITfInsertAtSelection,
                                             reinterpret_cast<void**>(&insertion));
  TraceTsfResult("preedit-query-insertion", result);
  if (FAILED(result)) return result;
  ITfRange* insertion_range = nullptr;
  result = insertion->InsertTextAtSelection(edit_cookie, TF_IAS_QUERYONLY, L"", 0,
                                             &insertion_range);
  insertion->Release();
  TraceTsfResult("preedit-get-insertion-range", result);
  if (FAILED(result) || insertion_range == nullptr) {
    if (insertion_range != nullptr) insertion_range->Release();
    return FAILED(result) ? result : E_FAIL;
  }

  ITfContextComposition* context_composition = nullptr;
  result = context_->QueryInterface(IID_ITfContextComposition,
                                    reinterpret_cast<void**>(&context_composition));
  if (SUCCEEDED(result)) {
    ITfComposition* created = nullptr;
    result = context_composition->StartComposition(edit_cookie, insertion_range, sink_, &created);
    TraceTsfResult("preedit-start-composition", result);
    context_composition->Release();
    if (SUCCEEDED(result) && created == nullptr) result = E_ABORT;
    if (SUCCEEDED(result)) {
      ITfRange* range = nullptr;
      result = created->GetRange(&range);
      if (SUCCEEDED(result)) {
        result = range->SetText(edit_cookie, 0, text_.c_str(), static_cast<LONG>(text_.size()));
        TraceTsfResult("preedit-set-text", result);
        if (SUCCEEDED(result)) {
          edit_sink_.OnOwnTextWrittenInContext(context_, expected_context_version_);
          result = SetCaretAtRangeEnd(context_, range, edit_cookie, cursor_);
        }
        range->Release();
      }
      if (SUCCEEDED(result)) {
        *composition_ = created;
      } else {
        created->EndComposition(edit_cookie);
        created->Release();
      }
    }
  }
  insertion_range->Release();
  return result;
}

HRESULT RequestPreeditUpdate(ITfContext* const context, const TfClientId client_id,
                             ITfComposition** const composition, std::wstring text,
                             ITfCompositionSink* const sink, CommitEventSink& edit_sink,
                             std::optional<std::size_t> cursor, TextService* text_service) {
  if (context == nullptr || composition == nullptr || sink == nullptr) return E_INVALIDARG;
  auto* const edit_session = new (std::nothrow) PreeditEditSession(context, composition, std::move(text), sink, edit_sink,
                                                               cursor, text_service);
  if (edit_session == nullptr) return E_OUTOFMEMORY;
  HRESULT session_result = E_FAIL;
  const HRESULT request_result = context->RequestEditSession(
      client_id, edit_session, TF_ES_READWRITE | TF_ES_ASYNCDONTCARE, &session_result);
  edit_session->Release();
  return FAILED(request_result) ? request_result : session_result;
}

HRESULT HasSelectedText(ITfContext* context, TfClientId client_id, bool* selected) {
  if (!context || !selected) return E_INVALIDARG;
  *selected = false;
  auto* query = new (std::nothrow) SelectionQuery(context);
  if (!query) return E_OUTOFMEMORY;
  HRESULT session_result = E_FAIL;
  const HRESULT result = context->RequestEditSession(client_id, query, TF_ES_SYNC | TF_ES_READ, &session_result);
  if (SUCCEEDED(result) && SUCCEEDED(session_result)) *selected = query->selected;
  query->Release();
  return FAILED(result) ? result : session_result;
}

HRESULT RequestCompositionEnd(ITfContext* context, TfClientId client_id,
                               ITfComposition** composition, ITfCompositionSink* sink,
                               CommitEventSink& edit_sink) {
  if (!context || !composition || !sink) return E_INVALIDARG;
  auto* edit = new (std::nothrow) CompositionEndSession(context, composition, sink, edit_sink);
  if (!edit) return E_OUTOFMEMORY;
  HRESULT session_result = E_FAIL;
  const HRESULT result = context->RequestEditSession(client_id, edit,
      TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &session_result);
  edit->Release();
  return FAILED(result) ? result : session_result;
}

void ReleaseComposition(ITfComposition*& composition) noexcept {
  if (composition != nullptr) {
    composition->Release();
    composition = nullptr;
  }
}

}  // namespace zhixu::tsf
