#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zhixu::input_host {

struct EngineKey {
  std::uint32_t virtual_key = 0;
  bool key_down = false;
  std::uint32_t modifiers = 0;
  std::uint32_t key_symbol = 0;
};

struct EngineState {
  bool consumed = false;
  std::string preedit;
  std::vector<std::string> candidate_text;
  std::uint32_t page = 0;
  bool has_next_page = false;
  bool ascii_mode = false;
  bool full_shape = false;
  bool ascii_punctuation = false;
  bool traditional = false;
  std::optional<std::uint32_t> cursor;
  std::uint32_t highlighted_candidate = 0;
};

struct CommitOrState {
  EngineState state;
  std::optional<std::string> commit_text;
};

class InputEngine {
 public:
  virtual ~InputEngine() = default;

  virtual bool Configure(bool fuzzy, unsigned count, bool learning) { return false; }
  virtual EngineState ProcessKey(const EngineKey& key) = 0;
  virtual CommitOrState ProcessKeyAction(const EngineKey& key) {
    return {.state = ProcessKey(key)};
  }
  virtual CommitOrState SelectCandidate(std::size_t index) = 0;
  // Stateless engines need no cleanup; stateful adapters must discard preedit.
  virtual void ResetComposition() noexcept {}
};

}  // namespace zhixu::input_host
