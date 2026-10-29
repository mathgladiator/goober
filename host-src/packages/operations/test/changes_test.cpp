#include <gtest/gtest.h>

#include "goober/base/files.h"
#include "goober/operations/changes.h"

using namespace goober;
using operations::ChangeLog;
using wal::FileChange;
namespace fs = std::filesystem;

namespace {

class ChangesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "changes";
    fs::remove_all(root);
    w = std::make_unique<wal::HostWal>(root);
    log = std::make_unique<ChangeLog>(root / "operations" / "changes.db", root);
    w->set_observer([this](const wal::Bundle& b) { log->record(b, actor, request_id, 1000); });
  }
  bool commit(wal::Bundle b) {
    std::string err;
    return w->commit(b, &err);
  }
  fs::path root;
  std::unique_ptr<wal::HostWal> w;
  std::unique_ptr<ChangeLog> log;
  std::string actor = "oauth:boss@example.com:grok", request_id = "r1";
};

}  // namespace

TEST(ClassifyPath, EachWriterIsNamed) {
  auto k = operations::classify_path("content/kcmeatup/places/index.md");
  EXPECT_EQ(k.kind, "content");
  EXPECT_EQ(k.tenant, "kcmeatup");
  EXPECT_EQ(k.target, "places/index.md");
  EXPECT_EQ(operations::classify_path("content/kcmeatup/_partials/nav.html").kind, "partial");
  EXPECT_EQ(operations::classify_path("content/kcmeatup/_partials/_versions/nav/1.html").kind, "");
  EXPECT_EQ(operations::classify_path("content/_host/_templates/page.html").kind, "template");
  auto box = operations::classify_path("boxes/kc/versions/abc/manifest.json");
  EXPECT_EQ(box.kind, "box");
  EXPECT_EQ(box.version, "abc");
  EXPECT_TRUE(operations::classify_path("boxes/kc/active").active);
  auto schema = operations::classify_path("schemas/places2.schema.json");
  EXPECT_EQ(schema.kind, "schema");
  EXPECT_EQ(schema.target, "places2");
  auto a = operations::classify_path("objects/kcmeatup/attachments/77");
  EXPECT_EQ(a.kind, "attachment");
  EXPECT_EQ(a.target, "77");
  EXPECT_EQ(operations::classify_path("keys/x").kind, "other");
}

TEST_F(ChangesTest, ContentKeepsBeforeAndAfterWithWhoDidIt) {
  ASSERT_TRUE(commit({{FileChange::Kind::kWrite, "content/kc/index.md", "# Hi\n"}}));
  request_id = "r2";
  ASSERT_TRUE(commit({{FileChange::Kind::kWrite, "content/kc/index.md", "# Hello\n"}}));
  ASSERT_TRUE(commit({{FileChange::Kind::kDelete, "content/kc/index.md", ""}}));
  auto rows = log->list("", "", 0, 10);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[2].as_object().at("action"), "create");
  EXPECT_EQ(rows[1].as_object().at("action"), "write");
  EXPECT_EQ(rows[0].as_object().at("action"), "delete");
  const auto& write = rows[1].as_object();
  EXPECT_EQ(std::string(write.at("actor").as_string()), actor);
  EXPECT_EQ(write.at("request_id"), "r2");
  EXPECT_EQ(write.at("tenant"), "kc");
  EXPECT_EQ(write.at("bytes_before").as_int64(), 5);
  EXPECT_TRUE(write.at("has_text").as_bool());
  EXPECT_FALSE(write.contains("before"));
  auto one = log->get(write.at("id").as_int64());
  ASSERT_TRUE(one);
  EXPECT_EQ(one->at("before"), "# Hi\n");
  EXPECT_EQ(one->at("after"), "# Hello\n");
  EXPECT_EQ(log->list("content", "other", 0, 10).size(), 0u);
  EXPECT_EQ(log->list("", "", rows[1].as_object().at("id").as_int64(), 10).size(), 1u);
}

TEST_F(ChangesTest, LargeTextsAreMeasuredNotKept) {
  std::string big(ChangeLog::kTextMaxBytes + 1, 'x');
  ASSERT_TRUE(commit({{FileChange::Kind::kWrite, "content/kc/big.html", big}}));
  auto rows = log->list("", "", 0, 10);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FALSE(rows[0].as_object().at("has_text").as_bool());
  EXPECT_EQ(rows[0].as_object().at("bytes_after").as_int64(), int64_t(big.size()));
  EXPECT_TRUE(rows[0].as_object().at("hash_after").is_string());
}

TEST_F(ChangesTest, AnUploadBundleIsOneDeployRow) {
  ASSERT_TRUE(commit({{FileChange::Kind::kWrite, "boxes/kc/versions/v1/manifest.json", "{}"},
                      {FileChange::Kind::kWrite, "boxes/kc/versions/v1/box.wasm", "wasm"},
                      {FileChange::Kind::kWrite, "boxes/kc/active", "v1"}}));
  ASSERT_TRUE(commit({{FileChange::Kind::kWrite, "boxes/kc/versions/v2/manifest.json", "{}"},
                      {FileChange::Kind::kWrite, "boxes/kc/active", "v2"}}));
  // A promotion back: only the active file.
  ASSERT_TRUE(commit({{FileChange::Kind::kWrite, "boxes/kc/active", "v1"}}));
  auto rows = log->list("deploy", "", 0, 10);
  ASSERT_EQ(rows.size(), 3u);
  const auto& back = rows[0].as_object().at("detail").as_object();
  EXPECT_EQ(back.at("version"), "v1");
  EXPECT_EQ(back.at("previous"), "v2");
  EXPECT_FALSE(back.at("uploaded").as_bool());
  const auto& first = rows[2].as_object().at("detail").as_object();
  EXPECT_FALSE(first.contains("previous"));
  EXPECT_TRUE(first.at("uploaded").as_bool());
  EXPECT_EQ(rows[2].as_object().at("target"), "kc");
  EXPECT_EQ(log->list("", "", 0, 10).size(), 3u);
}

TEST_F(ChangesTest, ActionsListTheChangesTheyCaused) {
  ASSERT_TRUE(commit({{FileChange::Kind::kWrite, "schemas/kc1.schema.json", "{}"}}));
  log->record_action({1000, "token:laptop", "schema_put", "r1", {{"name", "kc1"}}, false});
  log->record_action({1001, "token:laptop", "db_query", "r9", {{"tenant", "kc"}}, false});
  auto all = log->actions(false, 0, 10);
  ASSERT_EQ(all.size(), 2u);
  auto changed = log->actions(true, 0, 10);
  ASSERT_EQ(changed.size(), 1u);
  EXPECT_EQ(changed[0].as_object().at("tool"), "schema_put");
  EXPECT_EQ(changed[0].as_object().at("changes").as_array().size(), 1u);
  EXPECT_EQ(changed[0].as_object().at("args").as_object().at("name"), "kc1");
}
