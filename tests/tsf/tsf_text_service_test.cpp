#include <gtest/gtest.h>

#include <msctf.h>

#include "tsf/tsf_text_service.h"

namespace zhixu::tsf {
namespace {

class FocusTransport final : public EngineActionTransport {
 public:
  bool Connect(std::chrono::milliseconds) override { ++connections; return true; }
  void Disconnect() noexcept override { ++disconnections; }
  std::optional<protocol::v1::EngineAction> Dispatch(
      const KeyInput&, std::chrono::milliseconds) override { return std::nullopt; }
  unsigned int connections = 0;
  unsigned int disconnections = 0;
};

// Isolate the real TSF registration contract; text edits are not needed here.
class ContextWithEditSource : public ITfContext, public ITfSource {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfContext) *object = static_cast<ITfContext*>(this);
    else if (iid == IID_ITfSource) *object = static_cast<ITfSource*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown* sink, DWORD* cookie) override {
    if (iid != IID_ITfTextEditSink || cookie == nullptr) return E_INVALIDARG;
    if (edit_sink != nullptr) return E_UNEXPECTED;
    const auto result = sink->QueryInterface(iid, reinterpret_cast<void**>(&edit_sink));
    if (FAILED(result)) return result;
    ++subscriptions;
    *cookie = 17;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override {
    if (cookie != 17 || edit_sink == nullptr) return E_INVALIDARG;
    edit_sink->Release();
    edit_sink = nullptr;
    ++unsubscriptions;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId, ITfEditSession*, DWORD, HRESULT*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE InWriteSession(TfClientId, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION*, ULONG*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetStart(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetEnd(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetActiveView(ITfContextView**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumViews(IEnumTfContextViews**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetProperty(REFGUID, ITfProperty**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetAppProperty(REFGUID, ITfReadOnlyProperty**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TrackProperties(const GUID**, ULONG, const GUID**, ULONG, ITfReadOnlyProperty**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumProperties(IEnumTfProperties**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup**) override { return E_NOTIMPL; }
  ULONG references = 1;
  unsigned int subscriptions = 0;
  unsigned int unsubscriptions = 0;
  ITfTextEditSink* edit_sink = nullptr;
};

class TextRange final : public ITfRange {
 public:
  explicit TextRange(std::wstring& text, bool allocated = false)
      : text(text), end(static_cast<LONG>(text.size())), allocated(allocated) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfRange) return E_NOINTERFACE;
    *object = static_cast<ITfRange*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references;
    if (remaining == 0 && allocated) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE SetText(TfEditCookie, DWORD, const WCHAR* value, LONG count) override {
    text.replace(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start), value, count);
    end = start + count;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Collapse(TfEditCookie, TfAnchor anchor) override {
    if (anchor == TF_ANCHOR_END) start = end; else end = start;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(ITfRange** output) override {
    auto* copy = new TextRange(text, true); copy->start = start; copy->end = end; copy->transitory_range=transitory_range;
    *output = copy; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE IsEmpty(TfEditCookie, BOOL* empty) override { *empty = (transitory_range && text.empty()) || start == end; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetText(TfEditCookie, DWORD, WCHAR* output, ULONG capacity, ULONG* fetched) override {
    if(transitory_range && text.empty()){*fetched=0;return S_OK;}
    if(fail_queries || start<0 || end<start || end>static_cast<LONG>(text.size()))return E_FAIL;
    *fetched=(std::min)(capacity,static_cast<ULONG>(end-start));
    std::copy_n(text.data()+start,*fetched,output);return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFormattedText(TfEditCookie, IDataObject**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, LONG count, LONG* shifted, const TF_HALTCOND*) override {
    const LONG old = start;
    start = (std::max)(0L, (std::min)(end, start + count));
    *shifted = start - old;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie, LONG, LONG*, const TF_HALTCOND*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CompareStart(TfEditCookie, ITfRange* other, TfAnchor anchor, LONG* result) override {
    auto* range=static_cast<TextRange*>(other);if(fail_queries || range->fail_queries)return E_FAIL;
    if(&text!=&range->text)return E_INVALIDARG;
    *result=start-(anchor==TF_ANCHOR_START?range->start:range->end);return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CompareEnd(TfEditCookie, ITfRange* other, TfAnchor anchor, LONG* result) override {
    auto* range=static_cast<TextRange*>(other);if(fail_queries || range->fail_queries)return E_FAIL;
    if(&text!=&range->text)return E_INVALIDARG;
    *result=end-(anchor==TF_ANCHOR_START?range->start:range->end);return S_OK;
  }
  HRESULT STDMETHODCALLTYPE AdjustForInsert(TfEditCookie, ULONG, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetContext(ITfContext**) override { return E_NOTIMPL; }
  bool transitory_range=false;
  bool fail_queries=false;
  std::wstring& text;
  LONG start = 0;
  LONG end;
  bool allocated;
  ULONG references = 1;
};

class SelectionContext : public ContextWithEditSource {
 public:
  HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId, ITfEditSession* session, DWORD,
                                                HRESULT* result) override {
    *result = session->DoEditSession(7);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie, ULONG, ULONG count, TF_SELECTION* selection,
                                         ULONG* fetched) override {
    if (!selected_range || count != 1) return E_NOTIMPL;
    selection->range = selected_range;
    selection->style = {.ase = TF_AE_NONE, .fInterimChar = FALSE};
    selected_range->AddRef();
    *fetched = 1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie, ULONG count, const TF_SELECTION* selection) override {
    if (count != 1 || !selection || !selection->range) return E_INVALIDARG;
    auto* range = static_cast<TextRange*>(selection->range);
    selection_start = range->start; selection_end = range->end;
    return S_OK;
  }
  LONG selection_start = 0;
  LONG selection_end = 0;
  TextRange* selected_range = nullptr;
};

class TestComposition final : public ITfComposition {
 public:
  explicit TestComposition(TextRange& range) : range(range) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfComposition) return E_NOINTERFACE;
    *object = static_cast<ITfComposition*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE GetRange(ITfRange** output) override { *output = &range; range.AddRef(); return S_OK; }
  HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie, ITfRange*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EndComposition(TfEditCookie) override { ended = true; return S_OK; }
  TextRange& range;
  ULONG references = 1;
  bool ended = false;
};

class EditingTransport final : public EngineActionTransport {
 public:
  bool Connect(std::chrono::milliseconds) override { return available; }
  void Disconnect() noexcept override { ++disconnections; }
  std::optional<protocol::v1::EngineAction> Dispatch(
      const KeyInput& input, std::chrono::milliseconds) override { last_key = input; ++dispatches; return action; }
  void InvalidateSession(protocol::v1::InvalidationReason, std::uint64_t) noexcept override {
    ++invalidations;
  }
  void CommitRejected(std::uint64_t request, HRESULT error) noexcept override {
    rejected_request = request;
    rejected_error = error;
  }
  protocol::v1::EngineAction action;
  KeyInput last_key;
  bool available = true;
  unsigned int dispatches = 0;
  unsigned int disconnections = 0;
  unsigned int invalidations = 0;
  std::uint64_t rejected_request = 0;
  HRESULT rejected_error = S_OK;
};

TEST(TsfTextService, UnavailableEngineDoesNotClaimLettersDuringTestCallback) {
  EditingTransport transport;
  transport.available = false;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = TRUE;
  EXPECT_EQ(service->OnTestKeyDown(nullptr, 'N', 0, &eaten), S_OK);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(transport.dispatches, 0U);
  service->Release();
}

TEST(TsfTextService, DocumentFocusChangeReleasesTransport) {
  EditingTransport transport;
  auto* service = new TsfTextService(&transport);
  EXPECT_EQ(service->OnSetFocus(static_cast<ITfDocumentMgr*>(nullptr),
                               static_cast<ITfDocumentMgr*>(nullptr)), S_OK);
  EXPECT_EQ(transport.disconnections, 1U);
  EXPECT_EQ(transport.invalidations, 1U);
  service->Release();
}

TEST(TsfTextService, RejectedPreeditRequestReturnsTheOriginalKeyAndResetsEngine) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("n");
  ContextWithEditSource context;  // Refuses the edit-session request.
  auto* service = new TsfTextService(&transport);
  BOOL eaten = TRUE;
  EXPECT_EQ(service->OnKeyDown(&context, 'N', 0, &eaten), S_OK);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(transport.invalidations, 1U);
  EXPECT_EQ(service->Deactivate(), S_OK);
  service->Release();
}

TEST(TsfTextService, FailedPreeditExecutionReturnsTheOriginalKeyAndResetsEngine) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("n");
  SelectionContext context;  // Runs the session, but lacks ITfInsertAtSelection.
  auto* service = new TsfTextService(&transport);
  BOOL eaten = TRUE;
  EXPECT_EQ(service->OnKeyDown(&context, 'N', 0, &eaten), S_OK);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(transport.invalidations, 1U);
  EXPECT_EQ(service->Deactivate(), S_OK);
  service->Release();
}

TEST(TsfTextService, FailedCommitBeforeWritingReturnsKeyAndRejectsPendingCommit) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  auto* commit = transport.action.mutable_commit_request();
  commit->set_request_id(17);
  commit->set_text("你");
  SelectionContext context;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = TRUE;
  EXPECT_EQ(service->OnKeyDown(&context, 'N', 0, &eaten), S_OK);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(transport.rejected_request, 17U);
  EXPECT_EQ(transport.rejected_error, E_NOINTERFACE);
  EXPECT_EQ(transport.invalidations, 1U);
  EXPECT_EQ(service->Deactivate(), S_OK);
  service->Release();
}

class EditableContext : public SelectionContext,
                              public ITfInsertAtSelection, public ITfContextComposition {
 public:
  EditableContext() : range(document), composition(range) { selected_range = &range; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (iid == IID_ITfInsertAtSelection) *object = static_cast<ITfInsertAtSelection*>(this);
    else if (iid == IID_ITfContextComposition) *object = static_cast<ITfContextComposition*>(this);
    else return SelectionContext::QueryInterface(iid, object);
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return SelectionContext::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return SelectionContext::Release(); }
  HRESULT STDMETHODCALLTYPE InsertTextAtSelection(TfEditCookie, DWORD flags, const WCHAR* text,
                                                  LONG count, ITfRange** output) override {
    if (!(flags & TF_IAS_QUERYONLY)) range.SetText(7, 0, text, count);
    if (output) { *output = &range; range.AddRef(); }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(TfEditCookie, DWORD, IDataObject*, ITfRange**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE StartComposition(TfEditCookie, ITfRange*, ITfCompositionSink*, ITfComposition** output) override {
    *output = &composition; composition.AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EnumCompositions(IEnumITfCompositionView**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FindComposition(TfEditCookie, ITfRange*, IEnumITfCompositionView**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TakeOwnership(TfEditCookie, ITfCompositionView*, ITfCompositionSink*, ITfComposition**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie cookie, ULONG count, const TF_SELECTION* selection) override {
    return fail_caret ? E_FAIL : SelectionContext::SetSelection(cookie, count, selection);
  }
  std::wstring document;
  TextRange range;
  TestComposition composition;
  bool fail_caret = false;
};


// Stable TSF command identities for Alt+1/2/3.
constexpr GUID kRecommendationCommands[] = {
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x01}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x02}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x03}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x04}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x05}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x06}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x07}},
    {0x70cdbdc1, 0xbc2d, 0x4f02, {0x81, 0x38, 0x1d, 0x18, 0x60, 0x97, 0xaa, 0x08}}};

class ShortcutThreadManager final : public ITfThreadMgr, public ITfKeystrokeMgr, public ITfSource {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITfThreadMgr) *object = static_cast<ITfThreadMgr*>(this);
    else if (iid == IID_ITfKeystrokeMgr) *object = static_cast<ITfKeystrokeMgr*>(this);
    else if (iid == IID_ITfSource) *object = static_cast<ITfSource*>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE Activate(TfClientId*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Deactivate() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumDocumentMgrs(IEnumTfDocumentMgrs**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFocus(ITfDocumentMgr**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetFocus(ITfDocumentMgr*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE AssociateFocus(HWND, ITfDocumentMgr*, ITfDocumentMgr**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE IsThreadFocus(BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFunctionProvider(REFCLSID, ITfFunctionProvider**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFunctionProviders(IEnumTfFunctionProviders**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetGlobalCompartment(ITfCompartmentMgr**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown* sink, DWORD* cookie) override {
    if (iid != IID_ITfThreadMgrEventSink) return E_INVALIDARG;
    events = sink; events->AddRef(); *cookie = 1; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD) override { events->Release(); events = nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE AdviseKeyEventSink(TfClientId, ITfKeyEventSink* sink, BOOL) override {
    key_sink = sink; key_sink->AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UnadviseKeyEventSink(TfClientId) override {
    key_sink->Release(); key_sink = nullptr; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetForeground(CLSID*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TestKeyDown(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TestKeyUp(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE KeyDown(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE KeyUp(WPARAM, LPARAM, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPreservedKey(ITfContext*, const TF_PRESERVEDKEY*, GUID*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE IsPreservedKey(REFGUID, const TF_PRESERVEDKEY*, BOOL*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PreserveKey(TfClientId client, REFGUID guid, const TF_PRESERVEDKEY* chord,
                                       const WCHAR*, ULONG) override {
    EXPECT_EQ(client, 17U);
    EXPECT_EQ(chord->uModifiers, TF_MOD_ALT);
    const auto index = (chord->uVKey == VK_OEM_MINUS ? 6U : chord->uVKey == VK_OEM_PLUS ? 7U : chord->uVKey - '1');
    if (index >= 8 || guid != kRecommendationCommands[index]) return E_INVALIDARG;
    if (index == conflicting_index) return TF_E_ALREADY_EXISTS;
    registered[index] = true; ++preserves; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UnpreserveKey(REFGUID guid, const TF_PRESERVEDKEY* chord) override {
    const auto index = (chord->uVKey == VK_OEM_MINUS ? 6U : chord->uVKey == VK_OEM_PLUS ? 7U : chord->uVKey - '1');
    if (index >= 8 || guid != kRecommendationCommands[index] || !registered[index]) return E_INVALIDARG;
    registered[index] = false; ++unpreserves; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPreservedKeyDescription(REFGUID, const WCHAR*, ULONG) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPreservedKeyDescription(REFGUID, BSTR*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SimulatePreservedKey(ITfContext*, REFGUID, BOOL*) override { return E_NOTIMPL; }
  ULONG references = 1;
  IUnknown* events = nullptr;
  ITfKeyEventSink* key_sink = nullptr;
  bool registered[8]{};
  unsigned int preserves = 0, unpreserves = 0, conflicting_index = 99;
};

TEST(TsfTextService, RegistersRecommendationPreservedKeysUntilDeactivation) {
  ShortcutThreadManager manager;
  EditingTransport transport;
  auto* service = new TsfTextService(&transport);
  ASSERT_EQ(service->Activate(&manager, 17), S_OK);
  EXPECT_EQ(manager.preserves, 8U);
  for (bool registered : manager.registered) EXPECT_TRUE(registered);
  EXPECT_EQ(service->Deactivate(), S_OK);
  EXPECT_EQ(manager.unpreserves, 8U);
  for (bool registered : manager.registered) EXPECT_FALSE(registered);
  EXPECT_EQ(service->Activate(&manager, 17), S_OK);
  EXPECT_EQ(service->Deactivate(), S_OK);
  EXPECT_EQ(manager.preserves, 16U);
  EXPECT_EQ(manager.unpreserves, 16U);
  EXPECT_EQ(manager.references, 1U);
  service->Release();
}

TEST(TsfTextService, PreservedKeyConflictKeepsInputAvailableAndDoesNotUnregisterOtherOwner) {
  ShortcutThreadManager manager;
  manager.conflicting_index = 1;
  EditingTransport transport;
  auto* service = new TsfTextService(&transport);
  EXPECT_EQ(service->Activate(&manager, 17), S_OK);
  EXPECT_EQ(manager.preserves, 7U);
  EXPECT_FALSE(manager.registered[1]);
  EXPECT_EQ(service->Deactivate(), S_OK);
  EXPECT_EQ(manager.unpreserves, 7U);
  EXPECT_EQ(manager.references, 1U);
  service->Release();
}
TEST(TsfTextService, PreservedRecommendationKeysDispatchAltAndCommitText) {
  BYTE saved[256]{};
  ASSERT_TRUE(GetKeyboardState(saved));
  BYTE clear[256]{};
  ASSERT_TRUE(SetKeyboardState(clear));
  for (unsigned int i = 0; i < 6; ++i) {
    EditingTransport transport;
    transport.action.set_consumed(true);
    auto* request = transport.action.mutable_commit_request();
    request->set_request_id(17);
    request->set_expected_context_version(0);
    request->set_text("accepted");
    EditableContext context;
    auto* service = new TsfTextService(&transport);
    BOOL eaten = FALSE;
    EXPECT_EQ(service->OnPreservedKey(&context, kRecommendationCommands[i], &eaten), S_OK);
    EXPECT_TRUE(eaten);
    EXPECT_EQ(transport.dispatches, 1U);
    EXPECT_EQ(transport.last_key.virtual_key, '1' + i);
    EXPECT_EQ(transport.last_key.modifiers, 1U);
    EXPECT_EQ(context.document, L"accepted");
    service->Release();
  }
  ASSERT_TRUE(SetKeyboardState(saved));
}

TEST(TsfTextService, PreservedRecommendationPagingDispatchesAltWithoutEditing) {
  BYTE saved[256]{}; ASSERT_TRUE(GetKeyboardState(saved));
  BYTE clear[256]{}; ASSERT_TRUE(SetKeyboardState(clear));
  for (unsigned int i = 6; i < 8; ++i) {
    EditingTransport transport; transport.action.set_consumed(true);
    EditableContext context; auto* service = new TsfTextService(&transport);
    BOOL eaten = FALSE;
    EXPECT_EQ(service->OnPreservedKey(&context, kRecommendationCommands[i], &eaten), S_OK);
    EXPECT_TRUE(eaten); EXPECT_EQ(transport.dispatches, 1U);
    EXPECT_EQ(transport.last_key.virtual_key, i == 6 ? VK_OEM_MINUS : VK_OEM_PLUS);
    EXPECT_EQ(transport.last_key.modifiers, 1U);
    EXPECT_TRUE(context.document.empty()); service->Release();
  }
  ASSERT_TRUE(SetKeyboardState(saved));
}

TEST(TsfTextService, PreservedRecommendationWithoutResultLeavesApplicationShortcut) {
  EditingTransport transport;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = TRUE;
  EXPECT_EQ(service->OnPreservedKey(nullptr, kRecommendationCommands[0], &eaten), S_OK);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(transport.dispatches, 1U);
  EXPECT_EQ(service->OnPreservedKey(nullptr, GUID_NULL, &eaten), S_OK);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(transport.dispatches, 1U);
  service->Release();
}
TEST(TsfTextService, PreeditReplacesOnlySelectionAndUsesUtf16Cursor) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("你a");
  transport.action.mutable_composition()->set_cursor(3);  // UTF-8 bytes, UTF-16 position 1.
  EditableContext context;
  context.document = L"before SELECT after";
  context.range.start = 7;
  context.range.end = 13;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  ASSERT_EQ(service->OnKeyDown(&context, 'N', 0, &eaten), S_OK);
  EXPECT_TRUE(eaten);
  EXPECT_EQ(context.document, L"before 你a after");
  EXPECT_EQ(context.selection_start, 8);
  EXPECT_EQ(context.selection_end, 8);
  service->Deactivate();
  service->Release();
}

TEST(TsfTextService, OwnEditNotificationDoesNotAdvanceContextVersionAgain) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  EditableContext context;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(&context, 'N', 0, &eaten);
  service->OnEndEdit(&context, 7, nullptr);
  service->OnKeyDown(&context, 'I', 0, &eaten);
  EXPECT_EQ(transport.last_key.context_version, 0U);
  EXPECT_EQ(transport.invalidations, 0U);
  service->Deactivate();
  service->Release();
}

class EditRanges final : public IEnumTfRanges {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_IEnumTfRanges) return E_NOINTERFACE;
    *object = static_cast<IEnumTfRanges*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE Clone(IEnumTfRanges** output) override { *output = this; AddRef(); return S_OK; }
  HRESULT STDMETHODCALLTYPE Next(ULONG count, ITfRange** output, ULONG* fetched) override {
    *fetched = 0;
    if (count && range && !used) { *output = range; range->AddRef(); *fetched = 1; used = true; return S_OK; }
    if(count && second && !second_used){*output=second;second->AddRef();*fetched=1;second_used=true;return S_OK;}
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Reset() override { used = false; second_used=false; return S_OK; }
  HRESULT STDMETHODCALLTYPE Skip(ULONG) override { used = true; return S_OK; }
  ITfRange* second = nullptr;
  bool second_used=false;
  ITfRange* range = nullptr;
  bool used = false;
  ULONG references = 1;
};

class EditRecord final : public ITfEditRecord {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditRecord) return E_NOINTERFACE;
    *object = static_cast<ITfEditRecord*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE GetSelectionStatus(BOOL* changed) override { *changed = selection_changed; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetTextAndPropertyUpdates(DWORD flags, const GUID**, ULONG, IEnumTfRanges** output) override {
    ranges.range = (flags & TF_GTP_INCL_TEXT) ? changed_text : nullptr;
    ranges.second=second_text; ranges.Reset(); *output = &ranges; ranges.AddRef(); return S_OK;
  }
  BOOL selection_changed = FALSE;
  ITfRange* changed_text = nullptr;
  ITfRange* second_text = nullptr;
  EditRanges ranges;
  ULONG references = 1;
};

TEST(TsfTextService, PropertyOnlyEditNotificationKeepsActiveComposition) {
  EditableContext context;
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(&context, 'N', 0, &eaten);
  service->OnEndEdit(&context, 7, nullptr);  // First callback acknowledges the IME's own text write.
  const auto before = transport.invalidations;
  EditRecord property_only;
  service->OnEndEdit(&context, 7, &property_only);
  EXPECT_EQ(transport.invalidations, before);
  service->Deactivate();
  service->Release();
}

TEST(TsfTextService, ActualTextOrSelectionEditStillInvalidatesComposition) {
  for (const bool text_change : {false, true}) {
    EditableContext context;
    EditingTransport transport;
    transport.action.set_consumed(true);
    transport.action.mutable_composition()->set_preedit("ni");
    auto* service = new TsfTextService(&transport);
    BOOL eaten = FALSE;
    service->OnKeyDown(&context, 'N', 0, &eaten);
    service->OnEndEdit(&context, 7, nullptr);
    const auto before = transport.invalidations;
    EditRecord record;
    record.selection_changed = text_change ? FALSE : TRUE;
    record.changed_text = text_change ? &context.range : nullptr;
    service->OnEndEdit(&context, 7, &record);
    EXPECT_EQ(transport.invalidations, before + 1);
    service->Deactivate();
    service->Release();
  }
}

TEST(TsfTextService, BackspaceWithoutCompositionBelongsToTheApplication) {
  EditableContext context;
  context.document = L"你";
  context.range.start = 1;
  context.range.end = 1;  // Native caret after a committed character, no selection.
  EditingTransport transport;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = TRUE;
  EXPECT_EQ(service->OnTestKeyDown(&context, VK_BACK, 0, &eaten), S_OK);
  EXPECT_FALSE(eaten) << "IMM hosts rely on the test callback to pass Backspace to their editor";
  EXPECT_EQ(service->OnKeyDown(&context, VK_BACK, 0, &eaten), S_OK);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(transport.dispatches, 0U);
  service->Release();
}

TEST(TsfTextService, DelayedPreeditCannotWriteAfterDocumentFocusChange) {
  class DeferredContext final : public EditableContext {
   public:
    ~DeferredContext() { if (queued) queued->Release(); }
    HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId client, ITfEditSession* session,
                                                 DWORD flags, HRESULT* result) override {
      if (flags & TF_ES_SYNC) return EditableContext::RequestEditSession(client, session, flags, result);
      queued = session;
      queued->AddRef();
      *result = TF_S_ASYNC;
      return S_OK;
    }
    ITfEditSession* queued = nullptr;
  } context;
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(&context, 'N', 0, &eaten);
  ASSERT_NE(context.queued, nullptr);
  service->OnSetFocus(static_cast<ITfDocumentMgr*>(nullptr), static_cast<ITfDocumentMgr*>(nullptr));
  EXPECT_EQ(context.queued->DoEditSession(7), E_ABORT);
  EXPECT_TRUE(context.document.empty());
  context.queued->Release();
  context.queued = nullptr;
  service->Deactivate();
  service->Release();
}

TEST(TsfTextService, SuccessfulPreeditConsumesKeyAndWritesDocument) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("n");
  EditableContext context;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  EXPECT_EQ(service->OnKeyDown(&context, 'N', 0, &eaten), S_OK);
  EXPECT_TRUE(eaten);
  EXPECT_EQ(context.document, L"n");
  EXPECT_EQ(transport.invalidations, 0U);
  EXPECT_EQ(service->Deactivate(), S_OK);
  service->Release();
}

TEST(TsfTextService, FocusEndsOldCompositionWithoutErasingTextOrReusingItsRange) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  EditableContext old_context;
  EditableContext new_context;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(&old_context, 'N', 0, &eaten);
  service->OnSetFocus(static_cast<ITfDocumentMgr*>(nullptr), static_cast<ITfDocumentMgr*>(nullptr));
  EXPECT_EQ(old_context.document, L"ni");
  EXPECT_TRUE(old_context.composition.ended);
  transport.action.mutable_composition()->set_preedit("hao");
  service->OnKeyDown(&new_context, 'H', 0, &eaten);
  EXPECT_EQ(new_context.document, L"hao");
  EXPECT_EQ(old_context.document, L"ni");
  service->Deactivate();
  service->Release();
}

class QueuedEditableContext : public EditableContext {
 public:
  ~QueuedEditableContext() { for (auto* session : queued) session->Release(); }
  HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId client, ITfEditSession* session,
                                               DWORD flags, HRESULT* result) override {
    if (flags & TF_ES_SYNC) return EditableContext::RequestEditSession(client, session, flags, result);
    session->AddRef();
    queued.push_back(session);
    *result = TF_S_ASYNC;
    return S_OK;
  }
  std::vector<ITfEditSession*> queued;
};

TEST(TsfTextService, DetachedOldCleanupCannotHideAnExternalEditInNewContext) {
  QueuedEditableContext old_context;
  EditableContext new_context;
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(&old_context, 'N', 0, &eaten);
  ASSERT_EQ(old_context.queued.size(), 1U);
  ASSERT_EQ(old_context.queued[0]->DoEditSession(7), S_OK);
  service->OnEndEdit(&old_context, 7, nullptr);
  service->OnSetFocus(static_cast<ITfDocumentMgr*>(nullptr), static_cast<ITfDocumentMgr*>(nullptr));
  ASSERT_EQ(old_context.queued.size(), 2U);
  service->OnKeyDown(&new_context, 'N', 0, &eaten);
  service->OnEndEdit(&new_context, 7, nullptr);
  const auto before = transport.invalidations;
  ASSERT_EQ(old_context.queued[1]->DoEditSession(7), S_OK);
  service->OnEndEdit(&new_context, 7, nullptr);  // Now a real application edit.
  EXPECT_EQ(transport.invalidations, before + 1);
  for (auto* session : old_context.queued) session->Release();
  old_context.queued.clear();
  service->Deactivate();
  service->Release();
}

TEST(TsfTextService, DeactivationInvalidatesDelayedPreedit) {
  QueuedEditableContext context;
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(&context, 'N', 0, &eaten);
  ASSERT_EQ(context.queued.size(), 1U);
  service->Deactivate();
  EXPECT_EQ(context.queued[0]->DoEditSession(7), E_ABORT);
  EXPECT_TRUE(context.document.empty());
  context.queued[0]->Release();
  context.queued.clear();
  service->Release();
}

TEST(TsfTextService, OldCleanupCannotMaskExternalEditAfterReturningToSameContext) {
  QueuedEditableContext context;
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(&context, 'N', 0, &eaten);
  ASSERT_EQ(context.queued[0]->DoEditSession(7), S_OK);
  service->OnEndEdit(&context, 7, nullptr);
  service->OnSetFocus(static_cast<ITfDocumentMgr*>(nullptr), static_cast<ITfDocumentMgr*>(nullptr));
  service->OnPushContext(&context);  // Return to the same COM context, but a new generation.
  const auto before = transport.invalidations;
  ASSERT_EQ(context.queued[1]->DoEditSession(7), S_OK);
  service->OnEndEdit(&context, 7, nullptr);
  EXPECT_EQ(transport.invalidations, before + 1);
  for (auto* session : context.queued) session->Release();
  context.queued.clear();
  service->Deactivate();
  service->Release();
}

TEST(TsfTextService, DeactivationInvalidatesDelayedCommit) {
  QueuedEditableContext context;
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_commit_request()->set_request_id(19);
  transport.action.mutable_commit_request()->set_text("你");
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(&context, 'N', 0, &eaten);
  ASSERT_EQ(context.queued.size(), 1U);
  service->Deactivate();
  EXPECT_EQ(context.queued[0]->DoEditSession(7), E_ABORT);
  EXPECT_TRUE(context.document.empty());
  EXPECT_EQ(transport.rejected_request, 19U);
  context.queued[0]->Release();
  context.queued.clear();
  service->Release();
}

TEST(TsfTextService, FailedFocusCleanupStillDetachesOldRange) {
  class RefusesCleanup final : public EditableContext {
   public:
    HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId client, ITfEditSession* session,
                                                 DWORD flags, HRESULT* result) override {
      if (refuse && !(flags & TF_ES_SYNC)) return E_FAIL;
      return EditableContext::RequestEditSession(client, session, flags, result);
    }
    bool refuse = false;
  } old_context;
  EditableContext new_context;
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(&old_context, 'N', 0, &eaten);
  old_context.refuse = true;
  service->OnSetFocus(static_cast<ITfDocumentMgr*>(nullptr), static_cast<ITfDocumentMgr*>(nullptr));
  transport.action.mutable_composition()->set_preedit("hao");
  service->OnKeyDown(&new_context, 'H', 0, &eaten);
  EXPECT_EQ(old_context.document, L"ni");
  EXPECT_EQ(new_context.document, L"hao");
  service->Deactivate();
  service->Release();
}

TEST(TsfTextService, AppShortcutsTabAndShiftSelectionAreNeverDispatched) {
  struct Restore {
    BYTE original[256]{};
    Restore() { GetKeyboardState(original); }
    ~Restore() { SetKeyboardState(original); }
  } restore;
  EditingTransport transport;
  auto* service = new TsfTextService(&transport);
  BYTE keyboard[256]{};
  for (const auto modifiers : {VK_CONTROL, VK_MENU, VK_LWIN}) {
    ZeroMemory(keyboard, sizeof(keyboard));
    keyboard[modifiers] = 0x80;
    ASSERT_TRUE(SetKeyboardState(keyboard));
    for (const auto key : {'A', 'C', 'V', 'X', 'Z', 'F'}) {
      BOOL eaten = TRUE;
      service->OnTestKeyDown(nullptr, key, 0, &eaten);
      EXPECT_FALSE(eaten);
      service->OnKeyDown(nullptr, key, 0, &eaten);
      EXPECT_FALSE(eaten);
    }
  }
  ZeroMemory(keyboard, sizeof(keyboard));
  keyboard[VK_SHIFT] = 0x80;
  ASSERT_TRUE(SetKeyboardState(keyboard));
  for (const auto key : {VK_LEFT, VK_RIGHT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_TAB}) {
    BOOL eaten = TRUE;
    service->OnTestKeyDown(nullptr, key, 0, &eaten);
    EXPECT_FALSE(eaten);
    service->OnKeyDown(nullptr, key, 0, &eaten);
    EXPECT_FALSE(eaten);
  }
  EXPECT_EQ(transport.dispatches, 0U);
  service->Release();
}

TEST(TsfTextService, ShiftTapForwardsReleaseButShiftShortcutCannotToggleMode) {
  EditingTransport transport;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(nullptr, VK_SHIFT, 0, &eaten);
  service->OnKeyUp(nullptr, VK_SHIFT, 0, &eaten);
  EXPECT_EQ(transport.dispatches, 2U);
  EXPECT_FALSE(transport.last_key.key_down);
  service->OnKeyDown(nullptr, VK_SHIFT, 0, &eaten);
  service->OnTestKeyDown(nullptr, VK_F1, 0, &eaten);  // Application-owned key, no OnKeyDown callback.
  service->OnKeyUp(nullptr, VK_SHIFT, 0, &eaten);
  EXPECT_EQ(transport.dispatches, 4U);
  EXPECT_EQ(transport.last_key.modifiers & 6U, 6U);  // Cancel Rime's tap tracking, never toggle.
  service->Release();
}

TEST(TsfTextService, CaretFailureAfterTextWriteDoesNotReplayTheOriginalKey) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("n");
  EditableContext context;
  context.fail_caret = true;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  EXPECT_EQ(service->OnKeyDown(&context, 'N', 0, &eaten), S_OK);
  EXPECT_TRUE(eaten);  // Text was already written; releasing N would duplicate it.
  EXPECT_EQ(context.document, L"n");
  EXPECT_EQ(service->Deactivate(), S_OK);
  service->Release();
}

TEST(CommitEditSession, PlacesCaretAfterCommittedCompositionText) {
  SelectionContext context;
  TextService text_service;
  std::wstring document = L"ni";
  TextRange range(document);
  TestComposition composition(range);
  ITfComposition* active_composition = &composition;
  struct Sink final : CommitEventSink {
    void OnCommitApplied(const CommitOutcome&) override { applied = true; }
    void OnCommitRejected(const CommitOutcome&, HRESULT) override {}
    bool applied = false;
  } sink;
  auto* session = new CommitEditSession(&context, text_service,
      {.request_id = 1, .expected_context_version = 0}, L"\x4f60", sink, &active_composition);
  EXPECT_EQ(session->DoEditSession(7), S_OK);
  EXPECT_EQ(document, L"\x4f60");
  EXPECT_TRUE(composition.ended);
  EXPECT_TRUE(sink.applied);
  EXPECT_EQ(context.selection_start, 1);
  EXPECT_EQ(context.selection_end, 1);
  session->Release();
}

TEST(TsfTextService, LeavesControlSelectAllToApplication) {
  struct KeyboardRestore {
    BYTE saved[256]{};
    bool valid = GetKeyboardState(saved) != FALSE;
    ~KeyboardRestore() { if (valid) SetKeyboardState(saved); }
  } keyboard;
  ASSERT_TRUE(keyboard.valid);
  BYTE state[256]{};
  state[VK_CONTROL] = 0x80;
  ASSERT_TRUE(SetKeyboardState(state));
  auto* service = new TsfTextService(static_cast<EngineActionTransport*>(nullptr));
  BOOL eaten = TRUE;
  EXPECT_EQ(service->OnTestKeyDown(nullptr, 'A', 0, &eaten), S_OK);
  EXPECT_FALSE(eaten);
  service->Release();
}

TEST(TsfTextService, LeavesSelectedTextBackspaceAndDeleteToApplication) {
  SelectionContext context;
  std::wstring document = L"ni";
  TextRange range(document);
  context.selected_range = &range;  // Whole preedit is selected, not an insertion caret.
  auto* service = new TsfTextService(static_cast<EngineActionTransport*>(nullptr));
  for (const auto key : {VK_BACK, VK_DELETE}) {
    BOOL eaten = TRUE;
    EXPECT_EQ(service->OnTestKeyDown(&context, key, 0, &eaten), S_OK);
    EXPECT_FALSE(eaten);
  }
  EXPECT_EQ(document, L"ni");  // Testing a key must not modify the document.
  service->Release();
}

TEST(TsfTextService, ObservesExistingKeyContextWithoutPushNotification) {
  FocusTransport transport;
  ContextWithEditSource context;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = TRUE;
  EXPECT_EQ(service->OnKeyDown(&context, 'N', 0, &eaten), S_OK);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(context.subscriptions, 1U);
  EXPECT_EQ(service->OnKeyDown(&context, 'I', 0, &eaten), S_OK);
  EXPECT_EQ(context.subscriptions, 1U);  // Do not advise again for each key.
  EXPECT_EQ(service->Deactivate(), S_OK);
  EXPECT_EQ(context.unsubscriptions, 1U);
  EXPECT_EQ(context.references, 1U);
  service->Release();
}

TEST(TsfTextService, ForegroundSinkDoesNotEagerlyConnectAndBackgroundFocusReleasesIt) {
  FocusTransport transport;
  auto* service = new TsfTextService(&transport);
  EXPECT_EQ(service->OnSetFocus(TRUE), S_OK);
  EXPECT_EQ(transport.connections, 0U);
  EXPECT_EQ(service->OnSetFocus(FALSE), S_OK);
  EXPECT_EQ(transport.disconnections, 1U);
  service->Release();
}

TEST(TsfTextService, DeactivationReleasesForegroundConnection) {
  FocusTransport transport;
  auto* service = new TsfTextService(&transport);
  EXPECT_EQ(service->OnSetFocus(TRUE), S_OK);
  EXPECT_EQ(service->Deactivate(), S_OK);
  EXPECT_EQ(transport.disconnections, 1U);
  service->Release();
}

TEST(TsfTextService, ExposesRequiredTsfInterfaces) {
  auto* service = new TsfTextService(static_cast<EngineActionTransport*>(nullptr));

  ITfTextInputProcessorEx* processor = nullptr;
  ITfKeyEventSink* key_sink = nullptr;
  ITfThreadMgrEventSink* thread_sink = nullptr;
  ITfTextEditSink* edit_sink = nullptr;

  EXPECT_EQ(service->QueryInterface(IID_ITfTextInputProcessorEx,
                                    reinterpret_cast<void**>(&processor)), S_OK);
  EXPECT_EQ(service->QueryInterface(IID_ITfKeyEventSink,
                                    reinterpret_cast<void**>(&key_sink)), S_OK);
  EXPECT_EQ(service->QueryInterface(IID_ITfThreadMgrEventSink,
                                    reinterpret_cast<void**>(&thread_sink)), S_OK);
  EXPECT_EQ(service->QueryInterface(IID_ITfTextEditSink,
                                    reinterpret_cast<void**>(&edit_sink)), S_OK);

  processor->Release();
  key_sink->Release();
  thread_sink->Release();
  edit_sink->Release();
  service->Release();
}

TEST(TsfTextService, CompositionSinkSharesServiceIdentityAndLifetime) {
  auto* service = new TsfTextService(static_cast<EngineActionTransport*>(nullptr));
  ITfCompositionSink* sink = nullptr;
  const HRESULT result = service->QueryInterface(IID_ITfCompositionSink,
      reinterpret_cast<void**>(&sink));
  EXPECT_EQ(result, S_OK);
  if (sink != nullptr) {
    IUnknown* identity = nullptr;
    EXPECT_EQ(sink->QueryInterface(IID_IUnknown,
        reinterpret_cast<void**>(&identity)), S_OK);
    EXPECT_EQ(identity, static_cast<IUnknown*>(static_cast<ITfTextInputProcessorEx*>(service)));
    if (identity != nullptr) identity->Release();
    // The returned sink must keep the service alive after the caller lets go.
    EXPECT_EQ(service->Release(), 1U);
    EXPECT_EQ(sink->OnCompositionTerminated(0, nullptr), S_OK);
    EXPECT_EQ(sink->Release(), 0U);
  } else {
    service->Release();
  }
}

TEST(TsfTextService, MissingInputHostLeavesKeyUneatenWithoutBlocking) {
  auto* service = new TsfTextService(static_cast<EngineActionTransport*>(nullptr));
  BOOL eaten = TRUE;

  EXPECT_EQ(service->OnKeyDown(nullptr, 'A', 0, &eaten), S_OK);
  EXPECT_FALSE(eaten);

  service->Release();
}

TEST(TsfTextService, TestKeyDownClaimsCompositionKeysOnly) {
  FocusTransport transport;
  auto* service = new TsfTextService(&transport);
  BOOL letter_eaten = FALSE;
  BOOL tab_eaten = FALSE;
  BOOL function_eaten = TRUE;

  EXPECT_EQ(service->OnTestKeyDown(nullptr, 'A', 0, &letter_eaten), S_OK);
  EXPECT_TRUE(letter_eaten);
  EXPECT_EQ(service->OnTestKeyDown(nullptr, VK_TAB, 0, &tab_eaten), S_OK);
  EXPECT_FALSE(tab_eaten);  // Application focus/indent key by default.
  EXPECT_EQ(service->OnTestKeyDown(nullptr, VK_F1, 0, &function_eaten), S_OK);
  EXPECT_FALSE(function_eaten);

  service->Release();
}

TEST(TsfTextService, ForwardsFullPinyinPagingAndSeparatorKeysToInputHost) {
  struct PagingTransport final : EngineActionTransport {
    std::optional<protocol::v1::EngineAction> Dispatch(
        const KeyInput& input, std::chrono::milliseconds) override {
      last_key = input.virtual_key;
      protocol::v1::EngineAction action;
      action.set_consumed(true);
      return action;
    }
    WPARAM last_key = 0;
  } transport;
  auto* service = new TsfTextService(&transport);
  for (const auto key : {VK_PRIOR, VK_NEXT, VK_OEM_MINUS, VK_OEM_PLUS, VK_OEM_7}) {
    SCOPED_TRACE(key);
    BOOL eaten = FALSE;
    EXPECT_EQ(service->OnTestKeyDown(nullptr, key, 0, &eaten), S_OK);
    EXPECT_TRUE(eaten);
    eaten = FALSE;
    EXPECT_EQ(service->OnKeyDown(nullptr, key, 0, &eaten), S_OK);
    EXPECT_TRUE(eaten);
    EXPECT_EQ(transport.last_key, static_cast<WPARAM>(key));
  }
  service->Release();
}

TEST(TsfTextService, RejectsActivationWithoutThreadManager) {
  auto* service = new TsfTextService(static_cast<EngineActionTransport*>(nullptr));

  EXPECT_EQ(service->Activate(nullptr, TF_CLIENTID_NULL), E_INVALIDARG);
  EXPECT_EQ(service->Deactivate(), S_OK);

  service->Release();
}

}  // namespace
}  // namespace zhixu::tsf

namespace zhixu::tsf { namespace {
class OrderedCommitTransport : public EngineActionTransport {
 public:
  protocol::v1::EngineAction action;
  std::vector<std::pair<char, std::uint64_t>> events;
  std::optional<protocol::v1::EngineAction> Dispatch(const KeyInput&, std::chrono::milliseconds) override { return action; }
  void CommitApplied(std::uint64_t, std::uint64_t version) noexcept override { events.emplace_back('C', version); }
  void UpdateCaret(const protocol::v1::CaretUpdate&, std::uint64_t version) noexcept override { events.emplace_back('P', version); }
};
class StableCaretView final : public ITfContextView {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown&&iid!=IID_ITfContextView)return E_NOINTERFACE;*out=this;AddRef();return S_OK; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetRangeFromPoint(TfEditCookie,const POINT*,DWORD,ITfRange**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTextExt(TfEditCookie,ITfRange*,RECT* rect,BOOL* clipped) override { *rect={10,10,10,30};*clipped=FALSE;return S_OK; }
  HRESULT STDMETHODCALLTYPE GetScreenExt(RECT* rect) override { *rect={0,0,800,600};return S_OK; }
  HRESULT STDMETHODCALLTYPE GetWnd(HWND* out) override { *out=GetDesktopWindow();return S_OK; }
  ULONG refs=1;
};
class EarlyCaretContext : public EditableContext {
 public:
  HRESULT STDMETHODCALLTYPE GetActiveView(ITfContextView** out) override { *out=&view;view.AddRef();return S_OK; }
  HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie,ULONG,ULONG count,TF_SELECTION* selection,ULONG* fetched) override {
   if(count!=1)return E_INVALIDARG;range.start=selection_start;range.end=selection_end;selection->range=&range;selection->style={TF_AE_NONE,FALSE};range.AddRef();*fetched=1;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie cookie,ULONG count,const TF_SELECTION* selection) override {
   auto hr=EditableContext::SetSelection(cookie,count,selection);
   if(SUCCEEDED(hr)&&early&&service)service->OnEndEdit(this,cookie,nullptr);
   return hr;
  }
  StableCaretView view;
  TsfTextService* service=nullptr;
  bool early=true;
};
TEST(TsfTextService, CommitPublishesCurrentCaretAfterEarlyNativeEditNotification) {
  EarlyCaretContext context;
  OrderedCommitTransport transport;
  transport.action.set_consumed(true);
  auto* request=transport.action.mutable_commit_request();
  request->set_request_id(100);request->set_expected_context_version(0);request->set_text("君不见");
  auto* service=new TsfTextService(&transport);context.service=service;
  BOOL eaten=FALSE;
  ASSERT_EQ(service->OnKeyDown(&context,'N',0,&eaten),S_OK);
  EXPECT_TRUE(eaten);EXPECT_EQ(context.document,L"君不见");
  ASSERT_GE(transport.events.size(),2U);
  EXPECT_EQ(transport.events.back(),(std::pair<char,std::uint64_t>{'P',1}));
  const auto commit=std::find(transport.events.begin(),transport.events.end(),std::pair<char,std::uint64_t>{'C',1});
  ASSERT_NE(commit,transport.events.end());
  EXPECT_NE(std::find(commit,transport.events.end(),std::pair<char,std::uint64_t>{'P',1}),transport.events.end());
  service->Deactivate();service->Release();
}
TEST(TsfTextService, CommitPublishesCaretEvenBeforeDelayedNativeEditNotification) {
  EarlyCaretContext context;context.early=false;
  OrderedCommitTransport transport;transport.action.set_consumed(true);
  auto* request=transport.action.mutable_commit_request();request->set_request_id(101);request->set_text("君不见");
  auto* service=new TsfTextService(&transport);context.service=service;BOOL eaten=FALSE;
  ASSERT_EQ(service->OnKeyDown(&context,'N',0,&eaten),S_OK);
  ASSERT_GE(transport.events.size(),2U);
  EXPECT_EQ(transport.events.front(),(std::pair<char,std::uint64_t>{'C',1}));
  EXPECT_EQ(transport.events.back(),(std::pair<char,std::uint64_t>{'P',1}));
  service->Deactivate();service->Release();
}
} }

namespace zhixu::tsf {
namespace {
TEST(TsfTextService, NormalAltRecommendationKeysReachTransport) {
  BYTE saved[256]{}; ASSERT_TRUE(GetKeyboardState(saved));
  BYTE clear[256]{}; ASSERT_TRUE(SetKeyboardState(clear));
  for (const auto key : std::initializer_list<UINT>{'4', '5', '6', VK_OEM_MINUS, VK_OEM_PLUS}) {
    EditingTransport transport; transport.action.set_consumed(true);
    auto* service = new TsfTextService(&transport); BOOL eaten = FALSE;
    EXPECT_EQ(service->OnTestKeyDown(nullptr, key, 1ULL << 29, &eaten), S_OK);
    EXPECT_TRUE(eaten);
    EXPECT_EQ(service->OnKeyDown(nullptr, key, 1ULL << 29, &eaten), S_OK);
    EXPECT_TRUE(eaten); EXPECT_EQ(transport.dispatches, 1U);
    EXPECT_EQ(transport.last_key.virtual_key, key);
    EXPECT_EQ(transport.last_key.modifiers, 1U); service->Release();
  }
  ASSERT_TRUE(SetKeyboardState(saved));
}
}
}

namespace zhixu::tsf { namespace {
TEST(TsfTextService, AsciiModeDoesNotClaimNativePrintableKeys) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_ascii_mode(true);
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(nullptr, VK_SHIFT, 0, &eaten);
  service->OnKeyUp(nullptr, VK_SHIFT, 0, &eaten);
  for (auto key : {int('A'), int('Z'), int('0'), VK_SPACE, VK_OEM_PERIOD, VK_NUMPAD0, VK_NUMPAD9, VK_DECIMAL}) {
    eaten = TRUE;
    EXPECT_EQ(service->OnTestKeyDown(nullptr, key, 0, &eaten), S_OK);
    EXPECT_FALSE(eaten) << key;
  }
  transport.action.mutable_composition()->set_ascii_mode(false);
  service->OnKeyDown(nullptr, VK_SHIFT, 0, &eaten);
  service->OnKeyUp(nullptr, VK_SHIFT, 0, &eaten);
  service->OnTestKeyDown(nullptr, 'N', 0, &eaten);
  EXPECT_TRUE(eaten);
  service->Release();
}

TEST(TsfTextService, NativeDigitsPassThroughUnlessCompositionIsActive) {
  EditingTransport transport;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = TRUE;
  for (auto key : {int('0'), int('1'), int('9'), VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD9}) {
    service->OnTestKeyDown(nullptr, key, 0, &eaten);
    EXPECT_FALSE(eaten) << key;
  }
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  EditableContext context;
  service->OnKeyDown(&context, 'N', 0, &eaten);
  service->OnTestKeyDown(&context, VK_NUMPAD1, 0, &eaten);
  EXPECT_TRUE(eaten);
  service->Deactivate();
  service->Release();
}
} }

namespace zhixu::tsf { namespace {
TEST(TsfTextService, ConfirmedCommitSurvivesPropertyThenOwnTextNotification){
 class PropertyFirstContext final: public EarlyCaretContext {
  public:
   HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie cookie,ULONG count,const TF_SELECTION* selection) override {
    const auto result=EditableContext::SetSelection(cookie,count,selection);
    if(SUCCEEDED(result) && service){EditRecord property_only;service->OnEndEdit(this,cookie,&property_only);}
    return result;
   }
 } context;
 class RecordingCommitTransport final: public OrderedCommitTransport {
  public:
   void InvalidateSession(protocol::v1::InvalidationReason,std::uint64_t) noexcept override {++invalidations;}
   unsigned invalidations=0;
 } transport;
 transport.action.set_consumed(true);auto* commit=transport.action.mutable_commit_request();commit->set_request_id(501);commit->set_text("君不见");
 auto* service=new TsfTextService(&transport);context.service=service;BOOL eaten=FALSE;
 ASSERT_EQ(service->OnKeyDown(&context,'N',0,&eaten),S_OK);ASSERT_TRUE(eaten);ASSERT_EQ(context.document,L"君不见");
 ASSERT_NE(std::find(transport.events.begin(),transport.events.end(),std::pair<char,std::uint64_t>{'C',1}),transport.events.end());
 const auto before=transport.invalidations;EditRecord own_text;own_text.changed_text=&context.range;
 service->OnEndEdit(&context,7,&own_text);
 EXPECT_EQ(transport.invalidations,before);EXPECT_EQ(transport.events.back(),(std::pair<char,std::uint64_t>{'P',1}));
 EditRecord external;external.selection_changed=TRUE;context.selection_start=context.selection_end=0;service->OnEndEdit(&context,7,&external);EXPECT_EQ(transport.invalidations,before+1);
 service->Deactivate();service->Release();
}
TEST(TsfTextService, PropertyCallbackBeforeOwnTextDoesNotConsumeOwnership){
 EditableContext context;EditingTransport transport;transport.action.set_consumed(true);transport.action.mutable_composition()->set_preedit("ni");
 auto* service=new TsfTextService(&transport);BOOL eaten=FALSE;ASSERT_EQ(service->OnKeyDown(&context,'N',0,&eaten),S_OK);ASSERT_TRUE(eaten);
 const auto before=transport.invalidations;EditRecord property_only;service->OnEndEdit(&context,7,&property_only);
 EditRecord own_text;own_text.changed_text=&context.range;service->OnEndEdit(&context,7,&own_text);
 EXPECT_EQ(transport.invalidations,before)<<"An attribute-only callback must not consume ownership of the actual text write";
 // The next genuine selection change must still invalidate.
 EditRecord external;external.selection_changed=TRUE;service->OnEndEdit(&context,7,&external);
 EXPECT_EQ(transport.invalidations,before+1);
 service->Deactivate();service->Release();
}
} }

namespace zhixu::tsf { namespace {
class OwnedEditContext final : public EarlyCaretContext {
 public:
  TextRange selection{document};
  HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie,ULONG,ULONG count,TF_SELECTION* output,ULONG* fetched) override {
   if(count!=1)return E_INVALIDARG;selection.start=selection_start;selection.end=selection_end;
   output->range=&selection;output->style={TF_AE_NONE,FALSE};selection.AddRef();*fetched=1;return S_OK;
  }
};
TEST(TsfTextService, RepeatedCommittedTextRecordPreservesPredictionButExternalEditsCancel) {
 for(bool composition_path:{false,true}) for(int external=0;external<11;++external){
  OwnedEditContext context;context.early=false;
  EditingTransport transport;transport.action.set_consumed(true);
  auto* service=new TsfTextService(&transport);context.service=service;BOOL eaten=FALSE;
  if(composition_path){transport.action.mutable_composition()->set_preedit("da");ASSERT_EQ(service->OnKeyDown(&context,'D',0,&eaten),S_OK);service->OnEndEdit(&context,7,nullptr);transport.action.clear_composition();}
  auto* commit=transport.action.mutable_commit_request();commit->set_request_id(900);commit->set_text("大兴");
  ASSERT_EQ(service->OnKeyDown(&context,'N',0,&eaten),S_OK);ASSERT_TRUE(eaten);ASSERT_EQ(context.document,L"大兴");
  const auto before=transport.invalidations;
  TextRange owned(context.document);owned.start=0;owned.end=2;EditRecord record;record.changed_text=&owned;record.selection_changed=TRUE;
  service->OnEndEdit(&context,7,&record);record.selection_changed=FALSE;
  service->OnEndEdit(&context,7,&record);
  EXPECT_EQ(transport.invalidations,before)<<"repeat text callback after successful commit; composition="<<composition_path;
  // A genuine edit, moved caret, unknown record or failed query must invalidate.
  if(external==0)context.document[0]=L'小';
  if(external==1)context.document.clear();
  if(external==2)context.selection_start=context.selection_end=0;
  if(external==3){record.changed_text=nullptr;record.selection_changed=TRUE;context.selection_start=0;context.selection_end=2;}
  if(external==4){context.document+=L"外";owned.end=3;}
  if(external==5){owned.start=owned.end=2;}
  if(external==6)owned.fail_queries=true;
  if(external==8){owned.start=owned.end=0;}
  if(external==9){record.changed_text=nullptr;record.selection_changed=TRUE;context.selection_start=context.selection_end=2;service->OnEndEdit(&context,7,&record);EXPECT_EQ(transport.invalidations,before);Sleep(550);}
  TextRange outside(context.document);
  if(external==10){context.document+=L"外";outside.start=2;outside.end=3;record.second_text=&outside;}
  service->OnEndEdit(&context,7,external==7?nullptr:&record);
  EXPECT_EQ(transport.invalidations,before+1)<<"external case "<<external;
  service->Deactivate();service->Release();
 }
}
} }

namespace zhixu::tsf { namespace {
NativeCaretSnapshot test_native_caret{reinterpret_cast<HWND>(1),reinterpret_cast<HWND>(2),reinterpret_cast<HWND>(2),{0,0,1,20}};
bool test_native_available=true;
bool TestNativeCaret(NativeCaretSnapshot& output) noexcept {output=test_native_caret;return test_native_available;}
class TransitoryContext final : public EarlyCaretContext {
 public:
  TransitoryContext(){early=false;range.transitory_range=true;}
  TextRange selection{document};
  HRESULT STDMETHODCALLTYPE GetStatus(TF_STATUS* status) override {
   if(fail_status)return E_FAIL;*status={0,static_cast<DWORD>(transitory?TF_SS_TRANSITORY:0)};return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStart(TfEditCookie,ITfRange** output) override {
   if(fail_boundaries)return E_FAIL;auto* p=new TextRange(document,true);p->start=p->end=0;*output=p;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetEnd(TfEditCookie,ITfRange** output) override {
   if(fail_boundaries)return E_FAIL;auto* p=new TextRange(document,true);p->start=p->end=nonempty_context?1:static_cast<LONG>(document.size());*output=p;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie,ULONG,ULONG count,TF_SELECTION* output,ULONG* fetched) override {
   if(fail_selection || count!=1)return E_FAIL;
   selection.start=selection_start;selection.end=selection_end;output->range=&selection;output->style={TF_AE_NONE,FALSE};selection.AddRef();*fetched=1;return S_OK;
  }
  bool transitory=true,fail_status=false,fail_boundaries=false,fail_selection=false,nonempty_context=false;
};
TEST(TsfTextService, TransitoryBufferCleanupPreservesOneConfirmedRecommendation) {
 TransitoryContext context;EditingTransport transport;transport.action.set_consumed(true);
 auto* request=transport.action.mutable_commit_request();request->set_request_id(1300);request->set_text("君不见");
 auto* service=new TsfTextService(&transport,TestNativeCaret);context.service=service;BOOL eaten=FALSE;
 ASSERT_EQ(service->OnKeyDown(&context,'N',0,&eaten),S_OK);ASSERT_TRUE(eaten);ASSERT_EQ(context.document,L"君不见");
 TextRange written(context.document);EditRecord own;own.changed_text=&written;
 service->OnEndEdit(&context,7,&own);const auto before=transport.invalidations;
 // The native editor retains the committed result; only its TSF composition buffer clears.
 context.document.clear();context.range.start=context.range.end=0;context.selection_start=context.selection_end=0;
 TextRange cleared(context.document);EditRecord cleanup;cleanup.changed_text=&cleared;
 service->OnEndEdit(&context,7,&cleanup);
 EXPECT_EQ(transport.invalidations,before)<<"One verified transitory cleanup must not cancel the committed prediction";
 service->OnEndEdit(&context,7,&cleanup);EXPECT_EQ(transport.invalidations,before+1)<<"The exception is single use";
 service->Deactivate();service->Release();
}
TEST(TsfTextService, TransitoryCleanupRejectsUnverifiedOrExternalChanges) {
 for(int scenario=0;scenario<17;++scenario){
  TransitoryContext context;EditingTransport transport;transport.action.set_consumed(true);
  auto* request=transport.action.mutable_commit_request();request->set_request_id(1400);request->set_text("君不见");
  test_native_available=true;test_native_caret.caret={0,0,1,20};
  if(scenario==0)context.transitory=false;
  if(scenario==1)context.fail_status=true;
  auto* service=new TsfTextService(&transport,TestNativeCaret);context.service=service;BOOL eaten=FALSE;
  ASSERT_EQ(service->OnKeyDown(&context,'N',0,&eaten),S_OK);ASSERT_TRUE(eaten);
  TextRange written(context.document);EditRecord own;own.changed_text=&written;service->OnEndEdit(&context,7,&own);
  context.document.clear();context.range.start=context.range.end=0;context.selection_start=context.selection_end=0;
  context.transitory=true;context.fail_status=false;
  TextRange cleared(context.document);TextRange second(context.document);EditRecord record;record.changed_text=&cleared;
  if(scenario==2)context.transitory=false;
  if(scenario==3)context.fail_status=true;
  if(scenario==4)context.fail_boundaries=true;
  if(scenario==5)context.nonempty_context=true;
  if(scenario==6)context.fail_selection=true;
  if(scenario==7)cleared.start=cleared.end=1;
  if(scenario==8){record.changed_text=nullptr;record.selection_changed=TRUE;}
  if(scenario==9)record.second_text=&second;
  if(scenario==10)cleared.end=1;
  if(scenario==11)cleared.fail_queries=true;
  if(scenario==12)test_native_caret.caret.left=10;
  if(scenario==13)test_native_available=false;
  if(scenario==14)service->OnTestKeyDown(&context,VK_DELETE,0,&eaten);
  if(scenario==15)service->OnPreservedKey(&context,GUID_NULL,&eaten);
  if(scenario==16)Sleep(550);
  const auto before=transport.invalidations;service->OnEndEdit(&context,7,&record);
  EXPECT_EQ(transport.invalidations,before+1)<<"scenario="<<scenario;
  service->Deactivate();service->Release();
 }
 test_native_available=true;test_native_caret.caret={0,0,1,20};
}
TEST(TsfTextService, NewNativeKeyDisarmsDelayedInitialOwnNotification) {
 TransitoryContext context;EditingTransport transport;transport.action.set_consumed(true);
 auto* request=transport.action.mutable_commit_request();request->set_request_id(1500);request->set_text("君不见");
 auto* service=new TsfTextService(&transport,TestNativeCaret);context.service=service;BOOL eaten=FALSE;
 ASSERT_EQ(service->OnKeyDown(&context,'N',0,&eaten),S_OK);ASSERT_TRUE(eaten);
 // No initial own notification has arrived when the user starts a native edit.
 service->OnTestKeyDown(&context,VK_DELETE,0,&eaten);
 context.document.clear();context.selection_start=context.selection_end=0;
 TextRange changed(context.document);EditRecord external;external.changed_text=&changed;
 const auto before=transport.invalidations;service->OnEndEdit(&context,7,&external);
 EXPECT_EQ(transport.invalidations,before+1);
 service->Deactivate();service->Release();
}
} }

namespace zhixu::tsf { namespace {
TEST(TsfTextService, PinyinIdleSpaceBelongsToApplicationBeforeAndAfterCommit) {
  EditableContext context;
  EditingTransport transport;
  auto* service = new TsfTextService(&transport);
  BOOL eaten = TRUE;
  EXPECT_EQ(service->OnTestKeyDown(&context, VK_SPACE, 0, &eaten), S_OK);
  EXPECT_FALSE(eaten) << "IMM hosts must receive idle half-width Space in the test stage";
  EXPECT_EQ(service->OnKeyDown(&context, VK_SPACE, 0, &eaten), S_OK);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(transport.dispatches, 0U);
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_preedit("ni");
  service->OnKeyDown(&context, 'N', 0, &eaten);
  service->OnTestKeyDown(&context, VK_SPACE, 0, &eaten);
  EXPECT_TRUE(eaten) << "An active Pinyin composition must still claim candidate selection";
  transport.action.mutable_composition()->set_preedit("");
  auto* commit = transport.action.mutable_commit_request();
  commit->set_request_id(918273);
  commit->set_expected_context_version(transport.last_key.context_version);
  commit->set_text("你");
  service->OnKeyDown(&context, VK_SPACE, 0, &eaten);
  EXPECT_TRUE(eaten);
  EXPECT_EQ(context.document, L"你");
  const auto dispatches = transport.dispatches;
  service->OnTestKeyDown(&context, VK_SPACE, 0, &eaten);
  EXPECT_FALSE(eaten);
  service->OnKeyDown(&context, VK_SPACE, 0, &eaten);
  EXPECT_FALSE(eaten);
  EXPECT_EQ(transport.dispatches, dispatches);
  service->Deactivate();
  service->Release();
}
TEST(TsfTextService, FullShapeIdleSpaceRemainsEngineOwned) {
  EditingTransport transport;
  transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_full_shape(true);
  auto* service = new TsfTextService(&transport);
  BOOL eaten = FALSE;
  service->OnKeyDown(nullptr, VK_SHIFT, 0, &eaten);
  service->OnTestKeyDown(nullptr, VK_SPACE, 0, &eaten);
  EXPECT_TRUE(eaten);
  service->Release();
}
} }
namespace zhixu::tsf { namespace {
TEST(TsfTextService, ChineseShiftDigitsReachPunctuationEngine) {
  BYTE saved[256]{}; ASSERT_TRUE(GetKeyboardState(saved));
  BYTE state[256]{}; state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
  ASSERT_TRUE(SetKeyboardState(state));
  for (UINT key = '0'; key <= '9'; ++key) {
    EditingTransport transport; transport.action.set_consumed(true);
    auto* service = new TsfTextService(&transport); BOOL eaten = FALSE;
    EXPECT_EQ(service->OnTestKeyDown(nullptr, key, 0, &eaten), S_OK);
    EXPECT_TRUE(eaten) << "Shift+" << char(key);
    EXPECT_EQ(transport.dispatches, 0U);
    EXPECT_EQ(service->OnKeyDown(nullptr, key, 0, &eaten), S_OK);
    EXPECT_TRUE(eaten) << "Shift+" << char(key);
    EXPECT_EQ(transport.dispatches, 1U);
    EXPECT_EQ(transport.last_key.virtual_key, key);
    EXPECT_EQ(transport.last_key.modifiers, 4U);
    service->Release();
  }
  ASSERT_TRUE(SetKeyboardState(saved));
}
TEST(TsfTextService, AsciiShiftDigitsRemainNative) {
  BYTE saved[256]{}; ASSERT_TRUE(GetKeyboardState(saved)); BYTE state[256]{};
  ASSERT_TRUE(SetKeyboardState(state));
  EditingTransport transport; transport.action.set_consumed(true);
  transport.action.mutable_composition()->set_ascii_mode(true);
  auto* service = new TsfTextService(&transport); BOOL eaten=FALSE;
  service->OnKeyDown(nullptr,VK_SHIFT,0,&eaten);
  service->OnKeyUp(nullptr,VK_SHIFT,0,&eaten);
  const auto dispatched = transport.dispatches;
  state[VK_SHIFT] = state[VK_LSHIFT] = 0x80; ASSERT_TRUE(SetKeyboardState(state));
  for(UINT key='0'; key<='9'; ++key) {
    service->OnTestKeyDown(nullptr,key,0,&eaten); EXPECT_FALSE(eaten);
    service->OnKeyDown(nullptr,key,0,&eaten); EXPECT_FALSE(eaten);
  }
  EXPECT_EQ(transport.dispatches,dispatched); service->Release();
  ASSERT_TRUE(SetKeyboardState(saved));
}
} }
