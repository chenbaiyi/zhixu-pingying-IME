#include "predict/prediction/ranker.h"

#include <vector>

#include <gtest/gtest.h>

namespace zhixu::predict {
namespace {

TEST(Ranker, ExactContinuationWinsAndDropsDuplicateEnteredSuffixes) {
  const PredictionContext context{
      .committed_text = "道韵法",
      .blocked_texts = {},
      .allow_suggestions = true,
  };
  const std::vector<Suggestion> candidates{
      {.text = "悠长", .kind = LocalSuggestionKind::kModelGenerated, .source_weight = 0.92},
      {.text = "法则", .kind = LocalSuggestionKind::kExactContinuation, .source_weight = 1.0,
       .package_id = "com.zhixu.acceptance", .entry_id = "acceptance.daoyun"},
      {.text = "法则", .kind = LocalSuggestionKind::kModelGenerated, .source_weight = 0.99},
      {.text = "法", .kind = LocalSuggestionKind::kExactContinuation, .source_weight = 1.0},
      {.text = "机密", .kind = LocalSuggestionKind::kExactContinuation, .source_weight = 1.0,
       .tags = {"sensitive"}},
  };

  Ranker ranker;
  const auto ranked = ranker.Rank(context, candidates, 3);

  ASSERT_EQ(ranked.size(), 2u);
  EXPECT_EQ(ranked.front().text, "法则");
  EXPECT_EQ(ranked.front().kind, LocalSuggestionKind::kExactContinuation);
  EXPECT_EQ(ranked[1].text, "悠长");
}

TEST(Ranker, CapsOrdinaryUnicodeButPreservesAccurateQuotationsAndPrefersShortPairs) {
 std::string long_text;for(int i=0;i<21;++i)long_text+="𠀀";
 std::vector<Suggestion> candidates{{.text=long_text,.kind=LocalSuggestionKind::kUserNGram},
 {.text="缓缓收起手中那柄长剑",.kind=LocalSuggestionKind::kBaseNGram,.source_weight=1.0},
 {.text="收剑入鞘",.kind=LocalSuggestionKind::kBaseNGram,.source_weight=0.8},
 {.text=long_text+"。",.kind=LocalSuggestionKind::kQuotation}};
 const auto ranked=Ranker{}.Rank({},candidates,3);ASSERT_EQ(ranked.size(),3u);
 EXPECT_EQ(ranked[0].kind,LocalSuggestionKind::kQuotation);EXPECT_EQ(ranked[1].text,"收剑入鞘");
}
TEST(Ranker, KeepsContextualWritingAlongsidePriorityPhraseCompletions) {
 const std::vector<Suggestion> candidates{
  {.text="地一推",.kind=LocalSuggestionKind::kBaseNGram,.is_phrase_completion=true},
  {.text="地吹",.kind=LocalSuggestionKind::kBaseNGram,.is_phrase_completion=true},
  {.text="地吹着",.kind=LocalSuggestionKind::kBaseNGram,.is_phrase_completion=true},
  {.text="拂过山岗",.kind=LocalSuggestionKind::kBaseNGram},
  {.text="、小罗靸。",.kind=LocalSuggestionKind::kQuotation}};
 const auto ranked=Ranker{}.Rank({.committed_text="微风挟着花香轻轻"},candidates,3);
 ASSERT_EQ(ranked.size(),3u);EXPECT_TRUE(ranked[0].is_phrase_completion);
 EXPECT_TRUE(ranked[1].is_phrase_completion);EXPECT_EQ(ranked[2].text,"拂过山岗");
 const auto one=Ranker{}.Rank({},candidates,1);ASSERT_EQ(one.size(),1u);EXPECT_TRUE(one[0].is_phrase_completion);
}
TEST(Ranker, FullQuotationMatchWinsOverShorterSuffixPhraseCompletion) {
 const std::vector<Suggestion> candidates{
  {.text="一点踪影",.kind=LocalSuggestionKind::kBaseNGram,.match_length=2,.is_phrase_completion=true},
  {.text="黄河之水天上来，",.kind=LocalSuggestionKind::kQuotation,.source_title="唐诗三百首",.match_length=3},
  {.text="不散",.kind=LocalSuggestionKind::kBaseNGram,.match_length=2,.is_phrase_completion=true}};
 const auto ranked=Ranker{}.Rank({.committed_text="君不见"},candidates,3);
 ASSERT_EQ(ranked.size(),3u);EXPECT_EQ(ranked[0].text,"黄河之水天上来，");
}
TEST(Ranker, PhraseCompletionWinsAtEqualContextStrength) {
 const std::vector<Suggestion> candidates{
  {.text="待兔其事。",.kind=LocalSuggestionKind::kQuotation,.source_title="唐诗三百首",.match_length=2},
  {.text="待兔",.kind=LocalSuggestionKind::kBaseNGram,.match_length=2,.is_phrase_completion=true}};
 const auto ranked=Ranker{}.Rank({.committed_text="守株"},candidates,1);
 ASSERT_EQ(ranked.size(),1u);EXPECT_EQ(ranked[0].text,"待兔");
}
TEST(Ranker, CuratedAnthologyWinsOverAlphabeticalQuotationAtEqualMatch) {
 const std::vector<Suggestion> candidates{
  {.text="一片云山入画来，",.kind=LocalSuggestionKind::kQuotation,.source_weight=1.0,.source_title="某诗集",.match_length=3},
  {.text="黄河之水天上来，",.kind=LocalSuggestionKind::kQuotation,.source_weight=1.0,.source_title="唐诗三百首",.match_length=3}};
 const auto ranked=Ranker{}.Rank({.committed_text="君不见"},candidates,1);
 ASSERT_EQ(ranked.size(),1u);EXPECT_EQ(ranked[0].text,"黄河之水天上来，");
}
TEST(Ranker, ContextualDiversityDoesNotDisplaceLongerMatches) {
 const std::vector<Suggestion> candidates{
  {.text="完成甲",.kind=LocalSuggestionKind::kBaseNGram,.match_length=4,.is_phrase_completion=true},
  {.text="完成乙",.kind=LocalSuggestionKind::kBaseNGram,.match_length=4,.is_phrase_completion=true},
  {.text="完成丙",.kind=LocalSuggestionKind::kBaseNGram,.match_length=4,.is_phrase_completion=true},
  {.text="泛泛续写",.kind=LocalSuggestionKind::kBaseNGram,.match_length=2}};
 const auto ranked=Ranker{}.Rank({.committed_text="较长上下文"},candidates,3);
 ASSERT_EQ(ranked.size(),3u);for(const auto& suggestion:ranked)EXPECT_EQ(suggestion.match_length,4u);
}
TEST(Ranker, DuplicateRetainsStrongerMatchProvenance) {
 const std::vector<Suggestion> candidates{
  {.text="黄河之水天上来",.kind=LocalSuggestionKind::kBaseNGram,.match_length=2,.is_phrase_completion=true},
  {.text="黄河之水天上来",.kind=LocalSuggestionKind::kQuotation,.source_title="唐诗三百首",.match_length=3}};
 const auto ranked=Ranker{}.Rank({.committed_text="君不见"},candidates,3);
 ASSERT_EQ(ranked.size(),1u);EXPECT_EQ(ranked[0].kind,LocalSuggestionKind::kQuotation);EXPECT_EQ(ranked[0].source_title,"唐诗三百首");
}
}  // namespace
}  // namespace zhixu::predict
