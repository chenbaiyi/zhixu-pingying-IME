#pragma once

#include "predict/personalization/personalization_store.h"
#include "predict/prediction/prediction_types.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace zhixu::predict {

class PredictionCoordinator final {
 public:
  PredictionCoordinator(std::vector<std::filesystem::path> package_roots,
                        PersonalizationStore& personalization_store);

  [[nodiscard]] std::vector<Suggestion> Predict(PredictionContext context, size_t limit) const;

 private:
  std::vector<std::filesystem::path> package_roots_;
  PersonalizationStore& personalization_store_;
};

}  // namespace zhixu::predict
