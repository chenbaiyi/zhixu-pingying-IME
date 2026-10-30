#pragma once

#include "tsf/key_event_sink.h"
#include "tsf/edit_session.h"
#include "tsf/preedit_state.h"
#include "tsf/text_service.h"

#include <Windows.h>
#include <msctf.h>

#include <atomic>
#include <memory>

namespace zhixu::tsf {

struct NativeCaretSnapshot {
  HWND foreground = nullptr;
  HWND focus = nullptr;
  HWND caret_window = nullptr;
  RECT caret{};
  bool Matches(const NativeCaretSnapshot& other) const noexcept {
    return foreground == other.foreground && focus == other.focus &&
        caret_window == other.caret_window && EqualRect(&caret, &other.caret);
  }
};
using NativeCaretProbe = bool (*)(NativeCaretSnapshot&) noexcept;

class TsfTextService final : public ITfTextInputProcessorEx,
                             public ITfKeyEventSink,
                             public ITfThreadMgrEventSink,
                             public ITfTextEditSink,
                             public ITfCompositionSink,
                             public CommitEventSink {
 public:
  explicit TsfTextService(EngineActionTransport* transport = nullptr, NativeCaretProbe native_caret_probe = nullptr);
  explicit TsfTextService(std::unique_ptr<EngineActionTransport> transport);
  ~TsfTextService();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* thread_manager, TfClientId client_id) override;
  HRESULT STDMETHODCALLTYPE Deactivate() override;
  HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* thread_manager, TfClientId client_id,
                                       DWORD flags) override;

  HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override;
  HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context, WPARAM virtual_key,
                                          LPARAM key_data, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext* context, WPARAM virtual_key,
                                        LPARAM key_data, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM virtual_key,
                                      LPARAM key_data, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM virtual_key,
                                    LPARAM key_data, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext* context, REFGUID guid,
                                           BOOL* eaten) override;

  HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr* document_manager) override;
  HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr* document_manager) override;
  HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* focus,
                                       ITfDocumentMgr* previous_focus) override;
  HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext* context) override;
  HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override;

 HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* context, TfEditCookie read_cookie,
                                      ITfEditRecord* edit_record) override;
  HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie write_cookie,
                                                     ITfComposition* composition) override;

  void OnCommitApplied(const CommitOutcome& outcome) override;
  void OnCommitAppliedInContext(const CommitOutcome& outcome, ITfContext* context, TfEditCookie cookie) override;
  void OnCommitRejected(const CommitOutcome& outcome, HRESULT error) override;
  void RetainForEditSession() noexcept override { AddRef(); }
  void ReleaseForEditSession() noexcept override { Release(); }
  void OnCommittedRangeWritten(ITfContext*, ITfRange*, const std::wstring&) noexcept override;
  void OnOwnTextWritten() noexcept override { ClearCommittedRange(); own_edit_pending_ = true; ++own_text_write_serial_; }
  std::uint64_t CurrentEditGeneration() const noexcept override { return text_service_.context_version(); }
  void OnOwnTextWrittenInContext(ITfContext* context, std::uint64_t generation) noexcept override {
    ClearCommittedRange();
    ++own_text_write_serial_;
    // A detached old-context cleanup must not mask the next native edit.
    if (context == observed_context_ && generation == text_service_.context_version()) own_edit_pending_ = true;
  }

 private:
  [[nodiscard]] static bool WantsCompositionKey(WPARAM virtual_key, LPARAM key_data = 0) noexcept;
  HRESULT HandleKey(ITfContext* context, WPARAM virtual_key, LPARAM key_data, BOOL* eaten, bool down,
                    bool cancel_shift = false);
  [[nodiscard]] bool LeavesNativeKey(WPARAM key, LPARAM data) const noexcept;
  void ClearCommittedRange() noexcept;
  bool IsRepeatedCommittedEdit(ITfContext*, TfEditCookie, ITfEditRecord*) noexcept;
  bool IsTransitoryBufferCleanup(ITfContext*, TfEditCookie, ITfEditRecord*) noexcept;
  std::optional<NativeCaretSnapshot> ReadNativeCaret() const noexcept;
  void EndFocusedComposition();
  void NotifyInvalidated(InvalidationReason reason) noexcept;
  void UpdateCaret(ITfContext* context, TfEditCookie read_cookie) noexcept;
  HRESULT ObserveContext(ITfContext* context);
  void ReleaseObservedContext() noexcept;
  void ResetActivation() noexcept;
  bool CreateNotificationWindow();
  void PollRecommendation();
  HRESULT ApplyEngineAction(ITfContext* context, const protocol::v1::EngineAction& action);
  static LRESULT CALLBACK NotificationWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

  std::atomic<ULONG> references_{1};
  std::uint64_t own_text_write_serial_ = 0;
  TextService text_service_;
  std::unique_ptr<EngineActionTransport> owned_transport_;
  EngineActionTransport* transport_ = nullptr;
  ITfComposition* composition_ = nullptr;
  PreeditState preedit_state_;
  ITfThreadMgr* thread_manager_ = nullptr;
  ITfSource* thread_manager_source_ = nullptr;
  ITfKeystrokeMgr* keystroke_manager_ = nullptr;
  ITfContext* observed_context_ = nullptr;
  ITfSource* observed_context_source_ = nullptr;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  DWORD thread_manager_sink_cookie_ = TF_INVALID_COOKIE;
  DWORD text_edit_sink_cookie_ = TF_INVALID_COOKIE;
  bool key_event_sink_advised_ = false;
  bool recommendation_keys_preserved_[8]{};
  NativeCaretProbe native_caret_probe_ = nullptr;
  std::optional<NativeCaretSnapshot> cleanup_native_caret_;
  bool transitory_cleanup_pending_ = false;
  ITfRange* committed_range_ = nullptr;
  std::wstring committed_text_;
  std::uint64_t committed_version_ = 0;
  ULONGLONG committed_deadline_ = 0;
  bool own_edit_pending_ = false;
  bool shift_tap_pending_ = false;
  bool ascii_mode_ = false;
  bool full_shape_ = false;
  HWND notification_window_ = nullptr;
  HWND caret_foreground_window_ = nullptr;
  HINSTANCE notification_module_ = nullptr;
  KeyEventSink key_event_sink_;
};

}  // namespace zhixu::tsf
