#include "predict/prediction/prediction_coordinator.h"
#include "predict/prediction/retriever.h"
#include "predict/imepack/index_builder.h"
#include "predict/imepack/routing_index.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace zhixu::predict {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              (L"zhixu-retriever-" +
               std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()))) {}
  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(Retriever, RecallsExactContinuationFromInstalledPackageIndex) {
  TemporaryDirectory temporary;
  const auto index_directory = temporary.path() / L"indexes";
  std::filesystem::create_directories(index_directory);
  std::ofstream index(index_directory / L"continuation.jsonl", std::ios::binary);
  index << R"({"id":"acceptance.daoyun","trigger":"道韵","normalized_trigger":"道韵","text":"法则","weight":1.0,"tags":["sensitive"]})"
        << '\n';
  index.close();

  Retriever retriever({temporary.path()});
  const auto results = retriever.Recall(PredictionContext{.committed_text = "道韵"}, 3);

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results.front().text, "法则");
  EXPECT_EQ(results.front().kind, LocalSuggestionKind::kExactContinuation);
  EXPECT_EQ(results.front().entry_id, "acceptance.daoyun");
  EXPECT_EQ(results.front().tags, (std::vector<std::string>{"sensitive"}));
}

TEST(Retriever, RecallsOrderedQuotationWithProvenance) {
  TemporaryDirectory temporary;
  const auto index_directory = temporary.path() / L"indexes";
  std::filesystem::create_directories(index_directory);
  std::ofstream index(index_directory / L"quotation.jsonl", std::ios::binary);
  index << R"({"id":"acceptance.quotation","match":"道可道","text":"非常道","weight":1.0,"work":"道德经","author":"老子","source_context":"segment:1","tags":["acceptance"]})"
        << '\n';
  index.close();

  Retriever retriever({temporary.path()});
  const auto results = retriever.Recall(PredictionContext{.committed_text = "道可道"}, 3);

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results.front().text, "非常道");
  EXPECT_EQ(results.front().kind, LocalSuggestionKind::kQuotation);
  EXPECT_EQ(results.front().source_title, "道德经");
  EXPECT_EQ(results.front().author, "老子");
  EXPECT_EQ(results.front().source_context, "segment:1");
}

TEST(Retriever, BuildsDiskIndexAndCompletesQuotationPrefixWithChapter) {
  TemporaryDirectory temporary;
  std::filesystem::create_directories(temporary.path());
  std::ofstream(temporary.path() / L"manifest.json") << R"({"entries":{"path":"entries.jsonl"}})";
  std::ofstream entries(temporary.path() / L"entries.jsonl", std::ios::binary);
  entries << R"({"id":"q1","type":"quotation","segments":["道可道","非常道"],"work":"道德经","author":"老子","chapter":"第一章","tags":[]})" << '\n';
  entries << R"({"id":"q2","type":"quotation","segments":["天地不仁","以万物为刍狗"],"work":"道德经","author":"老子","chapter":"第五章","tags":[]})" << '\n';
  entries.close();
  ASSERT_TRUE(imepack::BuildPackageIndex(temporary.path()));
  EXPECT_TRUE(std::filesystem::exists(temporary.path() / L"indexes" / L"lookup.sqlite"));
  Retriever retriever({temporary.path()});
  const auto results = retriever.Recall({.committed_text = "他轻声说道，天地不"}, 20);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results.front().text, "仁");
  EXPECT_EQ(results.front().source_context, "第五章");
}

TEST(Retriever, UsesSceneKeywordsForUnlistedSentenceAndDoesNotStopBeforeRanking) {
  TemporaryDirectory temporary;
  std::filesystem::create_directories(temporary.path());
  std::ofstream(temporary.path() / L"manifest.json") << R"({"entries":{"path":"entries.jsonl"}})";
  std::ofstream entries(temporary.path() / L"entries.jsonl", std::ios::binary);
  entries << R"({"id":"c1","type":"continuation","trigger":"昏暗","completions":[{"text":"黯淡无光","weight":1.0}],"keywords":["暮色","大地"],"tags":[]})" << '\n';
  entries.close();
  ASSERT_TRUE(imepack::BuildPackageIndex(temporary.path()));
  const auto results = Retriever({temporary.path()}).Recall({.committed_text = "暮色笼罩着大地"}, 20);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results.front().text, "黯淡无光");
  EXPECT_EQ(results.front().kind, LocalSuggestionKind::kBaseNGram);
}

