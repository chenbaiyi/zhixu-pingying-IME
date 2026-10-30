#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace zhixu::predict {

enum class LocalSuggestionKind {
  kExactContinuation,
  kQuotation,
  kUserNGram,
  kBaseNGram,
  kModelGenerated,
};

struct PredictionContext {
  std::string committed_text;
  std::vector<std::string> blocked_texts;
  bool allow_suggestions = true;
};

struct Suggestion {
  std::string text;
  LocalSuggestionKind kind = LocalSuggestionKind::kModelGenerated;
  double source_weight = 0.0;
  std::string package_id;
  std::string entry_id;
  std::vector<std::string> tags;
  std::string source_title;
  std::string author;
  std::string source_context;
  unsigned int match_length = 0;
  unsigned int context_distance = 0;
  bool is_phrase_completion = false;
  std::uint64_t source_order = 0; // Existing ordered quotation index row; not a popularity score.
};

}  // namespace zhixu::predict
