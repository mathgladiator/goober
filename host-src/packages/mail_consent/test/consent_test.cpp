#include <gtest/gtest.h>

#include "goober/base/files.h"
#include "goober/mail_consent/consent.h"
#include "goober/sqlite/sqlite.h"

using namespace goober;
using namespace goober::mail::consent;
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {

struct Fixture {
  fs::path root;
  std::unique_ptr<Store> store;
  explicit Fixture(const char* name) : root(fs::path(GOOBER_TEST_TMP) / name) {
    fs::remove_all(root);
    store = std::make_unique<Store>(root / "consent.db");
  }
};

const std::string kAnn = address_hash("Ann@Example.com");

}  // namespace

TEST(Consent, TheHashIsTheSentLedgersAndIgnoresCase) {
  EXPECT_EQ(address_hash("ann@example.com"), kAnn);
  EXPECT_EQ(kAnn.size(), 32u);
  EXPECT_NE(address_hash("bob@example.com"), kAnn);
}

TEST(Consent, ABlockStopsUntilAPersonLiftsItWithAStatement) {
  Fixture f("consent_blocks");
  auto& s = *f.store;
  EXPECT_FALSE(s.block("kc", kAnn));
  EXPECT_TRUE(s.add_block("kc", kAnn, "invite_declined", bj::object{{"invite_id", 7}}, 1000));
  EXPECT_FALSE(s.add_block("kc", kAnn, "unsubscribed", {}, 2000));  // the first block stands
  auto b = s.block("kc", kAnn);
  ASSERT_TRUE(b);
  EXPECT_EQ(b->how, "invite_declined");
  EXPECT_EQ(b->evidence.at("invite_id").as_int64(), 7);
  EXPECT_FALSE(s.block("other", kAnn));  // one tenant only

  std::string why;
  EXPECT_FALSE(s.lift("kc", kAnn, "operator:boss@example.com", "asked", "r1", 3000, &why));  // too short to say anything
  EXPECT_NE(why.find("at least 20"), std::string::npos);
  EXPECT_FALSE(s.lift("kc", kAnn, "grok", "Asked by phone on 11/2 to rejoin", "r1", 3000, &why));
  auto lifted = s.lift("kc", kAnn, "operator:boss@example.com", "Asked by phone on 11/2 to rejoin", "r1", 3000, &why);
  ASSERT_TRUE(lifted) << why;
  EXPECT_EQ(lifted->at_ms, 1000);
  EXPECT_FALSE(s.block("kc", kAnn));
  EXPECT_FALSE(s.lift("kc", kAnn, "person", "I want mail again", "r2", 4000, &why));  // nothing left to lift
  // The record keeps both, with the statement.
  auto h = s.history("kc", kAnn);
  ASSERT_EQ(h.size(), 2u);
  EXPECT_EQ(h[0].at("what"), "block");
  EXPECT_EQ(h[1].at("what"), "lift");
  EXPECT_EQ(h[1].at("evidence").at("statement"), "Asked by phone on 11/2 to rejoin");
}

TEST(Consent, ListsNeedAnOptInAndSayingNoWins) {
  Fixture f("consent_lists");
  auto& s = *f.store;
  std::string why;
  EXPECT_FALSE(s.opt_in("kc", kAnn, "digest", "box", {}, 1, &why));  // a box can't record consent
  ASSERT_TRUE(s.opt_in("kc", kAnn, "digest", "confirmed", bj::object{{"request_id", "r"}}, 10, &why)) << why;
  EXPECT_EQ(s.list_state("kc", kAnn, "digest")->state, "in");
  EXPECT_EQ(s.count_in("kc", "digest"), 1);
  EXPECT_EQ(s.members("kc", "digest").at(0), kAnn);
  s.opt_out("kc", kAnn, "digest", "link", {}, 20);
  EXPECT_EQ(s.list_state("kc", kAnn, "digest")->state, "out");
  EXPECT_EQ(s.count_in("kc", "digest"), 0);
  // An operator never re-subscribes someone who said no; the person can.
  EXPECT_FALSE(s.opt_in("kc", kAnn, "digest", "operator", bj::object{{"statement", "at the meetup"}}, 30, &why));
  EXPECT_NE(why.find("only they"), std::string::npos);
  EXPECT_TRUE(s.opt_in("kc", kAnn, "digest", "account", {}, 40, &why)) << why;
  // A blocked address can't be opted in at all.
  s.add_block("kc", kAnn, "unsubscribed", {}, 50);
  EXPECT_FALSE(s.opt_in("kc", kAnn, "news", "account", {}, 60, &why));
  EXPECT_EQ(s.lists("kc", kAnn).size(), 1u);
}

TEST(Consent, OutreachIsCountedAndAnOptOutIsForGood) {
  Fixture f("consent_outreach");
  auto& s = *f.store;
  EXPECT_FALSE(s.outreach("kc", kAnn));
  s.outreach_sent("kc", kAnn, 100);
  s.outreach_sent("kc", kAnn, 200);
  auto o = s.outreach("kc", kAnn);
  ASSERT_TRUE(o);
  EXPECT_EQ(o->first_ms, 100);
  EXPECT_EQ(o->last_ms, 200);
  EXPECT_EQ(o->count, 2);
  EXPECT_EQ(s.outreach_since("kc", 150), 1);
  s.outreach_opt_out("kc", kAnn, "link", 300);
  EXPECT_TRUE(s.outreach("kc", kAnn)->opted_out);
  s.asked("kc", kAnn, "list-confirm:digest", 400);
  EXPECT_EQ(s.asked_since("kc", kAnn, "list-confirm:digest", 0), 1);
  EXPECT_EQ(s.asked_since("kc", kAnn, "list-confirm:digest", 401), 0);
}

TEST(Consent, PruningKeepsFiveYearsAndTheOldUnsubscribesBecomeBlocks) {
  Fixture f("consent_import");
  auto& s = *f.store;
  {
    sqlite::Db old(f.root / "unsubscribed.db");
    old.exec("CREATE TABLE unsubscribed (tenant TEXT NOT NULL, address TEXT NOT NULL, at_ms INTEGER NOT NULL, how TEXT "
             "NOT NULL, PRIMARY KEY (tenant, address))");
    old.exec("INSERT INTO unsubscribed VALUES ('kc', '" + kAnn + "', 5, 'link')");
  }
  EXPECT_EQ(s.import_unsubscribed(f.root / "unsubscribed.db", 10), 1);
  EXPECT_FALSE(fs::exists(f.root / "unsubscribed.db"));
  EXPECT_TRUE(fs::exists(f.root / "unsubscribed.db.imported"));
  EXPECT_EQ(s.block("kc", kAnn)->how, "unsubscribed");
  EXPECT_EQ(s.import_unsubscribed(f.root / "unsubscribed.db", 20), 0);  // once
  int64_t later = 5 + kLogKeepMs + 1;
  s.prune(later);
  EXPECT_EQ(s.history("kc", kAnn).size(), 0u);  // the log row aged out
  EXPECT_TRUE(s.block("kc", kAnn));             // the block itself never does
}
