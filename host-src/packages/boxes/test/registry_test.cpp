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

TEST_F(RegistryTest, ADroppedBoxLeavesOnlyTombstonesAndItsNameStaysUsable) {
  std::string err;
  auto v1 = reg->upload(manifest_for("old", "*", "/old", "old_get"), module_for({"old_get"}), &err);
  auto v2 = reg->upload(manifest_for("old", "*", "/older", "old_get"), module_for({"old_get"}), &err);
  ASSERT_TRUE(v1 && v2) << err;
  ASSERT_TRUE(reg->upload(manifest_for("keep", "*", "/keep", "keep_get"), module_for({"keep_get"}), &err)) << err;
  auto gone = reg->drop("old", boost::json::object{{"by", "panel:owner@example.com"}, {"why", "tidy"}}, &err);
  ASSERT_TRUE(gone) << err;
  EXPECT_EQ(gone->size(), 2u);
  // Out of service; the other box still serves.
  EXPECT_FALSE(reg->active()->boxes.count("old"));
  EXPECT_FALSE(reg->active()->table_for("x").match({"older"}, "get").value);
  EXPECT_TRUE(reg->active()->table_for("x").match({"keep"}, "get").value);
  // Only tombstones and the record are left.
  EXPECT_FALSE(fs::exists(root / "boxes/old/versions" / v1->version));
  EXPECT_FALSE(fs::exists(root / "boxes/old/active"));
  auto t = reg->tombstone("old", v2->version);
  ASSERT_TRUE(t);
  EXPECT_TRUE(t->at("dropped").as_bool());
  auto rec = reg->dropped("old");
  ASSERT_TRUE(rec);
  EXPECT_EQ(rec->at("why"), "tidy");
  EXPECT_EQ(rec->at("versions").as_array().size(), 2u);
  for (const auto& v : reg->versions("old")) EXPECT_EQ(v.state, "deleted");
  EXPECT_FALSE(reg->drop("old", {}, &err));
  EXPECT_NE(err.find("dropped already"), std::string::npos) << err;
  EXPECT_FALSE(reg->drop("never", {}, &err));
  // A restart doesn't bring it back.
  boxes::BoxRegistry again(*w, engine());
  again.load_all();
  EXPECT_FALSE(again.active()->boxes.count("old"));
  // Uploading the name again is a new box.
  ASSERT_TRUE(reg->upload(manifest_for("old", "*", "/new", "old_get"), module_for({"old_get"}), &err)) << err;
  EXPECT_FALSE(reg->dropped("old"));
  EXPECT_TRUE(reg->active()->table_for("x").match({"new"}, "get").value);
}

TEST_F(RegistryTest, ADisabledBoxDrops) {
  std::string err;
  ASSERT_TRUE(reg->upload(manifest_for("off", "*", "/off", "off_get"), module_for({"off_get"}), &err)) << err;
  ASSERT_TRUE(reg->disable("off", "paused", &err)) << err;
  ASSERT_TRUE(reg->drop("off", {}, &err)) << err;
  EXPECT_FALSE(reg->disabled("off"));
  EXPECT_TRUE(reg->dropped("off"));
}

TEST_F(RegistryTest, ASplitServesTheCandidateUnderItsShare) {
  std::string err;
  auto v1 = reg->upload(manifest_for("ab", "*", "/ab", "ab_get"), module_for({"ab_get"}), &err);
  ASSERT_TRUE(v1) << err;
  // The same routes, another module: a candidate stored dark.
  auto v2 = reg->store(manifest_for("ab", "*", "/ab", "ab_get"), module_for({"ab_get", "ab_get_extra_export_unused"}), "", &err);
  ASSERT_TRUE(v2) << err;
  ASSERT_TRUE(reg->split_set("ab", v2->version, 2500, {{"by", "test"}}, false, &err)) << err;
  auto set = reg->active();
  auto ref = *set->table_for("x").match({"ab"}, "get").value;
  EXPECT_EQ(ref.box->version, v1->version);  // the tables hold the live version
  auto under = set->candidate_for(ref, 2499);
  ASSERT_TRUE(under);
  EXPECT_EQ(under->box->version, v2->version);
  EXPECT_EQ(under->route->name, "ab_get");
  EXPECT_FALSE(set->candidate_for(ref, 2500));
  EXPECT_EQ(reg->split_record("ab")->at("share_bp"), 2500);
  // A restart resumes it, with the same salt.
  std::string salt = set->splits.at("ab").salt;
  boxes::BoxRegistry again(*w, engine());
  again.load_all();
  ASSERT_TRUE(again.active()->splits.count("ab"));
  EXPECT_EQ(again.active()->splits.at("ab").salt, salt);
  // The candidate is kept from archiving while it serves.
  bool kept = false;
  for (const auto& v : reg->versions("ab")) kept |= v.version == v2->version && !v.kept.empty();
  EXPECT_TRUE(kept);
  // Promoting a version ends the split.
  ASSERT_TRUE(reg->promote("ab", v2->version, &err)) << err;
  EXPECT_FALSE(reg->active()->splits.count("ab"));
  EXPECT_FALSE(reg->split_record("ab"));
}

