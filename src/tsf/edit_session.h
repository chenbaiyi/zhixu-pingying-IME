#pragma once

#include <Windows.h>
#include <msctf.h>

#include "tsf/text_service.h"

#include <atomic>
#include <string>
#include <optional>

namespace zhixu::tsf {

class CommitEventSink {
 public:
  virtual ~CommitEventSink() = default;
  virtual void OnCommitApplied(const CommitOutcome& outcome) = 0;
  virtual void OnCommitAppliedInContext(const CommitOutcome& outcome, ITfContext*, TfEditCookie) { OnCommitApplied(outcome); }
  virtual void OnCommitRejected(const CommitOutcome& outcome, HRESULT error) = 0;
  virtual void RetainForEditSession() noexcept {}
  virtual void ReleaseForEditSession() noexcept {}
  virtual void OnCommittedRangeWritten(ITfContext*, ITfRange*, const std::wstring&) noexcept {}
  virtual void OnOwnTextWritten() noexcept {}
  virtual std::uint64_t CurrentEditGeneration() const noexcept { return 0; }
  virtual void OnOwnTextWrittenInContext(ITfContext*, std::uint64_t) noexcept { OnOwnTextWritten(); }
};

class CommitEditSession final : public ITfEditSession {
 public:
  CommitEditSession(ITfContext* context, TextService& text_service, CommitIdentity identity,
                    std::wstring text, CommitEventSink& sink, ITfComposition** composition = nullptr);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie edit_cookie) override;

 private:
  ~CommitEditSession();

  std::atomic<ULONG> references_{1};
  ITfContext* context_;
  TextService& text_service_;
  CommitIdentity identity_;
  std::wstring text_;
  CommitEventSink& sink_;
  ITfComposition** composition_;
};

class PreeditEditSession final : public ITfEditSession {
 public:
  PreeditEditSession(ITfContext* context, ITfComposition** composition, std::wstring text,
                     ITfCompositionSink* sink, CommitEventSink& edit_sink,
                     std::optional<std::size_t> cursor = std::nullopt, TextService* text_service = nullptr);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie edit_cookie) override;

 private:
  ~PreeditEditSession();

  std::atomic<ULONG> references_{1};
  ITfContext* context_;
  ITfComposition** composition_;
  std::wstring text_;
  ITfCompositionSink* sink_;
  CommitEventSink& edit_sink_;
  std::optional<std::size_t> cursor_;
  TextService* text_service_;
  std::uint64_t expected_context_version_;
};

[[nodiscard]] HRESULT RequestTextCommit(ITfContext* context, TfClientId client_id,
                                        TextService& text_service, CommitIdentity identity,
                                        std::wstring text, CommitEventSink& sink);
[[nodiscard]] HRESULT RequestTextCommit(ITfContext* context, TfClientId client_id,
                                        TextService& text_service, CommitIdentity identity,
                                        std::wstring text, CommitEventSink& sink,
                                        ITfComposition** composition);
[[nodiscard]] HRESULT RequestPreeditUpdate(ITfContext* context, TfClientId client_id,
                                           ITfComposition** composition, std::wstring text,
                                           ITfCompositionSink* sink, CommitEventSink& edit_sink,
                                           std::optional<std::size_t> cursor = std::nullopt,
                                           TextService* text_service = nullptr);
HRESULT HasSelectedText(ITfContext* context, TfClientId client_id, bool* selected);
HRESULT RequestCompositionEnd(ITfContext* context, TfClientId client_id,
                               ITfComposition** composition, ITfCompositionSink* sink,
                               CommitEventSink& edit_sink);
void ReleaseComposition(ITfComposition*& composition) noexcept;

}  // namespace zhixu::tsf
