#pragma once

#include "predict/prediction/prediction_types.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace zhixu::predict {

class Retriever final {
 public:
  explicit Retriever(std::vector<std::filesystem::path> package_roots);

  [[nodiscard]] std::vector<Suggestion> Recall(const PredictionContext& context, size_t limit) const;

 private:
  std::vector<std::filesystem::path> package_roots_;
};

}  // namespace zhixu::predict
