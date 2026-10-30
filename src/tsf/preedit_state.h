#pragma once

#include <string>
#include <string_view>

namespace zhixu::tsf {

enum class PreeditOperation {
  Update,
  Clear,
  Commit,
};

class PreeditState final {
 public:
  [[nodiscard]] PreeditOperation Next(std::string_view preedit, bool has_commit_request) const noexcept;
  void Apply(std::string_view preedit);
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] const std::string& text() const noexcept;

 private:
  std::string text_;
};

}  // namespace zhixu::tsf