TEST_F(RegistryTest, OnlyCompatibleVersionsOfALiveBoxSplit) {
  std::string err;
  auto v1 = reg->upload(manifest_for("ab", "*", "/ab", "ab_get"), module_for({"ab_get"}), &err);
  auto moved = reg->store(manifest_for("ab", "*", "/moved", "ab_get"), module_for({"ab_get"}), "", &err);
  ASSERT_TRUE(v1 && moved) << err;
  EXPECT_FALSE(reg->split_set("ab", moved->version, 100, {}, false, &err));
  EXPECT_NE(err.find("can't split"), std::string::npos) << err;
  EXPECT_FALSE(reg->split_set("ab", v1->version, 100, {}, false, &err));
  EXPECT_NE(err.find("live one"), std::string::npos) << err;
  EXPECT_FALSE(reg->split_set("nope", v1->version, 100, {}, false, &err));
  auto same = reg->store(manifest_for("ab", "*", "/ab", "ab_get"), module_for({"ab_get", "x"}), "", &err);
  ASSERT_TRUE(same) << err;
  ASSERT_TRUE(reg->split_set("ab", same->version, 100, {}, false, &err)) << err;
  // Disabling the box ends it.
  ASSERT_TRUE(reg->disable("ab", "pause", &err)) << err;
  EXPECT_FALSE(reg->active()->splits.count("ab"));
  EXPECT_FALSE(reg->split_record("ab"));
  EXPECT_FALSE(reg->split_end("ab", &err));
}

TEST_F(RegistryTest, ABetaVersionAnswersOnlyOnBetaDomains) {
  std::string err;
  manifest::set_domain_aliases({{"beta.example.com", "example.com"}});
  reg->set_beta_domains({"beta.example.com"});
  auto v1 = reg->upload(manifest_for("site", "example.com", "/page", "page_get"), module_for({"page_get"}), &err);
  ASSERT_TRUE(v1) << err;
  // Before a beta version, beta answers with the live one.
  EXPECT_EQ(reg->active()->table_for("beta.example.com").match({"page"}, "get").value->box->version, v1->version);
  // A beta version that adds a route.
  auto v2 = reg->store(R"({"box":"site","domains":["example.com"],"routes":{"http":{"/page":{"get":{"name":"page_get","public":true}},
      "/new":{"get":{"name":"page_new","public":true}}}}})", module_for({"page_get", "page_new"}), "", &err);
  ASSERT_TRUE(v2) << err;
  ASSERT_TRUE(reg->beta_set("site", v2->version, &err)) << err;
  auto set = reg->active();
  EXPECT_EQ(set->table_for("beta.example.com").match({"page"}, "get").value->box->version, v2->version);
  EXPECT_TRUE(set->table_for("beta.example.com").match({"new"}, "get").value);
  EXPECT_EQ(set->table_for("example.com").match({"page"}, "get").value->box->version, v1->version);
  EXPECT_FALSE(set->table_for("example.com").match({"new"}, "get").value);
  EXPECT_EQ(set->box_for("site", true)->version, v2->version);
  EXPECT_EQ(set->box_for("site", false)->version, v1->version);
  EXPECT_EQ(*reg->beta_version("site"), v2->version);
  // A box only in beta.
  auto only = reg->store(manifest_for("fresh", "example.com", "/fresh", "fresh_get"), module_for({"fresh_get"}), "", &err);
  ASSERT_TRUE(only) << err;
  ASSERT_TRUE(reg->beta_set("fresh", only->version, &err)) << err;
  EXPECT_TRUE(reg->active()->table_for("beta.example.com").match({"fresh"}, "get").value);
  EXPECT_FALSE(reg->active()->table_for("example.com").match({"fresh"}, "get").value);
  // A new live version keeps the beta slot; a restart keeps both.
  ASSERT_TRUE(reg->upload(manifest_for("other", "example.com", "/other", "other_get"), module_for({"other_get"}), &err)) << err;
  EXPECT_TRUE(reg->active()->table_for("beta.example.com").match({"new"}, "get").value);
  boxes::BoxRegistry again(*w, engine());
  again.set_beta_domains({"beta.example.com"});
  again.load_all();
  EXPECT_TRUE(again.active()->table_for("beta.example.com").match({"fresh"}, "get").value);
  // Cleared, beta follows live again.
  ASSERT_TRUE(reg->beta_clear("site", &err)) << err;
  EXPECT_FALSE(reg->active()->table_for("beta.example.com").match({"new"}, "get").value);
  manifest::set_domain_aliases({});
}

