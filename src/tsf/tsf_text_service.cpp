#include "tsf/tsf_text_service.h"
#include "tsf/debug_trace.h"
#include "tsf/input_scope_policy.h"
#include "tsf/module_lifetime.h"
#include "common/settings/runtime_settings.h"
#include "common/protocol/recommendation_notification.h"
#include "common/protocol/key_modifiers.h"

#include <inputscope.h>
#include <string>
#include <utility>

namespace zhixu::tsf {

namespace {

constexpr std::uintptr_t kAltKeyDataMask = 1ULL << 29;
constexpr std::uintptr_t kRepeatedKeyDataMask = 1ULL << 30;
constexpr wchar_t kNotificationClass[] = L"ZhixuIME.Tsf.RecommendationNotification";

constexpr GUID kRecommendationCommands[] = {
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x01}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x02}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x03}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x04}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x05}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x06}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x07}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x08}}};
constexpr UINT kRecommendationKeys[] = {'1', '2', '3', '4', '5', '6', VK_OEM_MINUS, VK_OEM_PLUS};
constexpr wchar_t kRecommendationShortcutDescription[] = L"Accept ZhixuIME recommendation";

bool IsShift(WPARAM key) { return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT; }
bool IsNavigation(WPARAM key) {
  return key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
         key == VK_HOME || key == VK_END;
}
bool IsSelectionNavigation(WPARAM key) {
  return IsNavigation(key) || key == VK_PRIOR || key == VK_NEXT;
}
// Text-store display/attribute notifications are not application text edits.
// Keep conservative invalidation when the record cannot be inspected.
bool HasTextOrSelectionChanges(ITfEditRecord* record) {
  if (!record) return true;
  BOOL selection_changed = FALSE;
  HRESULT result = record->GetSelectionStatus(&selection_changed);
  TraceTsfResult("edit-selection-query", result);
  if (FAILED(result)) return true;
  TraceTsfResult("edit-selection-changed", selection_changed ? S_OK : S_FALSE);
  IEnumTfRanges* ranges = nullptr;
  result = record->GetTextAndPropertyUpdates(TF_GTP_INCL_TEXT, nullptr, 0, &ranges);
  TraceTsfResult("edit-text-query", result);
  if (FAILED(result) || !ranges) { if (ranges) ranges->Release(); return true; }
  ITfRange* changed = nullptr;
  ULONG fetched = 0;
  result = ranges->Next(1, &changed, &fetched);
  if (changed) changed->Release();
  ranges->Release();
  const bool text_changed = FAILED(result) || fetched != 0;
  TraceTsfResult("edit-text-changed", text_changed ? S_OK : S_FALSE);
  return selection_changed != FALSE || text_changed;
}

std::uint32_t Modifiers(LPARAM key_data) {
  return (((GetKeyState(VK_MENU) & 0x8000) || (static_cast<std::uintptr_t>(key_data) & kAltKeyDataMask)) ? protocol::kAlt : 0) |
         ((GetKeyState(VK_CONTROL) & 0x8000) ? protocol::kControl : 0) |
         ((GetKeyState(VK_SHIFT) & 0x8000) ? protocol::kShift : 0) |
         ((GetKeyState(VK_CAPITAL) & 1) ? protocol::kCapsLock : 0) |
         (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? protocol::kSuper : 0);
}
std::uint32_t KeySymbol(WPARAM key, LPARAM key_data) {
  BYTE keyboard[256]{};
  WCHAR buffer[4]{};
  if (!GetKeyboardState(keyboard)) return 0;
  // Flag 4 leaves the keyboard's dead-key state untouched (Windows 10+).
  const int count = ToUnicodeEx(static_cast<UINT>(key), static_cast<UINT>((key_data >> 16) & 0xff),
                               keyboard, buffer, 4, 4, GetKeyboardLayout(0));
  if (count == 1 && buffer[0] >= 0x20 && !(buffer[0] >= 0xd800 && buffer[0] <= 0xdfff)) return buffer[0];
  if (count == 2 && buffer[0] >= 0xd800 && buffer[0] <= 0xdbff && buffer[1] >= 0xdc00 && buffer[1] <= 0xdfff)
    return 0x10000 + ((buffer[0] - 0xd800) << 10) + buffer[1] - 0xdc00;
  return 0;
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), wide.data(), count) != count) {
    return {};
  }
  return wide;
}

PrivacyDecision PrivacyForContext(ITfContext* const context) {
  InputScopeContext scope_context;
  ITfInputScope* input_scope = nullptr;
  if (context == nullptr || FAILED(context->QueryInterface(IID_ITfInputScope,
                                                           reinterpret_cast<void**>(&input_scope))) ||
      input_scope == nullptr) {
    return ApplyApplicationSettings(EvaluateInputScope(scope_context), settings::LoadRuntimeSettings(), settings::CurrentApplicationHash());
  }
  ::InputScope* scopes = nullptr;
  UINT count = 0;
  if (SUCCEEDED(input_scope->GetInputScopes(&scopes, &count))) {
    for (UINT index = 0; index < count; ++index) {
      if (scopes[index] == IS_PASSWORD) scope_context.input_scope = InputScope::Password;
      if (scopes[index] == IS_NUMERIC_PIN || scopes[index] == IS_ALPHANUMERIC_PIN ||
          scopes[index] == IS_ALPHANUMERIC_PIN_SET) {
        scope_context.input_scope = InputScope::Pin;
      }
    }
    CoTaskMemFree(scopes);
  }
  input_scope->Release();
  return ApplyApplicationSettings(EvaluateInputScope(scope_context), settings::LoadRuntimeSettings(), settings::CurrentApplicationHash());
}

}  // namespace

