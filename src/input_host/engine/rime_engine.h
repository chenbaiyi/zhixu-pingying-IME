#pragma once

#include <filesystem>
#include <memory>

#include "input_host/engine/input_engine.h"

namespace zhixu::input_host {

struct RimeEngineConfig {
  std::filesystem::path shared_data_directory;
  std::filesystem::path user_data_directory;
  // Pin the product schema even when the profile remembers a test schema.
  std::string schema_id;
};

class RimeEngine final : public InputEngine {
 public:
  explicit RimeEngine(RimeEngineConfig config);
  ~RimeEngine() override;
  RimeEngine(const RimeEngine&) = delete;
  RimeEngine& operator=(const RimeEngine&) = delete;

  [[nodiscard]] bool available() const noexcept;
  EngineState ProcessKey(const EngineKey& key) override;
  CommitOrState ProcessKeyAction(const EngineKey& key) override;
  CommitOrState SelectCandidate(std::size_t index) override;
  void ResetComposition() noexcept override;
  bool Configure(bool fuzzy, unsigned count, bool learning) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace zhixu::input_host
