#include "predict/prediction/ranker.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace zhixu::predict {
namespace {

int CategoryPrecedence(const LocalSuggestionKind kind) {
  switch (kind) {
    case LocalSuggestionKind::kExactContinuation: return 1;
    case LocalSuggestionKind::kQuotation: return 0;
    case LocalSuggestionKind::kUserNGram: return 2;
    case LocalSuggestionKind::kBaseNGram: return 3;
    case LocalSuggestionKind::kModelGenerated: return 4;
  }
  return 5;
}

bool HasSensitiveTag(const Suggestion& suggestion) {
  return std::find(suggestion.tags.begin(), suggestion.tags.end(), "sensitive") != suggestion.tags.end();
}

bool IsAlreadyEntered(const std::string_view context, const std::string_view suggestion) {
  return !suggestion.empty() && context.size() >= suggestion.size() &&
         context.ends_with(suggestion);
}

size_t ScalarCount(std::string_view text) { return std::count_if(text.begin(),text.end(),[](unsigned char c){return (c&0xc0)!=0x80;}); }
bool PreferredLength(const Suggestion& s) { const auto n=ScalarCount(s.text);return s.kind==LocalSuggestionKind::kQuotation || (n>=2 && n<=8); }
bool IsCuratedQuotation(const Suggestion& suggestion) {
  return suggestion.kind == LocalSuggestionKind::kQuotation &&
         (suggestion.source_title == "唐诗三百首" ||
          suggestion.source_title == "宋词三百首" ||
          suggestion.source_title == "古诗十九首");
}

bool Better(const Suggestion& left, const Suggestion& right) {
  // A direct, longer context match must beat completion of a shorter suffix.
  // Generated text remains the fallback regardless of its model score.
  if ((left.kind == LocalSuggestionKind::kModelGenerated) !=
      (right.kind == LocalSuggestionKind::kModelGenerated)) {
    return left.kind != LocalSuggestionKind::kModelGenerated;
  }
  if (left.context_distance != right.context_distance) {
    return left.context_distance < right.context_distance;
  }
  if (left.match_length != right.match_length) return left.match_length > right.match_length;
  if (left.is_phrase_completion != right.is_phrase_completion) return left.is_phrase_completion;
  const auto left_category = CategoryPrecedence(left.kind);
  const auto right_category = CategoryPrecedence(right.kind);
  if (left_category != right_category) return left_category < right_category;
  if (IsCuratedQuotation(left) != IsCuratedQuotation(right)) return IsCuratedQuotation(left);
  if (PreferredLength(left)!=PreferredLength(right)) return PreferredLength(left);
  if (left.source_weight != right.source_weight) return left.source_weight > right.source_weight;
  if (IsCuratedQuotation(left) && IsCuratedQuotation(right)) {
    // Use the selected collection's original entry/segment order for repeated openings.
    if (left.source_title != right.source_title) return left.source_title < right.source_title;
    if (left.package_id != right.package_id) return left.package_id < right.package_id;
    const auto left_order = left.source_order ? left.source_order : std::numeric_limits<std::uint64_t>::max();
    const auto right_order = right.source_order ? right.source_order : std::numeric_limits<std::uint64_t>::max();
    if (left_order != right_order) return left_order < right_order;
  }
  if (left.text != right.text) return left.text < right.text;
  if (left.package_id != right.package_id) return left.package_id < right.package_id;
  return left.entry_id < right.entry_id;
}

}  // namespace

std::vector<Suggestion> Ranker::Rank(const PredictionContext& context,
                                     const std::span<const Suggestion> candidates,
                                     const size_t limit) const {
  if (!context.allow_suggestions || limit == 0) return {};
  const std::unordered_set<std::string> blocked(context.blocked_texts.begin(), context.blocked_texts.end());
  std::vector<Suggestion> ranked;
  ranked.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    if ((candidate.kind!=LocalSuggestionKind::kQuotation && ScalarCount(candidate.text)>20) || candidate.text.empty() || HasSensitiveTag(candidate) || blocked.contains(candidate.text) ||
        IsAlreadyEntered(context.committed_text, candidate.text)) {
      continue;
    }
    const auto duplicate = std::find_if(ranked.begin(), ranked.end(), [&](const Suggestion& existing) {
      return existing.text == candidate.text;
    });
    if (duplicate == ranked.end()) {
      ranked.push_back(candidate);
    } else if (Better(candidate, *duplicate)) {
      *duplicate = candidate;
    }
  }
  std::sort(ranked.begin(), ranked.end(), Better);
  // Keep a writing option only when it has equal context strength to the displaced completion.
  // The first choices still prioritize the missing portion of a word or idiom.
  if (limit >= 3 && ranked.size() > limit && ranked.front().is_phrase_completion) {
    const auto writing = std::find_if(ranked.begin(), ranked.end(), [](const Suggestion& s) {
      return !s.is_phrase_completion && s.kind != LocalSuggestionKind::kQuotation &&
             s.kind != LocalSuggestionKind::kModelGenerated;
    });
    if (writing != ranked.end() && writing >= ranked.begin() + limit &&
        writing->match_length == ranked[limit - 1].match_length &&
        writing->context_distance == ranked[limit - 1].context_distance &&
        ranked[limit - 1].kind != LocalSuggestionKind::kQuotation) {
      std::iter_swap(ranked.begin() + limit - 1, writing);
    }
  }
  if (ranked.size() > limit) ranked.resize(limit);
  return ranked;
}

}  // namespace zhixu::predict
