#include <gtest/gtest.h>

#include "goober/base/files.h"
#include "goober/host/operator_tokens.h"

using namespace goober::host;
namespace fs = std::filesystem;

TEST(OperatorTokens, CreateFindListRevoke) {
  fs::path storage = fs::path(GOOBER_TEST_TMP) / "tokens";
  fs::remove_all(storage);
  std::string err;
  EXPECT_FALSE(create_operator_token(storage, "Bad Name", 1, &err));
  auto a = create_operator_token(storage, "claude-code", 1000, &err);
  ASSERT_TRUE(a) << err;
  auto b = create_operator_token(storage, "ci", 2000, &err);
  ASSERT_TRUE(b);
  EXPECT_EQ(a->substr(0, 4), "gop_");
  EXPECT_EQ(a->size(), 68u);

  auto found = find_operator_token(storage, *a);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->name, "claude-code");
  EXPECT_EQ(found->roles, std::vector<std::string>{"host:owner"});
  EXPECT_FALSE(find_operator_token(storage, "gop_" + std::string(64, '0')));
  EXPECT_FALSE(find_operator_token(storage, "../../etc/passwd"));
  // Only the hash is on disk.
  for (const auto& f : goober::base::list_files(storage)) {
    EXPECT_EQ(goober::base::read_file(storage / f)->find(*a), std::string::npos);
  }

  EXPECT_EQ(list_operator_tokens(storage).size(), 2u);
  EXPECT_EQ(revoke_operator_tokens(storage, "claude-code"), 1);
  EXPECT_FALSE(find_operator_token(storage, *a));
  auto left = list_operator_tokens(storage);
  ASSERT_EQ(left.size(), 1u);
  EXPECT_EQ(revoke_operator_tokens(storage, left[0].id), 1);
  EXPECT_TRUE(list_operator_tokens(storage).empty());
}
