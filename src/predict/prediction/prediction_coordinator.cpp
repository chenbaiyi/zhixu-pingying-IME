#include "predict/prediction/prediction_coordinator.h"

#include "predict/prediction/ranker.h"
#include "predict/prediction/retriever.h"

#include <algorithm>
#include <utility>

namespace zhixu::predict {

PredictionCoordinator::PredictionCoordinator(std::vector<std::filesystem::path> package_roots,
                                             PersonalizationStore& personalization_store)
    : package_roots_(std::move(package_roots)), personalization_store_(personalization_store) {
  // Consult compact writing data and poetry selections before large general volumes
  // so the shared bounded recall deadline does not starve curated quotations.
  const auto priority = [](const std::filesystem::path& root) {
    const auto id = root.parent_path().filename().string();
    if (id.find("writing") != std::string::npos) return 0;
    if (id.find("poetry") != std::string::npos) return 1;
    return 2;
  };
  std::stable_sort(package_roots_.begin(), package_roots_.end(), [&](const auto& a, const auto& b) {
    return priority(a) < priority(b);
  });
}

std::vector<Suggestion> PredictionCoordinator::Predict(PredictionContext context, const size_t limit) const {
  if (!context.allow_suggestions || limit == 0) return {};

  const auto blocked = personalization_store_.BlockedSuggestions();
  context.blocked_texts.insert(context.blocked_texts.end(), blocked.begin(), blocked.end());

  Retriever retriever(package_roots_);
  auto candidates = retriever.Recall(context, limit);
  const auto ngrams = personalization_store_.RecallNGrams(context.committed_text, limit);
  candidates.reserve(candidates.size() + ngrams.size());
  for (const auto& ngram : ngrams) {
    candidates.push_back({
        .text = ngram.completion,
        .kind = LocalSuggestionKind::kUserNGram,
        .source_weight = static_cast<double>(ngram.frequency) /
                         static_cast<double>(ngram.frequency + 1),
    });
  }

  return Ranker().Rank(context, candidates, limit);
}

}  // namespace zhixu::predict
