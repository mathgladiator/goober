#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "goober/base/files.h"
#include "goober/base/time.h"
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
         R"(":{"get":{"name":")" + route + R"(","public":true}}}}})";
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

// Mail addresses are claimed once per domain across boxes (038).
TEST_F(RegistryTest, MailAddressesAreClaimedOnce) {
  auto mail = [](const std::string& box, const std::string& domain, const std::string& address, const std::string& route) {
    return R"({"box":")" + box + R"(","domains":[")" + domain + R"("],"routes":{"mail":{")" + address +
           R"(":{"receive":{"name":")" + route + R"(","public":true}}}}})";
  };
  std::string err;
  ASSERT_TRUE(reg->upload(mail("desk", "acme.example", "support@", "desk_support"), module_for({"desk_support"}), &err)) << err;
  EXPECT_FALSE(reg->upload(mail("other", "acme.example", "support@acme.example", "other_support"),
                           module_for({"other_support"}), &err));
  EXPECT_NE(err.find("already claimed by box desk"), std::string::npos) << err;
  // Another domain is another address; a different local part is too.
  EXPECT_TRUE(reg->upload(mail("other", "beef.example", "support@", "other_support"), module_for({"other_support"}), &err)) << err;
  EXPECT_TRUE(reg->upload(mail("third", "acme.example", "orders@", "third_orders"), module_for({"third_orders"}), &err)) << err;
  // One catch-all per domain.
  ASSERT_TRUE(reg->upload(mail("inbox", "*", "*@", "inbox_all"), module_for({"inbox_all"}), &err)) << err;
  EXPECT_FALSE(reg->upload(mail("inbox2", "acme.example", "*@", "inbox2_all"), module_for({"inbox2_all"}), &err));
  // A route without its export is refused like any other.
  EXPECT_FALSE(reg->upload(mail("x", "x.example", "x@", "x_route"), module_for({}), &err));
}

