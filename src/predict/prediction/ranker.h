#pragma once

#include "predict/prediction/prediction_types.h"

#include <cstddef>
#include <span>
#include <vector>

namespace zhixu::predict {

class Ranker final {
 public:
  [[nodiscard]] std::vector<Suggestion> Rank(const PredictionContext& context,
                                             std::span<const Suggestion> candidates,
                                             size_t limit) const;
};

}  // namespace zhixu::predict