TsfTextService::TsfTextService(EngineActionTransport* const transport, NativeCaretProbe native_caret_probe)
    : transport_(transport), native_caret_probe_(native_caret_probe), key_event_sink_(transport_) {
  AddModuleReference();
}

TsfTextService::TsfTextService(std::unique_ptr<EngineActionTransport> transport)
    : owned_transport_(std::move(transport)),
      transport_(owned_transport_.get()),
      key_event_sink_(transport_) {
  AddModuleReference();
}

TsfTextService::~TsfTextService() {
  ResetActivation();
  ReleaseComposition(composition_);
  ReleaseModuleReference();
}

HRESULT TsfTextService::QueryInterface(REFIID iid, void** const object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (iid == IID_IUnknown || iid == IID_ITfTextInputProcessor ||
      iid == IID_ITfTextInputProcessorEx) {
    *object = static_cast<ITfTextInputProcessorEx*>(this);
  } else if (iid == IID_ITfKeyEventSink) {
    *object = static_cast<ITfKeyEventSink*>(this);
  } else if (iid == IID_ITfThreadMgrEventSink) {
    *object = static_cast<ITfThreadMgrEventSink*>(this);
  } else if (iid == IID_ITfTextEditSink) {
    *object = static_cast<ITfTextEditSink*>(this);
  } else if (iid == IID_ITfCompositionSink) {
    *object = static_cast<ITfCompositionSink*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

ULONG TsfTextService::AddRef() {
  return ++references_;
}

ULONG TsfTextService::Release() {
  const ULONG remaining = --references_;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

HRESULT TsfTextService::Activate(ITfThreadMgr* const thread_manager, const TfClientId client_id) {
  return ActivateEx(thread_manager, client_id, 0);
}

HRESULT TsfTextService::Deactivate() {
  ResetActivation();
  return S_OK;
}

HRESULT TsfTextService::ActivateEx(ITfThreadMgr* const thread_manager, const TfClientId client_id,
                                    const DWORD) {
  static_cast<void>(settings::CurrentApplicationHash());
  TraceTsfResult("activate-enter", S_OK);
  if (thread_manager == nullptr) {
    return E_INVALIDARG;
  }

  ResetActivation();
  thread_manager_ = thread_manager;
  thread_manager_->AddRef();
  client_id_ = client_id;

  HRESULT result = thread_manager_->QueryInterface(
      IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keystroke_manager_));
  if (FAILED(result)) {
    ResetActivation();
    return result;
  }
  result = keystroke_manager_->AdviseKeyEventSink(client_id_, static_cast<ITfKeyEventSink*>(this),
                                                    TRUE);
  if (FAILED(result)) {
    ResetActivation();
    return result;
  }
  key_event_sink_advised_ = true;

  // Alt chords can arrive as TSF preserved keys instead of normal key events.
  // Keep normal input available if another service already owns a chord.
  for (unsigned int i = 0; i < std::size(kRecommendationKeys); ++i) {
    const TF_PRESERVEDKEY chord{kRecommendationKeys[i], TF_MOD_ALT};
    const auto preserved = keystroke_manager_->PreserveKey(client_id_, kRecommendationCommands[i],
        &chord, kRecommendationShortcutDescription,
        static_cast<ULONG>(std::size(kRecommendationShortcutDescription) - 1));
    recommendation_keys_preserved_[i] = SUCCEEDED(preserved);
    TraceTsfResult("preserve-recommendation-key", preserved);
  }

  result = thread_manager_->QueryInterface(
      IID_ITfSource, reinterpret_cast<void**>(&thread_manager_source_));
  if (FAILED(result)) {
    ResetActivation();
    return result;
  }
  result = thread_manager_source_->AdviseSink(IID_ITfThreadMgrEventSink,
                                               static_cast<ITfThreadMgrEventSink*>(this),
                                               &thread_manager_sink_cookie_);
  if (FAILED(result)) {
    ResetActivation();
    return result;
  }
  if (!CreateNotificationWindow()) {
    ResetActivation();
    return E_FAIL;
  }
  return S_OK;
}

bool TsfTextService::CreateNotificationWindow() {
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&NotificationWindowProcedure), &notification_module_)) return false;
  WNDCLASSEXW window_class{.cbSize = sizeof(window_class),
                           .lpfnWndProc = NotificationWindowProcedure,
                           .hInstance = notification_module_, .lpszClassName = kNotificationClass};
  if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  notification_window_ = CreateWindowExW(0, kNotificationClass, L"", 0, 0, 0, 0, 0,
                                         HWND_MESSAGE, nullptr, notification_module_, this);
  return notification_window_ != nullptr;
}

