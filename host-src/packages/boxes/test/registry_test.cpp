#include <gtest/gtest.h>

#include "goober/base/files.h"
#include "goober/boxes/registry.h"

using namespace goober;
namespace fs = std::filesystem;

namespace {
runtime::wasm::Engine& engine() {
  static runtime::wasm::Engine e;
  return e;
}

std::string module_for(std::vector<std::string> routes) {
  std::string wat = R"((module
  (import "goober" "goober_respond" (func $respond (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "text/plain"))";
  int i = 0;
  for (const auto& r : routes) {
    wat += "\n  (func (export \"goober_route_" + r + "\") (result i32) (drop (call $respond (i32.const 200) "
           "(i32.const 0) (i32.const 10) (i32.const 0) (i32.const " + std::to_string(++i) + "))) (i32.const 0))";
  }
  return wat + ")";
}

std::string manifest_for(const std::string& box, const std::string& domain, const std::string& path,
                         const std::string& route) {
  return R"({"box":")" + box + R"(","domains":[")" + domain + R"("],"routes":{"http":{")" + path +
         R"(":{"get":{"name":")" + route + R"("}}}}})";
}
}  // namespace

class RegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "registry";
    fs::remove_all(root);
    w = std::make_unique<wal::HostWal>(root);
    reg = std::make_unique<boxes::BoxRegistry>(*w, engine());
  }
  fs::path root;
  std::unique_ptr<wal::HostWal> w;
  std::unique_ptr<boxes::BoxRegistry> reg;
};

TEST_F(RegistryTest, UploadActivatesAndPersists) {
  std::string err;
  auto r = reg->upload(manifest_for("hello", "*", "/hello", "say_hello"), module_for({"say_hello"}), &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(r->version.size(), 24u);
  auto set = reg->active();
  auto m = set->table_for("example.com").match({"hello"}, "get");
  ASSERT_TRUE(m.value);
  EXPECT_EQ(m.value->route->name, "say_hello");
  EXPECT_TRUE(fs::exists(root / "boxes/hello/versions" / r->version / "box.wasm"));

  boxes::BoxRegistry again(*w, engine());
  EXPECT_TRUE(again.load_all().empty());
  EXPECT_TRUE(again.active()->table_for("x").match({"hello"}, "get").value);
}

TEST_F(RegistryTest, RefusesConflictsAcrossBoxes) {
  std::string err;
  ASSERT_TRUE(reg->upload(manifest_for("one", "*", "/a/{x}", "one_a"), module_for({"one_a"}), &err)) << err;
  EXPECT_FALSE(reg->upload(manifest_for("two", "*", "/a/{y : int64}", "two_a"), module_for({"two_a"}), &err));
  EXPECT_NE(err.find("conflicts"), std::string::npos) << err;
  err.clear();
  EXPECT_FALSE(reg->upload(manifest_for("two", "a.com", "/a/{y}", "two_a"), module_for({"two_a"}), &err));
  err.clear();
  EXPECT_FALSE(reg->upload(manifest_for("two", "*", "/b", "one_a"), module_for({"one_a"}), &err));
  EXPECT_NE(err.find("already used"), std::string::npos) << err;
}

TEST_F(RegistryTest, DomainsSeparateRoutes) {
  std::string err;
  ASSERT_TRUE(reg->upload(manifest_for("one", "a.com", "/x", "one_x"), module_for({"one_x"}), &err)) << err;
  ASSERT_TRUE(reg->upload(manifest_for("two", "b.com", "/x", "two_x"), module_for({"two_x"}), &err)) << err;
  auto set = reg->active();
  EXPECT_EQ(set->table_for("a.com").match({"x"}, "get").value->route->name, "one_x");
  EXPECT_EQ(set->table_for("b.com").match({"x"}, "get").value->route->name, "two_x");
  EXPECT_FALSE(set->table_for("c.com").match({"x"}, "get").value);
}

TEST_F(RegistryTest, NewVersionReplacesAndPromoteRollsBack) {
  std::string err;
  auto v1 = reg->upload(manifest_for("hello", "*", "/hello", "say_hello"), module_for({"say_hello"}), &err);
  auto v2 = reg->upload(manifest_for("hello", "*", "/hi", "say_hello"), module_for({"say_hello"}), &err);
  ASSERT_TRUE(v1 && v2) << err;
  EXPECT_FALSE(reg->active()->table_for("x").match({"hello"}, "get").value);
  EXPECT_EQ(reg->versions("hello").size(), 2u);
  ASSERT_TRUE(reg->promote("hello", v1->version, &err)) << err;
  EXPECT_TRUE(reg->active()->table_for("x").match({"hello"}, "get").value);
  EXPECT_FALSE(reg->promote("hello", "nope", &err));
}

TEST_F(RegistryTest, RefusesBadModules) {
  std::string err;
  EXPECT_FALSE(reg->upload(manifest_for("hello", "*", "/hello", "say_hello"), module_for({"other"}), &err));
  EXPECT_NE(err.find("goober_route_say_hello"), std::string::npos) << err;
  EXPECT_TRUE(reg->box_names().empty());
}
