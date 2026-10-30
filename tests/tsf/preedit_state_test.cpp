#include <gtest/gtest.h>

#include "tsf/preedit_state.h"

namespace zhixu::tsf {
namespace {

TEST(PreeditState, StartsAndUpdatesCompositionForNonemptyPreedit) {
  PreeditState state;

  EXPECT_EQ(state.Next("ni", false), PreeditOperation::Update);
  state.Apply("ni");
  EXPECT_TRUE(state.active());
  EXPECT_EQ(state.text(), "ni");
  EXPECT_EQ(state.Next("nih", false), PreeditOperation::Update);
}

TEST(PreeditState, ClearsCompositionWhenTheEngineHasNoPreedit) {
  PreeditState state;
  state.Apply("ni");

  EXPECT_EQ(state.Next("", false), PreeditOperation::Clear);
  state.Apply("");
  EXPECT_FALSE(state.active());
}

TEST(PreeditState, CommitsByReplacingActiveComposition) {
  PreeditState state;
  state.Apply("ni");

  EXPECT_EQ(state.Next("", true), PreeditOperation::Commit);
}

}  // namespace
}  // namespace zhixu::tsf