// Versions no longer live are archived, then deleted with a tombstone (045 § 3.1).
TEST_F(RegistryTest, OldVersionsAreArchivedThenDeleted) {
  constexpr int64_t kDay = 86400000;
  reg->set_retention({30 * kDay, 400 * kDay, 2});
  std::string err;
  std::vector<std::string> v;
  for (int i = 1; i <= 6; ++i) {
    auto r = reg->upload(manifest_for("hello", "*", "/p" + std::to_string(i), "say_hello"), module_for({"say_hello"}), &err);
    ASSERT_TRUE(r) << err;
    v.push_back(r->version);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  auto dark = reg->store(manifest_for("hello", "*", "/dark", "say_hello"), module_for({"say_hello"}), "", &err);
  ASSERT_TRUE(dark) << err;
  // v[2] is named by a kept change set.
  std::string protect = v[2];
  // The host's reasons are read once per pruning for every box (P4), not once per box.
  ASSERT_TRUE(reg->upload(manifest_for("other", "*", "/other", "say_other"), module_for({"say_other"}), &err)) << err;
  int protector_calls = 0;
  reg->set_protector([protect, &protector_calls] {
    ++protector_calls;
    return boxes::BoxRegistry::Protected{{"hello", {{protect, "change set 7"}}}, {"other", {}}};
  });
  auto state = [&](const std::string& version) {
    for (const auto& i : reg->versions("hello")) {
      if (i.version == version) return i.state + (i.kept.empty() ? "" : "+kept");
    }
    return std::string("missing");
  };
  EXPECT_EQ(state(v[5]), "live");
  EXPECT_EQ(state(v[4]), "stored+kept");  // the newest two that were live
  EXPECT_EQ(state(v[3]), "stored+kept");
  EXPECT_EQ(state(v[2]), "stored+kept");  // the change set's
  EXPECT_EQ(state(v[0]), "stored");

  int64_t now = base::now_unix_ms();
  protector_calls = 0;
  auto none = reg->prune(now);
  EXPECT_TRUE(none.archived.empty() && none.deleted.empty());
  EXPECT_EQ(protector_calls, 1);

  auto month = reg->prune(now + 31 * kDay);
  EXPECT_EQ(month.archived.size(), 3u);  // v[0], v[1], and the dark one
  EXPECT_EQ(state(v[0]), "archived");
  EXPECT_EQ(state(dark->version), "archived");
  // Archived: kept for the record, but not tried or promoted until restored.
  EXPECT_FALSE(reg->load("hello", v[0], &err));
  EXPECT_NE(err.find("version_restore"), std::string::npos) << err;
  EXPECT_FALSE(reg->promote("hello", v[0], &err));
  EXPECT_TRUE(reg->files("hello", v[0]));
  ASSERT_TRUE(reg->restore("hello", v[1], &err)) << err;
  EXPECT_EQ(state(v[1]), "stored");
  EXPECT_TRUE(reg->load("hello", v[1], &err)) << err;
  EXPECT_FALSE(reg->restore("hello", v[1], &err));  // not archived now

  // A disabled box's version is kept however long it is off.
  ASSERT_TRUE(reg->disable("hello", "maintenance", &err)) << err;
  auto later = reg->prune(now + 401 * kDay, [](const std::string&, const std::string&) {
    return boost::json::object{{"uploaded_by", "token:ci"}};
  });
  // v[0], v[1] (its time started again at restore, still past 400 days by then), v[3], and the
  // dark one go; v[5] (disabled), v[4] (now among the newest two), and v[2] (the change set) stay.
  EXPECT_EQ(later.deleted.size(), 4u);
  EXPECT_EQ(state(v[0]), "deleted");
  EXPECT_EQ(state(v[3]), "deleted");
  EXPECT_EQ(state(v[5]), "stored+kept");
  EXPECT_EQ(state(v[4]), "stored+kept");
  EXPECT_EQ(state(v[2]), "stored+kept");
  EXPECT_FALSE(fs::exists(root / "boxes/hello/versions" / v[0]));
  auto t = reg->tombstone("hello", v[0]);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->at("module_sha256").as_string().size(), 64u);
  EXPECT_EQ(t->at("uploaded_by"), "token:ci");
  EXPECT_EQ(t->at("live").as_array().size(), 1u);
  EXPECT_FALSE(reg->restore("hello", v[0], &err));
  EXPECT_NE(err.find("deleted"), std::string::npos) << err;

  // Uploading the same bytes again stores it again; the tombstone stays for the record.
  ASSERT_TRUE(reg->upload(manifest_for("hello", "*", "/p1", "say_hello"), module_for({"say_hello"}), &err)) << err;
  EXPECT_EQ(state(v[0]), "live");
  EXPECT_TRUE(reg->tombstone("hello", v[0]));
}

// A version stored before records were kept starts its inactive time at the first pruning.
TEST_F(RegistryTest, VersionsWithoutARecordStartTheirTimeAtTheFirstPruning) {
  constexpr int64_t kDay = 86400000;
  reg->set_retention({30 * kDay, 400 * kDay, 1});
  std::string err;
  auto a = reg->upload(manifest_for("hello", "*", "/a", "say_hello"), module_for({"say_hello"}), &err);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  auto b = reg->upload(manifest_for("hello", "*", "/b", "say_hello"), module_for({"say_hello"}), &err);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  auto c = reg->upload(manifest_for("hello", "*", "/c", "say_hello"), module_for({"say_hello"}), &err);
  ASSERT_TRUE(a && b && c) << err;
  for (const auto& v : {a->version, b->version}) fs::remove(root / "boxes/hello/versions" / v / "life.json");
  int64_t now = base::now_unix_ms() + 1000 * kDay;
  auto first = reg->prune(now);
  EXPECT_TRUE(first.archived.empty() && first.deleted.empty());
  auto later = reg->prune(now + 31 * kDay);
  // b, the newest stored, may have been live: kept; a is archived.
  ASSERT_EQ(later.archived.size(), 1u);
  EXPECT_EQ(later.archived[0].second, a->version);
}
