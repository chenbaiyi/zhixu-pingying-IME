#include "input_host/engine/rime_engine.h"
#include "common/protocol/key_modifiers.h"

#include <utility>
#include <algorithm>
#include <limits>
#include <array>
#include <fstream>

#ifdef ZHIXU_ENABLE_LIBRIME
#include <cstring>
#include <mutex>
#include <string>

#include <rime_api.h>

#include <Windows.h>
#ifdef ZHIXU_ENABLE_OCTAGRAM
// Keep the statically linked module from being discarded by the linker.
void rime_require_module_octagram();
#endif
#endif

namespace zhixu::input_host {

#ifdef ZHIXU_ENABLE_LIBRIME
namespace {

std::string ToUtf8(const std::filesystem::path& path) {
  const auto wide = path.wstring();
  if (wide.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                       static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (size == 0) return {};
  std::string utf8(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                          static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr) == 0) return {};
  return utf8;
}

struct RimeRuntime { std::mutex mutex; RimeApi* api = nullptr; std::size_t users = 0; };
RimeRuntime& Runtime() { static RimeRuntime runtime; return runtime; }

#ifdef ZHIXU_ENABLE_OCTAGRAM
bool HasGrammarModel(const RimeEngineConfig& config) {
  // Match ResourceResolver's user-first lookup order.
  auto path = config.user_data_directory / "wanxiang-lts-zh-hans.gram";
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (error) return false;
    path = config.shared_data_directory / "wanxiang-lts-zh-hans.gram";
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error || size < 44) return false;
  std::array<char, 44> header{};
  std::ifstream stream(path, std::ios::binary);
  if (!stream.read(header.data(), header.size()) ||
      std::memcmp(header.data(), "Rime::Grammar/1.0", 17) != 0) return false;
  std::uint32_t units = 0, offset = 0;
  std::memcpy(&units, header.data() + 36, sizeof(units));
  std::memcpy(&offset, header.data() + 40, sizeof(offset));
  const std::uint64_t start = 40ULL + offset;
  return units != 0 && start >= header.size() && start <= size &&
         units <= (size - start) / sizeof(std::uint32_t);
}
#endif

EngineState ReadState(RimeApi* api, const RimeSessionId session, const bool consumed) {
  EngineState state; state.consumed = consumed;
  RimeContext context{}; context.data_size = sizeof(context) - sizeof(context.data_size);
  if (api->get_context(session, &context)) {
    if (context.composition.preedit != nullptr) state.preedit = context.composition.preedit;
    state.cursor = static_cast<std::uint32_t>(context.composition.cursor_pos);
    state.page = static_cast<std::uint32_t>(context.menu.page_no);
    state.highlighted_candidate = static_cast<std::uint32_t>(context.menu.highlighted_candidate_index);
    state.has_next_page = context.menu.is_last_page == False;
    for (int index = 0; index < context.menu.num_candidates; ++index)
      if (context.menu.candidates[index].text != nullptr)
        state.candidate_text.emplace_back(context.menu.candidates[index].text);
    api->free_context(&context);
  }
  RimeStatus status{}; status.data_size = sizeof(status) - sizeof(status.data_size);
  if (api->get_status(session, &status)) {
    state.ascii_mode = status.is_ascii_mode != False;
    state.full_shape = status.is_full_shape != False;
    state.ascii_punctuation = status.is_ascii_punct != False;
    state.traditional = status.is_traditional != False;
    api->free_status(&status);
  }
  return state;
}

}  // namespace
#endif