TEST_F(RegistryTest, ASplitEndedByTheRegistryKeepsItsRecordAndIsNotChangedAfter) {
  std::string err;
  auto v1 = reg->upload(manifest_for("ab", "*", "/ab", "ab_get"), module_for({"ab_get"}), &err);
  auto v2 = reg->store(manifest_for("ab", "*", "/ab", "ab_get"), module_for({"ab_get", "y"}), "", &err);
  ASSERT_TRUE(v1 && v2) << err;
  ASSERT_TRUE(reg->split_set("ab", v2->version, 100, {{"why", "try"}}, false, &err)) << err;
  // A second start is refused; a change of the running split isn't.
  EXPECT_FALSE(reg->split_set("ab", v2->version, 200, {}, false, &err));
  EXPECT_NE(err.find("split already"), std::string::npos) << err;
  ASSERT_TRUE(reg->split_set("ab", v2->version, 200, {{"why", "try"}}, true, &err)) << err;
  // A new live version ends it, keeping the record as split-last.
  auto v3 = reg->upload(manifest_for("ab", "*", "/ab", "ab_get"), module_for({"ab_get", "z"}), &err);
  ASSERT_TRUE(v3) << err;
  auto last = base::read_file(w->root() / "boxes" / "ab" / "split-last");
  ASSERT_TRUE(last);
  EXPECT_NE(last->find("\"outcome\":\"ended\""), std::string::npos) << *last;
  EXPECT_EQ(last->find("salt"), std::string::npos) << *last;
  // A change that read the record before the end can't bring the split back.
  EXPECT_FALSE(reg->split_set("ab", v2->version, 500, {}, true, &err));
  EXPECT_NE(err.find("ended meanwhile"), std::string::npos) << err;
  EXPECT_FALSE(reg->active()->splits.count("ab"));
}

TEST_F(RegistryTest, ProductionGoesLiveOverABetaVersionInItsWay) {
  std::string err;
  manifest::set_domain_aliases({{"beta.example.com", "example.com"}});
  reg->set_beta_domains({"beta.example.com"});
  // A box only in beta claims GET /x on beta.
  auto squat = reg->store(manifest_for("squat", "example.com", "/x", "squat_x"), module_for({"squat_x"}), "", &err, true);
  ASSERT_TRUE(squat) << err;
  ASSERT_TRUE(reg->beta_set("squat", squat->version, &err)) << err;
  bool marked = false;
  for (const auto& v : reg->versions("squat")) marked |= v.for_beta;
  EXPECT_TRUE(marked);
  // Production's box adds GET /x: it goes live, and the beta version in its way is cleared.
  ASSERT_TRUE(reg->upload(manifest_for("shop", "example.com", "/x", "shop_x"), module_for({"shop_x"}), &err)) << err;
  EXPECT_EQ(reg->active()->table_for("beta.example.com").match({"x"}, "get").value->box->manifest.box, "shop");
  EXPECT_FALSE(reg->beta_version("squat"));
  // Production's own conflicts still refuse.
  EXPECT_FALSE(reg->upload(manifest_for("rival", "example.com", "/x", "rival_x"), module_for({"rival_x"}), &err));
  manifest::set_domain_aliases({});
}