LRESULT CALLBACK TsfTextService::NotificationWindowProcedure(
    const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) {
  auto* service = reinterpret_cast<TsfTextService*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    service = static_cast<TsfTextService*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(service));
  }
  if (message == protocol::kRecommendationSelectedMessage && service != nullptr) {
    service->AddRef();
    try { service->PollRecommendation(); } catch (...) { /* Never unwind into the host message loop. */ }
    service->Release();
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

void TsfTextService::PollRecommendation() {
  if (transport_ == nullptr || thread_manager_ == nullptr || caret_foreground_window_ == nullptr ||
      GetForegroundWindow() != caret_foreground_window_) return;
  ITfDocumentMgr* document = nullptr;
  if (FAILED(thread_manager_->GetFocus(&document)) || document == nullptr) return;
  ITfContext* context = nullptr;
  const HRESULT top_result = document->GetTop(&context);
  document->Release();
  if (FAILED(top_result) || context == nullptr) return;
  if (observed_context_ == nullptr || context != observed_context_) {
    context->Release();
    return;
  }
  const auto privacy = PrivacyForContext(context);
  protocol::v1::PrivacyDecision wire_privacy;
  wire_privacy.set_allow_composition(privacy.allow_composition);
  wire_privacy.set_allow_learning(privacy.allow_learning);
  wire_privacy.set_allow_prediction(privacy.allow_prediction);
  const auto action = transport_->PollRecommendation(text_service_.context_version(), wire_privacy,
                                                     std::chrono::milliseconds(10));
  if (action && action->consumed() && privacy.allow_composition) {
    if (FAILED(ApplyEngineAction(context, *action))) {
      NotifyInvalidated(InvalidationReason::ExternalEdit);
      preedit_state_.Apply("");
      RequestCompositionEnd(context, client_id_, &composition_, static_cast<ITfCompositionSink*>(this), *this);
    }
  }
  context->Release();
}

HRESULT TsfTextService::OnSetFocus(const BOOL foreground) {
  if (transport_ != nullptr) {
    // This describes TSF service ordering, not Windows foreground ownership.
    // Connect only for a real key query; background Explorer must not grab it.
    if (!foreground) {
      EndFocusedComposition();
      NotifyInvalidated(InvalidationReason::FocusChanged);
      transport_->Disconnect();
      TraceTsfResult("focus-disconnect", S_OK);
    }
  }
  return S_OK;
}

HRESULT TsfTextService::OnTestKeyDown(ITfContext* const context, const WPARAM virtual_key, const LPARAM key_data,
                                       BOOL* const eaten) {
  ClearCommittedRange();
  own_edit_pending_ = false;
  if (eaten == nullptr) {
    return E_POINTER;
  }
  if (!IsShift(virtual_key)) shift_tap_pending_ = false;
  *eaten = WantsCompositionKey(virtual_key, key_data) && !LeavesNativeKey(virtual_key, key_data) ? TRUE : FALSE;
  if (virtual_key == VK_BACK || virtual_key == VK_DELETE) {
    bool selected = false;
    if (context != nullptr && SUCCEEDED(HasSelectedText(context, client_id_, &selected)) && selected) {
      *eaten = FALSE;  // Let the application delete its actual selection.
    }
  }
  if ((virtual_key == VK_BACK || virtual_key == VK_DELETE || virtual_key == VK_LEFT || virtual_key == VK_RIGHT) &&
      !preedit_state_.active()) *eaten = FALSE;
  if ((IsNavigation(virtual_key) || virtual_key == VK_RETURN) && !preedit_state_.active()) *eaten = FALSE;
  if (IsSelectionNavigation(virtual_key) && (Modifiers(key_data) & protocol::kShift)) *eaten = FALSE;
  if (*eaten && (transport_ == nullptr || !transport_->Connect(std::chrono::milliseconds(10)))) *eaten = FALSE;
  if (virtual_key == VK_SPACE) TraceTsfResult("test-key-space", *eaten ? S_OK : S_FALSE);
  return S_OK;
}

HRESULT TsfTextService::OnTestKeyUp(ITfContext*, const WPARAM key, const LPARAM, BOOL* const eaten) {
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = IsShift(key) && transport_ != nullptr &&
           transport_->Connect(std::chrono::milliseconds(10)) ? TRUE : FALSE;
  return S_OK;
}

HRESULT TsfTextService::OnKeyDown(ITfContext* const context, const WPARAM virtual_key, const LPARAM key_data,
                                  BOOL* const eaten) {
  ClearCommittedRange();
  own_edit_pending_ = false;
  return HandleKey(context, virtual_key, key_data, eaten, true);
}

HRESULT TsfTextService::HandleKey(ITfContext* const context, const WPARAM virtual_key, const LPARAM key_data,
                                  BOOL* const eaten, const bool down, const bool cancel_shift) {
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  if (down) {
    shift_tap_pending_ = IsShift(virtual_key) && !(static_cast<std::uintptr_t>(key_data) & kRepeatedKeyDataMask);
  }
  if (down && (!WantsCompositionKey(virtual_key, key_data) || LeavesNativeKey(virtual_key, key_data))) {
    return S_OK;
  }
  if ((virtual_key == VK_BACK || virtual_key == VK_DELETE || virtual_key == VK_LEFT || virtual_key == VK_RIGHT) &&
      !preedit_state_.active()) return S_OK;
  if ((IsNavigation(virtual_key) || virtual_key == VK_RETURN) && !preedit_state_.active()) return S_OK;
  if (IsSelectionNavigation(virtual_key) && (Modifiers(key_data) & protocol::kShift)) return S_OK;
  if ((virtual_key == VK_BACK || virtual_key == VK_DELETE) && context != nullptr) {
    bool selected = false;
    if (SUCCEEDED(HasSelectedText(context, client_id_, &selected)) && selected) return S_OK;
  }
  TraceTsfResult("key-is-space", virtual_key == VK_SPACE ? S_OK : S_FALSE);
  if (context != nullptr) TraceTsfResult("observe-key-context", ObserveContext(context));
  const auto privacy = PrivacyForContext(context);
  const auto key_flags = static_cast<std::uintptr_t>(key_data);
  const auto action = key_event_sink_.Handle({.virtual_key = virtual_key,
                                               .key_data = key_data,
                                               .context_version = text_service_.context_version(),
                                               .modifiers = cancel_shift ? protocol::kShift | protocol::kControl
                                                                        : Modifiers(key_data),
                                               .repeated = (key_flags & kRepeatedKeyDataMask) != 0,
                                               .allow_composition = privacy.allow_composition,
                                               .allow_learning = privacy.allow_learning,
                                               .allow_prediction = privacy.allow_prediction,
                                               .key_down = down,
                                               .key_symbol = down ? KeySymbol(virtual_key, key_data) : 0});
  if (action) {
    ascii_mode_ = action->composition().ascii_mode();
    full_shape_ = action->composition().full_shape();
    TraceTsfResult("action-has-candidates", action->composition().candidates_size() > 0 ? S_OK : S_FALSE);
    TraceTsfResult("action-has-commit", action->has_commit_request() ? S_OK : S_FALSE);
  }
  if (!action || !action->consumed()) {
    TraceTsfResult("key-action-not-consumed", action ? S_FALSE : E_FAIL);
    return S_OK;
  }
  TraceTsfResult("key-action-consumed", S_OK);
  *eaten = TRUE;
  const auto writes_before_edit = own_text_write_serial_;
  const HRESULT edit_result = ApplyEngineAction(context, *action);
  if (FAILED(edit_result)) {
    // Rime consumption alone cannot justify swallowing the application key.
    // A failed caret update may follow a successful text write, however;
    // releasing that same key would insert it twice.
    if (own_text_write_serial_ == writes_before_edit) *eaten = FALSE;
    TraceTsfResult("failed-edit-key-released", *eaten ? S_FALSE : S_OK);
    NotifyInvalidated(InvalidationReason::ExternalEdit);
    preedit_state_.Apply("");
    if (composition_ != nullptr && context != nullptr) {
      TraceTsfResult("end-failed-composition", RequestCompositionEnd(context, client_id_,
          &composition_, static_cast<ITfCompositionSink*>(this), *this));
    }
  }
  return S_OK;
}

HRESULT TsfTextService::ApplyEngineAction(ITfContext* context, const protocol::v1::EngineAction& action) {
  HRESULT edit_result = S_OK;
  const auto& preedit = action.composition().preedit();
  if (preedit_state_.Next(preedit, action.has_commit_request()) == PreeditOperation::Commit) {
    const auto& request = action.commit_request();
    if (request.expected_context_version() != text_service_.context_version()) {
      if (transport_) transport_->CommitRejected(request.request_id(), E_ABORT);
      return E_ABORT;
    }
    edit_result = RequestTextCommit(context, client_id_, text_service_,
                                    {.request_id = request.request_id(),
                                     .expected_context_version = request.expected_context_version()},
                                    Utf8ToWide(request.text()), *this, &composition_);
    TraceTsfResult("commit-request", edit_result);
    if (SUCCEEDED(edit_result)) {
      preedit_state_.Apply("");
    }
  } else if (!preedit.empty() || composition_ != nullptr) {
    edit_result = RequestPreeditUpdate(context, client_id_, &composition_,
        Utf8ToWide(preedit), static_cast<ITfCompositionSink*>(this), *this,
        action.composition().has_cursor()
            ? std::optional<std::size_t>{Utf8ToWide(preedit.substr(0, action.composition().cursor())).size()}
            : std::nullopt, &text_service_);
    TraceTsfResult("preedit-request", edit_result);
    if (SUCCEEDED(edit_result)) {
      preedit_state_.Apply(preedit);
    }
  } else {
    // Mode/shortcut actions with no composition need no document write.
    preedit_state_.Apply("");
  }
  return edit_result;
}

HRESULT TsfTextService::OnKeyUp(ITfContext* context, const WPARAM key, const LPARAM data, BOOL* const eaten) {
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  if (IsShift(key)) return HandleKey(context, key, data, eaten, false,
                                     !std::exchange(shift_tap_pending_, false));
  return S_OK;
}

HRESULT TsfTextService::OnPreservedKey(ITfContext* context, REFGUID guid, BOOL* const eaten) {
  ClearCommittedRange();
  own_edit_pending_ = false;
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  for (unsigned int i = 0; i < std::size(kRecommendationKeys); ++i) {
    if (guid == kRecommendationCommands[i]) {
      // The command GUID carries Alt even when its physical key state has changed.
      return HandleKey(context, kRecommendationKeys[i], static_cast<LPARAM>(kAltKeyDataMask), eaten, true);
    }
  }
  return S_OK;
}

HRESULT TsfTextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
HRESULT TsfTextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
HRESULT TsfTextService::OnSetFocus(ITfDocumentMgr*, ITfDocumentMgr*) {
  TraceTsfResult("document-focus-changed", S_OK);
  EndFocusedComposition();
  NotifyInvalidated(InvalidationReason::FocusChanged);
  ReleaseObservedContext();
  if (transport_) transport_->Disconnect();
  return S_OK;
}

HRESULT TsfTextService::OnPushContext(ITfContext* const context) {
  const HRESULT result = ObserveContext(context);
  if (SUCCEEDED(result)) NotifyInvalidated(InvalidationReason::CaretMoved);
  return result;
}

HRESULT TsfTextService::ObserveContext(ITfContext* const context) {
  if (context == nullptr) {
    return E_INVALIDARG;
  }
  if (context == observed_context_) return S_OK;
  if (observed_context_ != nullptr) {
    EndFocusedComposition();
    NotifyInvalidated(InvalidationReason::FocusChanged);
  }
  ReleaseObservedContext();

  HRESULT result = context->QueryInterface(IID_ITfSource,
                                           reinterpret_cast<void**>(&observed_context_source_));
  if (FAILED(result)) {
    return result;
  }
  result = observed_context_source_->AdviseSink(IID_ITfTextEditSink,
                                                 static_cast<ITfTextEditSink*>(this),
                                                 &text_edit_sink_cookie_);
  if (FAILED(result)) {
    observed_context_source_->Release();
    observed_context_source_ = nullptr;
    return result;
  }
  observed_context_ = context;
  observed_context_->AddRef();
  return S_OK;
}

HRESULT TsfTextService::OnPopContext(ITfContext* const context) {
  if (context == observed_context_) {
    EndFocusedComposition();
    ReleaseObservedContext();
  }
  NotifyInvalidated(InvalidationReason::CaretMoved);
  return S_OK;
}

HRESULT TsfTextService::OnEndEdit(ITfContext* const context, const TfEditCookie read_cookie, ITfEditRecord* const edit_record) {
  if (context != observed_context_) return S_OK;
  // Attribute/layout callbacks can precede the actual text/selection record.
  // They must not consume the ownership marker for that pending text write.
  const bool text_or_selection_changed = HasTextOrSelectionChanges(edit_record);
  if (!text_or_selection_changed) {
    // Geometry can still settle below without changing the document generation.
  } else if (std::exchange(own_edit_pending_, false)) {
    // A commit already advanced the version; preedit/caret writes must not.
  } else if (IsRepeatedCommittedEdit(context, read_cookie, edit_record)) {
    // Some hosts emit another text record while settling the first insertion.
  } else {
    // Selection changes / application deletions invalidate Rime's old input.
    // Do not erase application text here; only end its IME composition marker.
    NotifyInvalidated(InvalidationReason::ExternalEdit);
    preedit_state_.Apply("");
    if (composition_ != nullptr) {
      TraceTsfResult("end-external-composition", RequestCompositionEnd(context, client_id_,
          &composition_, static_cast<ITfCompositionSink*>(this), *this));
    }
  }
  UpdateCaret(context, read_cookie);
  return S_OK;
}

std::optional<NativeCaretSnapshot> TsfTextService::ReadNativeCaret() const noexcept {
  NativeCaretSnapshot snapshot;
  if (native_caret_probe_) {
    if (!native_caret_probe_(snapshot)) return std::nullopt;
    return snapshot;
  }
  GUITHREADINFO info{.cbSize = sizeof(info)};
  snapshot.foreground = GetForegroundWindow();
  if (!GetGUIThreadInfo(GetCurrentThreadId(), &info) || !snapshot.foreground ||
      !info.hwndFocus || !info.hwndCaret ||
      GetAncestor(info.hwndFocus, GA_ROOT) != snapshot.foreground) return std::nullopt;
  snapshot.focus = info.hwndFocus;
  snapshot.caret_window = info.hwndCaret;
  snapshot.caret = info.rcCaret;
  return snapshot;
}

void TsfTextService::ClearCommittedRange() noexcept {
  transitory_cleanup_pending_ = false;
  cleanup_native_caret_.reset();
  if (committed_range_) committed_range_->Release();
  committed_range_ = nullptr;
  committed_text_.clear();
  committed_version_ = 0;
  committed_deadline_ = 0;
}

void TsfTextService::OnCommittedRangeWritten(ITfContext* context, ITfRange* range,
                                             const std::wstring& text) noexcept {
  ClearCommittedRange();
  if (context != observed_context_ || !range || text.empty()) return;
  try {
    committed_text_ = text;
    if (FAILED(range->Clone(&committed_range_)) || !committed_range_) ClearCommittedRange();
  } catch (...) { ClearCommittedRange(); }
}

bool TsfTextService::IsRepeatedCommittedEdit(ITfContext* context, TfEditCookie cookie,
                                             ITfEditRecord* record) noexcept {
  if (!committed_range_ || !record || context != observed_context_ ||
      committed_version_ != text_service_.context_version() || !committed_deadline_ ||
      GetTickCount64() > committed_deadline_) {
    ClearCommittedRange();
    return false;
  }
  // Never obtain neighboring document text. Only re-read our retained owned span.
  try {
    std::wstring actual(committed_text_.size() + 1, L'\0');
    ULONG length = 0;
    if (FAILED(committed_range_->GetText(cookie, 0, actual.data(),
            static_cast<ULONG>(actual.size()), &length))) return false;
    if (length == 0) return IsTransitoryBufferCleanup(context, cookie, record);
    if (length != committed_text_.size() ||
        actual.compare(0, length, committed_text_) != 0) return false;
  } catch (...) { return false; }
  TF_SELECTION selection{};
  ULONG fetched = 0;
  const auto result = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
  BOOL empty = FALSE;
  LONG at_end = 1;
  const bool same_caret = SUCCEEDED(result) && fetched == 1 && selection.range &&
      SUCCEEDED(selection.range->IsEmpty(cookie, &empty)) && empty &&
      SUCCEEDED(selection.range->CompareStart(cookie, committed_range_, TF_ANCHOR_END, &at_end)) && at_end == 0;
  if (selection.range) selection.range->Release();
  if (!same_caret) return false;
  BOOL selection_changed = FALSE;
  if (FAILED(record->GetSelectionStatus(&selection_changed))) return false;
  IEnumTfRanges* updates = nullptr;
  if (FAILED(record->GetTextAndPropertyUpdates(TF_GTP_INCL_TEXT, nullptr, 0, &updates)) || !updates) {
    if (updates) updates->Release();
    return false;
  }
  bool saw_text = false;
  bool matches = true;
  for (;;) {
    ITfRange* changed = nullptr;
    fetched = 0;
    const auto next = updates->Next(1, &changed, &fetched);
    if (FAILED(next) || (fetched && !changed)) matches = false;
    if (!matches || !fetched) { if (changed) changed->Release(); break; }
    saw_text = true;
    LONG start = -1, end = 1;
    empty = TRUE;
    // An empty boundary update can be an adjacent external deletion: fail closed.
    matches = SUCCEEDED(changed->IsEmpty(cookie, &empty)) && !empty &&
        SUCCEEDED(changed->CompareStart(cookie, committed_range_, TF_ANCHOR_START, &start)) && start >= 0 &&
        SUCCEEDED(changed->CompareEnd(cookie, committed_range_, TF_ANCHOR_END, &end)) && end <= 0;
    changed->Release();
    if (!matches) break;
  }
  updates->Release();
  return matches && (saw_text || selection_changed);
}

bool TsfTextService::IsTransitoryBufferCleanup(ITfContext* context, TfEditCookie cookie,
                                               ITfEditRecord* record) noexcept {
  if (!std::exchange(transitory_cleanup_pending_, false) || composition_ || !cleanup_native_caret_) return false;
  const auto native = ReadNativeCaret();
  if (!native || !native->Matches(*cleanup_native_caret_)) { TraceTsfResult("cleanup-native-caret-mismatch", E_FAIL); return false; }
  TF_STATUS status{};
  const auto status_result = context->GetStatus(&status);
  TraceTsfResult("cleanup-context-status", status_result);
  if (FAILED(status_result) || !(status.dwStaticFlags & TF_SS_TRANSITORY)) return false;
  BOOL empty = FALSE;
  if (FAILED(committed_range_->IsEmpty(cookie, &empty)) || !empty) return false;
  ITfRange* start = nullptr;
  ITfRange* end = nullptr;
  const auto start_result = context->GetStart(cookie, &start);
  const auto end_result = context->GetEnd(cookie, &end);
  LONG compared = 1;
  const bool context_empty = SUCCEEDED(start_result) && SUCCEEDED(end_result) && start && end &&
      SUCCEEDED(start->CompareStart(cookie, end, TF_ANCHOR_END, &compared)) && compared == 0;
  if (end) end->Release();
  if (!context_empty) { TraceTsfResult("cleanup-context-nonempty", E_FAIL); if (start) start->Release(); return false; }
  TF_SELECTION selection{};
  ULONG fetched = 0;
  const auto selection_result = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
  compared = 1;
  const bool selection_matches = SUCCEEDED(selection_result) && fetched == 1 && selection.range &&
      SUCCEEDED(selection.range->IsEmpty(cookie, &empty)) && empty &&
      SUCCEEDED(selection.range->CompareStart(cookie, start, TF_ANCHOR_START, &compared)) && compared == 0;
  if (selection.range) selection.range->Release();
  IEnumTfRanges* ranges = nullptr;
  const auto ranges_result = record->GetTextAndPropertyUpdates(TF_GTP_INCL_TEXT, nullptr, 0, &ranges);
  ITfRange* changed = nullptr;
  fetched = 0;
  bool matches = selection_matches && SUCCEEDED(ranges_result) && ranges &&
      SUCCEEDED(ranges->Next(1, &changed, &fetched)) && fetched == 1 && changed;
  if (matches) {
    compared = 1;
    matches = SUCCEEDED(changed->IsEmpty(cookie, &empty)) && empty &&
        SUCCEEDED(changed->CompareStart(cookie, start, TF_ANCHOR_START, &compared)) && compared == 0;
  }
  if (changed) changed->Release();
  changed = nullptr;
  if (matches) {
    fetched = 0;
    matches = SUCCEEDED(ranges->Next(1, &changed, &fetched)) && fetched == 0;
  }
  if (changed) changed->Release();
  if (ranges) ranges->Release();
  start->Release();
  TraceTsfResult("transitory-buffer-cleanup", matches ? S_OK : E_FAIL);
  return matches;
}

void TsfTextService::EndFocusedComposition() {
  ClearCommittedRange();
  if (composition_ && observed_context_) {
    const auto result = RequestCompositionEnd(observed_context_, client_id_, &composition_,
                                              static_cast<ITfCompositionSink*>(this), *this);
    TraceTsfResult("end-focus-composition", result);
    // The end session retains its old target, detached from the next context.
  }
  // Even a host refusing cleanup cannot lend its range to the next document.
  ReleaseComposition(composition_);
  preedit_state_.Apply("");
  shift_tap_pending_ = false;
  own_edit_pending_ = false;
}

void TsfTextService::OnCommitApplied(const CommitOutcome& outcome) {
  TraceTsfResult("commit-applied", S_OK);
  if (transport_ != nullptr) {
    transport_->CommitApplied(outcome.request_id, outcome.context_version);
  }
}

void TsfTextService::OnCommitAppliedInContext(const CommitOutcome& outcome,
                                               ITfContext* context, TfEditCookie cookie) {
  if (committed_range_ && context == observed_context_ &&
      outcome.context_version == text_service_.context_version()) {
    committed_version_ = outcome.context_version;
    committed_deadline_ = GetTickCount64() + 500;
    TF_STATUS status{};
    const auto status_result = context->GetStatus(&status);
    TraceTsfResult("commit-context-status", status_result);
    if (SUCCEEDED(status_result)) TraceTsfResult("commit-context-static-flags", static_cast<HRESULT>(status.dwStaticFlags));
    if (SUCCEEDED(status_result) && (status.dwStaticFlags & TF_SS_TRANSITORY)) {
      cleanup_native_caret_ = ReadNativeCaret();
      transitory_cleanup_pending_ = cleanup_native_caret_.has_value();
      TraceTsfResult("commit-native-caret", transitory_cleanup_pending_ ? S_OK : E_FAIL);
    }
  } else ClearCommittedRange();
  OnCommitApplied(outcome);
  // Some native hosts report OnEndEdit before ApplyCommit advances the version.
  // Publish the committed caret using the still-valid edit cookie, after the ack.
  if (context == observed_context_ && outcome.context_version == text_service_.context_version()) {
    UpdateCaret(context, cookie);
  }
}

HRESULT TsfTextService::OnCompositionTerminated(const TfEditCookie,
                                                ITfComposition* const composition) {
  if (composition != nullptr && composition == composition_) {
    ReleaseComposition(composition_);
    preedit_state_.Apply("");
    if (!own_edit_pending_) NotifyInvalidated(InvalidationReason::ExternalEdit);
  }
  return S_OK;
}

void TsfTextService::OnCommitRejected(const CommitOutcome& outcome, const HRESULT error) {
  ClearCommittedRange();
  TraceTsfResult("commit-rejected", error);
  if (transport_ != nullptr) {
    transport_->CommitRejected(outcome.request_id, error);
  }
}

void TsfTextService::NotifyInvalidated(const InvalidationReason reason) noexcept {
  ClearCommittedRange();
  TraceTsfResult(reason == InvalidationReason::FocusChanged ? "invalidate-focus" :
      "invalidate-caret", S_OK);
  caret_foreground_window_ = nullptr;
  text_service_.Invalidate(reason);
  if (transport_ == nullptr) return;
  const auto wire_reason = reason == InvalidationReason::FocusChanged
                               ? protocol::v1::FOCUS_CHANGED
                               : protocol::v1::CARET_MOVED;
  transport_->InvalidateSession(wire_reason, text_service_.context_version());
}

void TsfTextService::UpdateCaret(ITfContext* const context, const TfEditCookie read_cookie) noexcept {
  if (transport_ == nullptr || context == nullptr) return;
  ITfContextView* view = nullptr;
  TF_SELECTION selection{};
  ULONG fetched = 0;
  RECT caret{};
  BOOL clipped = FALSE;
  HWND window = nullptr;
  HRESULT result = context->GetActiveView(&view);
  TraceTsfResult("caret-get-view", result);
  if (SUCCEEDED(result) && view != nullptr) {
    result = context->GetSelection(read_cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    TraceTsfResult("caret-get-selection", result);
  }
  if (SUCCEEDED(result) && view != nullptr && fetched == 1 && selection.range != nullptr) {
    result = view->GetTextExt(read_cookie, selection.range, &caret, &clipped);
    TraceTsfResult("caret-get-text-ext", result);
    if (SUCCEEDED(result)) {
      result = view->GetWnd(&window);
      TraceTsfResult("caret-get-window", result);
    }
  }
  if (FAILED(result) || view == nullptr || fetched != 1 || selection.range == nullptr || window == nullptr) {
    if (selection.range != nullptr) selection.range->Release();
    if (view != nullptr) view->Release();
    return;
  }
  selection.range->Release();
  view->Release();
  MONITORINFO monitor{.cbSize = sizeof(monitor)};
  if (GetMonitorInfoW(MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST), &monitor) == FALSE) return;
  protocol::v1::CaretUpdate update;
  update.mutable_caret()->set_left(caret.left);
  update.mutable_caret()->set_top(caret.top);
  update.mutable_caret()->set_right(caret.right);
  update.mutable_caret()->set_bottom(caret.bottom);
  update.mutable_work_area()->set_left(monitor.rcWork.left);
  update.mutable_work_area()->set_top(monitor.rcWork.top);
  update.mutable_work_area()->set_right(monitor.rcWork.right);
  update.mutable_work_area()->set_bottom(monitor.rcWork.bottom);
  update.set_dpi(GetDpiForWindow(window));
  update.set_foreground_window(reinterpret_cast<std::uintptr_t>(GetForegroundWindow()));
  caret_foreground_window_ = GetForegroundWindow();
  update.set_notification_window(reinterpret_cast<std::uintptr_t>(notification_window_));
  TraceTsfResult("caret-positive-width", caret.right > caret.left ? S_OK : S_FALSE);
  TraceTsfResult("caret-positive-height", caret.bottom > caret.top ? S_OK : S_FALSE);
  TraceTsfResult("caret-send-update", S_OK);
  transport_->UpdateCaret(update, text_service_.context_version());
}

void TsfTextService::ReleaseObservedContext() noexcept {
  ClearCommittedRange();
  if (observed_context_source_ != nullptr && text_edit_sink_cookie_ != TF_INVALID_COOKIE) {
    observed_context_source_->UnadviseSink(text_edit_sink_cookie_);
  }
  text_edit_sink_cookie_ = TF_INVALID_COOKIE;
  if (observed_context_source_ != nullptr) {
    observed_context_source_->Release();
    observed_context_source_ = nullptr;
  }
  if (observed_context_ != nullptr) {
    observed_context_->Release();
    observed_context_ = nullptr;
  }
}

void TsfTextService::ResetActivation() noexcept {
  EndFocusedComposition();
  text_service_.Invalidate(InvalidationReason::Disconnected);
  if (transport_ != nullptr) transport_->Disconnect();
  if (notification_window_ != nullptr) {
    DestroyWindow(notification_window_);
    notification_window_ = nullptr;
  }
  if (notification_module_ != nullptr) {
    UnregisterClassW(kNotificationClass, notification_module_);
    notification_module_ = nullptr;
  }
  caret_foreground_window_ = nullptr;
  ReleaseComposition(composition_);
  preedit_state_.Apply("");
  own_edit_pending_ = false;
  ReleaseObservedContext();
  if (thread_manager_source_ != nullptr && thread_manager_sink_cookie_ != TF_INVALID_COOKIE) {
    thread_manager_source_->UnadviseSink(thread_manager_sink_cookie_);
  }
  thread_manager_sink_cookie_ = TF_INVALID_COOKIE;
  if (thread_manager_source_ != nullptr) {
    thread_manager_source_->Release();
    thread_manager_source_ = nullptr;
  }
  if (keystroke_manager_ != nullptr) {
    for (unsigned int i = 0; i < std::size(kRecommendationKeys); ++i) {
      if (recommendation_keys_preserved_[i]) {
        const TF_PRESERVEDKEY chord{kRecommendationKeys[i], TF_MOD_ALT};
        keystroke_manager_->UnpreserveKey(kRecommendationCommands[i], &chord);
        recommendation_keys_preserved_[i] = false;
      }
    }
    if (key_event_sink_advised_) {
      keystroke_manager_->UnadviseKeyEventSink(client_id_);
    }
    keystroke_manager_->Release();
    keystroke_manager_ = nullptr;
  }
  key_event_sink_advised_ = false;
  client_id_ = TF_CLIENTID_NULL;
  if (thread_manager_ != nullptr) {
    thread_manager_->Release();
    thread_manager_ = nullptr;
  }
}

bool TsfTextService::LeavesNativeKey(const WPARAM key, const LPARAM data) const noexcept {
  if (preedit_state_.active()) return false;
  const auto held = Modifiers(data) & protocol::kHeldModifiers;
  if (held & (protocol::kControl | protocol::kAlt | protocol::kSuper)) return false;
  if (IsShift(key) || key == VK_CAPITAL) return false;
  // Shift+Space remains an engine-owned width toggle in either input mode.
  if (key == VK_SPACE && held == protocol::kShift) return false;
  if (full_shape_) return false;
  // IMM-backed editors need the test callback to release idle half-width Space.
  // Active composition and width toggles remain engine-owned above.
  if (key == VK_SPACE && held == 0) return true;
  if (ascii_mode_) return true;
  // Shift+number produces punctuation and must reach the Chinese engine.
  return (held == 0 && key >= '0' && key <= '9') || (key >= VK_NUMPAD0 && key <= VK_DIVIDE);
}
bool TsfTextService::WantsCompositionKey(const WPARAM virtual_key, const LPARAM key_data) noexcept {
  const auto held = Modifiers(key_data) & protocol::kHeldModifiers;
  // Preserve the existing explicit recommendation acceptance chord.
  if (held == protocol::kAlt && ((virtual_key >= '1' && virtual_key <= '6') || virtual_key == VK_OEM_MINUS || virtual_key == VK_OEM_PLUS)) return true;
  if (held & (protocol::kAlt | protocol::kSuper)) return false;
  if ((held & protocol::kControl) && !(held == protocol::kControl && virtual_key == VK_OEM_PERIOD)) return false;
  if (virtual_key == VK_TAB) return settings::LoadRuntimeSettings().tab_accept_enabled;
  return (virtual_key >= 'A' && virtual_key <= 'Z') ||
         (virtual_key >= '0' && virtual_key <= '9') ||
         (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_DIVIDE) || virtual_key == VK_SPACE ||
         virtual_key == VK_BACK || virtual_key == VK_ESCAPE || virtual_key == VK_RETURN ||
         IsShift(virtual_key) || virtual_key == VK_CAPITAL ||
         virtual_key == VK_DELETE || virtual_key == VK_LEFT || virtual_key == VK_RIGHT ||
         virtual_key == VK_UP || virtual_key == VK_DOWN || virtual_key == VK_HOME || virtual_key == VK_END ||
         virtual_key == VK_PRIOR || virtual_key == VK_NEXT ||
         (virtual_key >= VK_OEM_1 && virtual_key <= VK_OEM_3) ||
         (virtual_key >= VK_OEM_4 && virtual_key <= VK_OEM_8);
}

}  // namespace zhixu::tsf