TEST(Retriever, DictionaryPrefixHasOrdinaryProvenance) {
 TemporaryDirectory temporary;std::filesystem::create_directories(temporary.path());
 std::ofstream(temporary.path()/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
 std::ofstream(temporary.path()/L"entries.jsonl")<<R"({"id":"phrase1","type":"phrase","text":"黯淡无光","pinyin":"an dan wu guang","weight":1.0,"tags":[]})";
 ASSERT_TRUE(imepack::BuildPackageIndex(temporary.path()));
 const auto results=Retriever({temporary.path()}).Recall({.committed_text="黯淡无"},3);
 ASSERT_FALSE(results.empty());EXPECT_EQ(results.front().text,"光");EXPECT_EQ(results.front().kind,LocalSuggestionKind::kBaseNGram);
}

TEST(RoutingIndex, RoutesAcrossVolumesAndFallsBackForNewVersions) {
 TemporaryDirectory temporary;std::vector<std::filesystem::path> roots;
 for(const auto& id:{L"com.zhixu.first",L"com.zhixu.last"}){
  const auto root=temporary.path()/id/L"1.0.0";std::filesystem::create_directories(root);
  std::ofstream(root/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
  std::ofstream out(root/L"entries.jsonl");
  if(roots.empty())out<<R"({"id":"one","type":"quotation","segments":["山水空蒙","天地悠悠"],"work":"甲","author":"甲","tags":[]})";
  else out<<R"({"id":"two","type":"quotation","segments":["云山苍苍","江水泱泱"],"work":"乙","author":"乙","tags":[]})";
  out.close();ASSERT_TRUE(imepack::BuildPackageIndex(root));roots.push_back(root);
 }
 ASSERT_TRUE(imepack::BuildRoutingIndex(roots));
 const auto selected=imepack::RoutePackageRoots(roots,{.committed_text="云山苍"});
 ASSERT_EQ(selected.size(),1u);EXPECT_EQ(selected[0],roots[1]);
 const auto disabled=imepack::RoutePackageRoots({roots[0]},{.committed_text="云山苍"});EXPECT_TRUE(disabled.empty());
 const auto new_version=temporary.path()/L"com.zhixu.last"/L"2.0.0";
 const auto fallback=imepack::RoutePackageRoots({new_version},{.committed_text="云山苍"});ASSERT_EQ(fallback.size(),1u);EXPECT_EQ(fallback[0],new_version);
}

TEST(RoutingIndex, MatchesTraditionalWritingContext) {
 TemporaryDirectory temporary;const auto root=temporary.path()/L"com.zhixu.writing"/L"1.0.0";std::filesystem::create_directories(root);
 std::ofstream(root/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
 std::ofstream(root/L"entries.jsonl")<<R"({"id":"s1","type":"continuation","trigger":"灵气","completions":[{"text":"流转不息","weight":1.0}],"tags":[]})";
 ASSERT_TRUE(imepack::BuildPackageIndex(root));ASSERT_TRUE(imepack::BuildRoutingIndex({root}));
 const auto suggestions=Retriever({root}).Recall({.committed_text="体内靈氣"},3);ASSERT_FALSE(suggestions.empty());EXPECT_EQ(suggestions[0].text,"流转不息");
}
TEST(Retriever, MarksEditionAttributionInsteadOfAssumingOriginalAuthorship) {
 TemporaryDirectory temporary;std::filesystem::create_directories(temporary.path());
 std::ofstream(temporary.path()/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
 std::ofstream(temporary.path()/L"entries.jsonl")<<R"({"id":"edition","type":"quotation","segments":["山海之经","异兽奇禽"],"work":"山海经","author":"郭璞","source_url":"https://github.com/kanripo/KR3l0090/blob/fixed/text.txt","tags":[]})";
 ASSERT_TRUE(imepack::BuildPackageIndex(temporary.path()));const auto results=Retriever({temporary.path()}).Recall({.committed_text="山海之经"},3);
 ASSERT_FALSE(results.empty());EXPECT_EQ(results[0].author,"底本署名：郭璞");EXPECT_EQ(results[0].source_title,"山海经（底本）");
}
TEST(Retriever, SceneFallbackUsesRecentCueAndSuppressesDistantCue) {
 TemporaryDirectory temporary;std::filesystem::create_directories(temporary.path());
 std::ofstream(temporary.path()/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
 std::ofstream(temporary.path()/L"entries.jsonl")<<R"({"id":"old","type":"continuation","trigger":"暮色","completions":[{"text":"一片寂静","weight":1.0}],"tags":[],"keywords":["暮色"]})" "\n"
 <<R"({"id":"new","type":"continuation","trigger":"剑光","completions":[{"text":"破空而至","weight":1.0}],"tags":[],"keywords":["剑光"]})";
 ASSERT_TRUE(imepack::BuildPackageIndex(temporary.path()));
 const auto results=Retriever({temporary.path()}).Recall({.committed_text="暮色中他挥剑，剑光从云后忽然"},3);
 ASSERT_FALSE(results.empty());EXPECT_EQ(results[0].text,"破空而至");
 std::string distant="暮色";for(int i=0;i<40;++i)distant+="甲";
 EXPECT_TRUE(Retriever({temporary.path()}).Recall({.committed_text=distant},3).empty());
}
TEST(Retriever, DoesNotDuplicateExistingAsciiQuotationPunctuation) {
 TemporaryDirectory temporary;std::filesystem::create_directories(temporary.path());
 std::ofstream(temporary.path()/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
 std::ofstream(temporary.path()/L"entries.jsonl")<<R"({"id":"q","type":"quotation","segments":["道可道，","非常道。"],"work":"道德经","author":"老子","tags":[]})";
 ASSERT_TRUE(imepack::BuildPackageIndex(temporary.path()));
 const auto results=Retriever({temporary.path()}).Recall({.committed_text="道可道,"},3);
 ASSERT_FALSE(results.empty());EXPECT_EQ(results[0].text,"非常道。");
}
TEST(RoutingIndex, TwoCharacterPhrasePrefixRoutesAndWinsOverQuotation) {
 TemporaryDirectory temporary;std::vector<std::filesystem::path> roots;
 for(const auto& id:{L"com.zhixu.classics",L"com.zhixu.words"}){
  auto root=temporary.path()/id/L"1.0.0";std::filesystem::create_directories(root);
  std::ofstream(root/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
  std::ofstream out(root/L"entries.jsonl");
  if(roots.empty())out<<R"({"id":"q","type":"quotation","segments":["罄竹难书其罪大。","从来拓土与开疆。"],"work":"入山歌","author":"原文署名","tags":[]})";
  else out<<R"({"id":"w","type":"phrase","text":"罄竹难书","pinyin":"qing zhu nan shu","weight":0.5,"tags":[]})";
  out.close();ASSERT_TRUE(imepack::BuildPackageIndex(root));roots.push_back(root);
 }
 ASSERT_TRUE(imepack::BuildRoutingIndex(roots));
 PersonalizationStore learning({},false);PredictionCoordinator coordinator(roots,learning);
 for(const auto& context:{"罄竹","罪行罄竹","罄竹难","其罪罄竹難"}){
  const auto results=coordinator.Predict({.committed_text=context},3);ASSERT_FALSE(results.empty())<<context;
  EXPECT_EQ(results[0].text,std::string(context).ends_with("难") || std::string(context).ends_with("難")?"书":"难书");
  EXPECT_EQ(results[0].source_title,"成语词语");
 }
 const auto completed=Retriever(roots).Recall({.committed_text="罄竹难书"},3);
 for(const auto& candidate:completed)EXPECT_NE(candidate.text,"罄竹难书");
}
TEST(Retriever, QuotationPrefixRecallsEnoughDistinctResultsForThirtySuggestions) {
 TemporaryDirectory temporary;std::filesystem::create_directories(temporary.path());
 std::ofstream(temporary.path()/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
 std::ofstream entries(temporary.path()/L"entries.jsonl",std::ios::binary);
 for(int i=0;i<40;++i)entries<<"{\"id\":\"quote"<<i<<"\",\"type\":\"quotation\",\"segments\":[\"君不见甲"<<i<<"\",\"下一句\"],\"work\":\"某诗集\",\"author\":\"作者\",\"tags\":[]}\n";
 entries.close();ASSERT_TRUE(imepack::BuildPackageIndex(temporary.path()));
 const auto results=Retriever({temporary.path()}).Recall({.committed_text="君不见"},30);
 EXPECT_GE(results.size(),30u);
}
TEST(Retriever, CuratedQuotationSurvivesPrefixRecallAndShortSuffixPhrases) {
 TemporaryDirectory temporary;std::filesystem::create_directories(temporary.path());
 std::ofstream(temporary.path()/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
 std::ofstream entries(temporary.path()/L"entries.jsonl",std::ios::binary);
 for(int i=0;i<70;++i)entries<<"{\"id\":\"rare"<<i<<"\",\"type\":\"quotation\",\"segments\":[\"君不见一"<<i<<"\",\"下一句\"],\"work\":\"某诗集\",\"author\":\"作者\",\"tags\":[]}\n";
 entries<<R"({"id":"classic","type":"quotation","segments":["君不见黄河之水天上来，","奔流到海不复回。"],"work":"唐诗三百首","author":"李白","tags":[]})"<<'\n';
 entries<<R"({"id":"short","type":"phrase","text":"不见一点踪影","weight":1.0,"tags":[]})"<<'\n';
 entries.close();ASSERT_TRUE(imepack::BuildPackageIndex(temporary.path()));
 PersonalizationStore learning({},false);PredictionCoordinator coordinator({temporary.path()},learning);
 const auto results=coordinator.Predict({.committed_text="君不见"},30);
 ASSERT_EQ(results.size(),30u);EXPECT_EQ(results[0].text,"黄河之水天上来，");EXPECT_EQ(results[0].source_title,"唐诗三百首");EXPECT_EQ(results[0].author,"李白");
}
TEST(Retriever, AmbiguousCuratedQuotationPreservesCollectionAndSegmentOrder) {
 TemporaryDirectory temporary;std::filesystem::create_directories(temporary.path());
 std::ofstream(temporary.path()/L"manifest.json")<<R"({"entries":{"path":"entries.jsonl"}})";
 std::ofstream entries(temporary.path()/L"entries.jsonl",std::ios::binary);
 entries<<R"({"id":"opening","type":"quotation","segments":["君不见，","黄河之水天上来，","奔流到海不复回。","君不见，","高堂明镜悲白发，","朝如青丝暮成雪。"],"work":"唐诗三百首","author":"李白","chapter":"将进酒","tags":[]})"<<'\n';
 entries<<R"({"id":"later","type":"quotation","segments":["车辚辚，","马萧萧，","君不见，","青海头，","古来白骨无人收。"],"work":"唐诗三百首","author":"杜甫","chapter":"兵车行","tags":[]})"<<'\n';
 entries.close();ASSERT_TRUE(imepack::BuildPackageIndex(temporary.path()));
 PersonalizationStore learning({},false);PredictionCoordinator coordinator({temporary.path()},learning);
 const auto results=coordinator.Predict({.committed_text="君不见"},30);
 ASSERT_EQ(results.size(),3u);
 EXPECT_EQ(results[0].text,"，黄河之水天上来，");
 EXPECT_EQ(results[1].text,"，高堂明镜悲白发，");
 EXPECT_EQ(results[2].text,"，青海头，");
 EXPECT_EQ(results[0].source_context,"将进酒");
 EXPECT_EQ(results[0].source_title,"唐诗三百首");
}
}  // namespace
}  // namespace zhixu::predict