class RimeEngine::Impl {
 public:
  explicit Impl(RimeEngineConfig config) {
#ifdef ZHIXU_ENABLE_LIBRIME
    schema_id_ = config.schema_id;
    shared_data_directory_ = ToUtf8(config.shared_data_directory);
    user_data_directory_ = ToUtf8(config.user_data_directory);
    if (shared_data_directory_.empty() || user_data_directory_.empty()) return;
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    if (runtime.api == nullptr) {
      runtime.api = rime_get_api();
      if (runtime.api == nullptr) return;
      RimeTraits traits{}; traits.data_size = sizeof(traits) - sizeof(traits.data_size);
      traits.shared_data_dir = shared_data_directory_.c_str(); traits.user_data_dir = user_data_directory_.c_str();
      traits.app_name = "rime.zhixuime"; traits.min_log_level = 2; traits.log_dir = "";
#ifdef ZHIXU_ENABLE_OCTAGRAM
      rime_require_module_octagram();
      static const char* grammar_modules[] = {"default", "octagram", nullptr};
      static const char* default_modules[] = {"default", nullptr};
      // The installer verifies SHA256. Reject absent/truncated/incompatible
      // images here before the third-party memory-mapped loader sees them.
      traits.modules = HasGrammarModel(config)
                           ? grammar_modules : default_modules;
#endif
      runtime.api->setup(&traits); runtime.api->initialize(&traits);
      // Initialization alone does not compile schemas or dictionaries for a
      // fresh profile. Finish deployment before accepting the first key.
      // A previously initialized profile may skip deployment with False,
      // even when the product changes its bundled schema/dictionary.
      runtime.api->start_maintenance(config.schema_id.empty() ? False : True);
      runtime.api->join_maintenance_thread();
    }
    session_ = runtime.api->create_session();
    if (session_ == 0 || (!config.schema_id.empty() &&
        runtime.api->select_schema(session_, config.schema_id.c_str()) == False)) {
      if (session_ != 0) runtime.api->destroy_session(session_);
      session_ = 0;
      if (runtime.users == 0) { runtime.api->finalize(); runtime.api = nullptr; }
      return;
    }
    runtime.api->set_option(session_, "ascii_mode", False);
    ++runtime.users; api_ = runtime.api;
#else
    static_cast<void>(config);
#endif
  }
  ~Impl() {
#ifdef ZHIXU_ENABLE_LIBRIME
    if (api_ == nullptr) return;
    auto& runtime = Runtime(); std::lock_guard lock(runtime.mutex);
    api_->destroy_session(session_);
    if (--runtime.users == 0) { api_->finalize(); runtime.api = nullptr; }
#endif
  }
  [[nodiscard]] bool available() const noexcept {
#ifdef ZHIXU_ENABLE_LIBRIME
    return api_ != nullptr;
#else
    return false;
#endif
  }
#ifdef ZHIXU_ENABLE_LIBRIME
  EngineState ReadViewState(const bool consumed) {
    auto state = ReadState(api_, session_, consumed);
    if (state.preedit.empty() || state.candidate_text.empty()) {
      expanded_page_ = 0; grid_highlight_ = 0;
      return state;
    }
    if (expanded_page_ == 0) return state;
    state.candidate_text.clear();
    state.has_next_page = false;
    const std::size_t offset = (expanded_page_ - 1) * 30ull;
    RimeCandidateListIterator iterator{};
    if (offset <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
        api_->candidate_list_from_index(session_, &iterator, static_cast<int>(offset))) {
      while (api_->candidate_list_next(&iterator)) {
        if (state.candidate_text.size() == 30) { state.has_next_page = true; break; }
        state.candidate_text.emplace_back(iterator.candidate.text ? iterator.candidate.text : "");
      }
      api_->candidate_list_end(&iterator);
    }
    state.page = expanded_page_;
    if (grid_highlight_ >= state.candidate_text.size()) grid_highlight_ = 0;
    state.highlighted_candidate = grid_highlight_;
    // The public Next action advances a row; a later batch is needed only
    // after the last visible row, not on every press of the paging key.
    state.has_next_page = state.has_next_page ||
        (grid_highlight_ / 6 + 1) * 6 < state.candidate_text.size();
    return state;
  }
#endif
  CommitOrState ProcessKeyAction(const EngineKey& key) {
#ifdef ZHIXU_ENABLE_LIBRIME
    if (api_ != nullptr && options_ready_) {
      const bool shift_key = key.virtual_key == VK_SHIFT || key.virtual_key == VK_LSHIFT ||
                             key.virtual_key == VK_RSHIFT;
      if (!key.key_down && !shift_key) return {.state = ReadViewState(false)};
      const auto held = key.modifiers & protocol::kHeldModifiers;
      // Application shortcuts are never fed into the editor; only the
      // explicitly owned punctuation toggle is processed here.
      if ((held & (protocol::kAlt | protocol::kSuper)) != 0 ||
          ((held & protocol::kControl) != 0 &&
           !(held == protocol::kControl && key.virtual_key == VK_OEM_PERIOD) && !shift_key)) {
        return {.state = ReadViewState(false)};
      }
      // Keep Rime on its compact first page. Expansion retains that same
      // candidate stream from index zero; paging keys then navigate rows.
      if (held == 0 && key.key_down) {
        auto current = ReadViewState(false);
        const bool next = key.virtual_key == VK_NEXT || key.virtual_key == VK_OEM_PLUS;
        const bool previous = key.virtual_key == VK_PRIOR || key.virtual_key == VK_OEM_MINUS;
        if (!current.candidate_text.empty() && (next || previous)) {
          if (next && current.has_next_page) {
            if (expanded_page_ == 0) {
              expanded_page_ = 1;
              grid_highlight_ = current.highlighted_candidate;
            } else if ((grid_highlight_ / 6 + 1) * 6 < current.candidate_text.size()) {
              grid_highlight_ = (grid_highlight_ / 6 + 1) * 6;
            } else {
              ++expanded_page_; grid_highlight_ = 0;
            }
          } else if (previous && expanded_page_ > 0) {
            if (grid_highlight_ >= 6) {
              grid_highlight_ = (grid_highlight_ / 6 - 1) * 6;
            } else {
              --expanded_page_; grid_highlight_ = 0;
              if (expanded_page_ > 0) {
                const auto previous_batch = ReadViewState(false);
                if (!previous_batch.candidate_text.empty())
                  grid_highlight_ = static_cast<unsigned>((previous_batch.candidate_text.size()-1) / 6) * 6;
              }
            }
          }
          return {.state = ReadViewState(true)};
        }
        const bool vertical = key.virtual_key == VK_UP || key.virtual_key == VK_DOWN;
        const bool horizontal = key.virtual_key == VK_LEFT || key.virtual_key == VK_RIGHT;
        if (!current.candidate_text.empty() && (vertical || (expanded_page_ && horizontal))) {
          const int step = vertical ? (expanded_page_ ? 6 : 1) : 1;
          const bool backward = key.virtual_key == VK_UP || key.virtual_key == VK_LEFT;
          const int target = static_cast<int>(current.highlighted_candidate) + (backward ? -step : step);
          if (expanded_page_) {
            if (target >= 0 && target < static_cast<int>(current.candidate_text.size()))
              grid_highlight_ = static_cast<unsigned>(target);
          } else {
            const auto index = std::clamp(target, 0, static_cast<int>(current.candidate_text.size())-1);
            api_->highlight_candidate_on_current_page(session_, index);
          }
          return {.state = ReadViewState(true)};
        }
        if (expanded_page_ && key.virtual_key == VK_SPACE)
          return SelectCandidate(grid_highlight_);
      }
      if (held == 0 && (key.virtual_key == VK_HOME || key.virtual_key == VK_END)) {
        const auto* input = api_->get_input(session_);
        if (input && *input) {
          expanded_page_ = 0; grid_highlight_ = 0;
          api_->set_caret_pos(session_, key.virtual_key == VK_HOME ? 0 : std::strlen(input));
          return {.state = ReadViewState(true)};
        }
        return {.state = ReadViewState(false)};
      }
      // Windows letter virtual keys are uppercase even for an unmodified
      // lowercase keystroke. Rime's speller expects lowercase key symbols.
      auto key_symbol = key.virtual_key;
      if (key_symbol >= 'A' && key_symbol <= 'Z') {
        if (((key.modifiers & protocol::kShift) != 0) ==
            ((key.modifiers & protocol::kCapsLock) != 0)) key_symbol += 'a' - 'A';
      }
      // Rime uses X11 key symbols, not Windows virtual-key values, for these
      // editing keys. Otherwise the host deletes text while Rime keeps input.
      if (key_symbol == VK_BACK) key_symbol = 0xff08;  // BackSpace
      if (key_symbol == VK_ESCAPE) key_symbol = 0xff1b;  // Escape
      if (key_symbol == VK_DELETE) key_symbol = 0xffff;  // Delete
      if (key_symbol == VK_LEFT) key_symbol = 0xff51;  // Left
      if (key_symbol == VK_RIGHT) key_symbol = 0xff53;  // Right
      if (key.virtual_key == VK_UP) key_symbol = 0xff52;
      if (key.virtual_key == VK_DOWN) key_symbol = 0xff54;
      if (key.virtual_key == VK_HOME) key_symbol = 0xff50;
      if (key.virtual_key == VK_END) key_symbol = 0xff57;
      if (key.virtual_key == VK_RETURN) key_symbol = 0xff0d;
      if (shift_key) key_symbol = key.virtual_key == VK_RSHIFT ? 0xffe2 : 0xffe1;
      if (key.virtual_key == VK_CAPITAL) key_symbol = 0xffe5;
      if (key_symbol == VK_PRIOR) key_symbol = 0xff55;  // Page_Up
      if (key_symbol == VK_NEXT) key_symbol = 0xff56;  // Page_Down
      if (key_symbol == VK_OEM_MINUS) key_symbol = '-';
      if (key_symbol == VK_OEM_PLUS) key_symbol = '=';
      if (key_symbol == VK_OEM_7) key_symbol = '\'';  // Pinyin syllable separator.
      if (key.virtual_key == VK_OEM_COMMA) key_symbol = ',';
      if (key.virtual_key == VK_OEM_PERIOD) key_symbol = '.';
      if (key.virtual_key == VK_OEM_1) key_symbol = ';';
      if (key.virtual_key == VK_OEM_2) key_symbol = '/';
      if (key.virtual_key == VK_OEM_3) key_symbol = '`';
      if (key.virtual_key == VK_OEM_4) key_symbol = '[';
      if (key.virtual_key == VK_OEM_5) key_symbol = '\\';
      if (key.virtual_key == VK_OEM_6) key_symbol = ']';
      if ((held & protocol::kShift) != 0 && key_symbol < 0x100) {
        const std::string plain = "1234567890-=[]\\;',./`";
        const std::string shifted = "!@#$%^&*()_+{}|:\"<>?~";
        const auto position = plain.find(static_cast<char>(key_symbol));
        if (position != std::string::npos) key_symbol = shifted[position];
      }
      if (key.key_symbol != 0) key_symbol = key.key_symbol;
      // The wire protocol's Alt bit is 1; Rime's X11 Alt/Mod1 mask is 8.
      // Passing 1 directly would tell Rime that Shift is pressed.
      int rime_modifiers = ((key.modifiers & protocol::kShift) != 0 ? 1 : 0) |
                           ((key.modifiers & protocol::kCapsLock) != 0 ? 2 : 0) |
                           ((key.modifiers & protocol::kControl) != 0 ? 4 : 0) |
                           (!key.key_down ? (1 << 30) : 0);
      // Windows reports the new toggle state on the CapsLock key itself.
      if (key.virtual_key == VK_CAPITAL) rime_modifiers ^= 2;
      const char* raw_input = api_->get_input(session_);
      const std::string before_input = raw_input ? raw_input : "";
      const auto before_caret = api_->get_caret_pos(session_);
      const bool consumed = api_->process_key(session_, static_cast<int>(key_symbol), rime_modifiers) != False;
      raw_input = api_->get_input(session_);
      // Modified editor keys can also change spelling (e.g. Shift+Backspace).
      // Inspect the actual edit instead of assuming only plain keys edit it.
      if (before_input != (raw_input ? raw_input : "") || before_caret != api_->get_caret_pos(session_)) {
        expanded_page_ = 0; grid_highlight_ = 0;
      }
      CommitOrState outcome{.state = ReadViewState(consumed)};
      RimeCommit commit{}; commit.data_size = sizeof(commit) - sizeof(commit.data_size);
      if (api_->get_commit(session_, &commit)) {
        if (commit.text != nullptr) outcome.commit_text = commit.text;
        api_->free_commit(&commit);
      }
      if (outcome.commit_text) {
        // Rime may commit the old composition then reject the printable key
        // (uppercase/ASCII punctuation). Write both atomically, not twice.
        if (!consumed && key.key_down && key_symbol >= 0x20 && key_symbol <= 0x7e && held == 0)
          outcome.commit_text->push_back(static_cast<char>(key_symbol));
        else if (!consumed && key.key_down && key_symbol >= 0x20 && key_symbol <= 0x7e && held == protocol::kShift)
          outcome.commit_text->push_back(static_cast<char>(key_symbol));
        outcome.state.consumed = true;
      }
      return outcome;
    }
#else
    static_cast<void>(key);
#endif
    return {};
  }
  CommitOrState SelectCandidate(const std::size_t index) {
#ifdef ZHIXU_ENABLE_LIBRIME
    if (api_ != nullptr) {
      if (!options_ready_) return {};
      // Host numbers and candidate.index are relative to the visible page.
      auto current = ReadViewState(false);
      if (index >= current.candidate_text.size()) {
        // Ignore an unavailable number while keeping the user's preedit.
        // With no menu, the same number remains an application key.
        if (current.candidate_text.empty() && index < 9)
          return ProcessKeyAction({.virtual_key = static_cast<std::uint32_t>('1' + index), .key_down = true});
        current.consumed = !current.candidate_text.empty();
        return {.state = std::move(current)};
      }
      const auto absolute = expanded_page_ ? (expanded_page_ - 1) * 30ull + index : index;
      const bool consumed = api_->select_candidate(session_, absolute) != False;
      expanded_page_ = 0; grid_highlight_ = 0;
      CommitOrState outcome{.state = ReadViewState(consumed)};
      RimeCommit commit{}; commit.data_size = sizeof(commit) - sizeof(commit.data_size);
      if (api_->get_commit(session_, &commit)) {
        if (commit.text != nullptr) outcome.commit_text = commit.text;
        api_->free_commit(&commit);
      }
      return outcome;
    }
#else
    static_cast<void>(index);
#endif
    return {};
  }
  bool Configure(bool fuzzy, unsigned count, bool learning) {
#ifdef ZHIXU_ENABLE_LIBRIME
    if (!api_ || schema_id_.empty() || count < 1 || count > 9) return false;
    if (options_ready_ && configured_ && fuzzy_ == fuzzy && count_ == count && learning_ == learning) return false;
    if (configured_ && options_ready_ && learning_ == learning) {
      const char* input=api_->get_input(session_);
      if(input && *input) return false;
    }
    const char* existing=api_->get_input(session_);
    const std::string spelling=existing ? existing : "";
    const auto caret=api_->get_caret_pos(session_);
    const bool ascii = api_->get_option(session_, "ascii_mode") != False;
    const bool punct = api_->get_option(session_, "ascii_punct") != False;
    const bool full = api_->get_option(session_, "full_shape") != False;
    options_ready_ = false;
    RimeConfig config{};
    if (!api_->schema_open(schema_id_.c_str(), &config)) return false;
    bool ok = api_->config_set_int(&config,"menu/page_size",count) &&
        api_->config_set_bool(&config,"translator/enable_user_dict",learning);
    if (schema_id_ == "zhixu_pinyin")
      ok = ok && api_->config_set_string(&config,"translator/prism",fuzzy ? "zhixu_pinyin" : "zhixu_pinyin_exact");
    // Keep the shared config alive while the replacement Schema reads its data.
    if (ok) ok = api_->select_schema(session_,schema_id_.c_str()) != False;
    api_->config_close(&config);
    if (!ok) { api_->clear_composition(session_); return false; }
    api_->set_option(session_,"ascii_mode",ascii); api_->set_option(session_,"ascii_punct",punct); api_->set_option(session_,"full_shape",full);
    if (!spelling.empty()) {
      if (!api_->set_input(session_,spelling.c_str())) { api_->clear_composition(session_); return false; }
      api_->set_caret_pos(session_,caret);
    }
    fuzzy_=fuzzy; count_=count; learning_=learning; configured_=true; options_ready_=true;
    expanded_page_=0; grid_highlight_=0;
    return true;
#else
    return false;
#endif
  }
  void ResetComposition() noexcept {
#ifdef ZHIXU_ENABLE_LIBRIME
    if (api_ != nullptr) {
      expanded_page_ = 0; grid_highlight_ = 0;
      api_->clear_composition(session_);
      // Clear an interrupted modifier tap without toggling mode on the next
      // application's Shift release. X11 Control+Shift release is a no-op.
      api_->process_key(session_, 0xffe1, (1 << 30) | 1 | 4);
      // Discard any uncollected commit from the invalidated session, too.
      RimeCommit commit{}; commit.data_size = sizeof(commit) - sizeof(commit.data_size);
      if (api_->get_commit(session_, &commit)) api_->free_commit(&commit);
    }
#endif
  }
 private:
#ifdef ZHIXU_ENABLE_LIBRIME
  std::string schema_id_;
  bool configured_=false, options_ready_=true, fuzzy_=true, learning_=true; unsigned count_=8;
  std::string shared_data_directory_; std::string user_data_directory_;
  RimeApi* api_ = nullptr; RimeSessionId session_ = 0;
  unsigned expanded_page_ = 0, grid_highlight_ = 0;
#endif
};

RimeEngine::RimeEngine(RimeEngineConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
RimeEngine::~RimeEngine() = default;
bool RimeEngine::available() const noexcept { return impl_->available(); }
EngineState RimeEngine::ProcessKey(const EngineKey& key) { return impl_->ProcessKeyAction(key).state; }
CommitOrState RimeEngine::ProcessKeyAction(const EngineKey& key) { return impl_->ProcessKeyAction(key); }
CommitOrState RimeEngine::SelectCandidate(const std::size_t index) { return impl_->SelectCandidate(index); }
void RimeEngine::ResetComposition() noexcept { impl_->ResetComposition(); }

}  // namespace zhixu::input_host

bool zhixu::input_host::RimeEngine::Configure(bool fuzzy, unsigned count, bool learning) { return impl_->Configure(fuzzy,count,learning); }
