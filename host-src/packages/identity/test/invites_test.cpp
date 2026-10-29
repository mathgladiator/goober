// Invites and terms in the realm (design/machine/029.Invites.md §§ 2, 8).

#include <gtest/gtest.h>

#include <regex>

#include "goober/identity/identity.h"

using namespace goober::identity;
namespace fs = std::filesystem;

namespace {
constexpr int64_t kNow = 1'800'000'000'000;
constexpr int64_t kDay = 24LL * 3600 * 1000;

class InvitesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir = fs::path(GOOBER_TEST_TMP) / "identity_invites";
    fs::remove_all(dir);
    id = std::make_unique<Identity>(dir, [this](const Mail& m) { mail.push_back(m); });
  }
  Invite make(const std::string& email) {
    Invite i;
    i.email = email;
    i.roles = {"member"};
    i.lists = {"digest"};
    i.note = "Come grill with us";
    i.inviter_name = "Ann";
    i.inviter_email = "ann@example.com";
    i.inviter_kind = "person";
    i.box = "thesis";
    i.return_path = "/welcome";
    i.terms_sha256 = "abc";
    return i;
  }
  // Signs in by code: the address is proved.
  int64_t prove(const std::string& email, int64_t now) {
    id->start_email("kc", email, "kc.example.com", true, now);
    std::smatch m;
    std::regex_search(mail.back().text, m, std::regex("is ([0-9]{6})"));
    std::string why;
    auto in = id->verify_email("kc", email, m[1].str(), true, now, kDay, &why);
    EXPECT_TRUE(in) << why;
    return in ? in->user.user_id : 0;
  }
  fs::path dir;
  std::vector<Mail> mail;
  std::unique_ptr<Identity> id;
};
}  // namespace

TEST_F(InvitesTest, AnInviteIsAcceptedByTheProvedAddressAndGrantsItsRoles) {
  std::string code, why;
  auto inv = id->create_invite("kc", make("Bob@Example.com"), kNow, &code, &why);
  ASSERT_TRUE(inv) << why;
  EXPECT_EQ(inv->email, "bob@example.com");
  EXPECT_EQ(inv->expires_at, kNow + kInviteTtlMs);
  EXPECT_EQ(id->open_invite_for("kc", "BOB@example.com")->id, inv->id);
  // Only one open invite per address.
  EXPECT_FALSE(id->create_invite("kc", make("bob@example.com"), kNow + 1, &code, &why));
  EXPECT_EQ(code, "open");

  int64_t carol = prove("carol@example.com", kNow);
  EXPECT_FALSE(id->accept_invite("kc", inv->id, carol, "session", {}, kNow + 2, &why));  // another address
  int64_t bob = prove("bob@example.com", kNow + 3);
  auto allowed = [](const std::string& role) { return role == "member"; };
  auto user = id->accept_invite("kc", inv->id, bob, "code", allowed, kNow + 4, &why);
  ASSERT_TRUE(user) << why;
  EXPECT_EQ(user->roles, std::vector<std::string>{"member"});
  EXPECT_EQ(id->invite("kc", inv->id)->state, "accepted");
  EXPECT_FALSE(id->accept_invite("kc", inv->id, bob, "code", allowed, kNow + 5, &why));  // once
  // The event a box hears, naming the invite and its box.
  auto events = id->events_after("kc", 0, 50);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().kind, kInviteAccepted);
  EXPECT_EQ(events.back().invite_id, inv->id);
  EXPECT_EQ(events.back().box, "thesis");
  // A proved account can't be invited: it gets a grant instead.
  EXPECT_FALSE(id->create_invite("kc", make("bob@example.com"), kNow + 6, &code, &why));
  EXPECT_EQ(code, "member");
}

TEST_F(InvitesTest, RolesTheConfigDroppedArentGranted) {
  std::string code, why;
  auto i = make("dan@example.com");
  i.roles = {"member", "editor/blog"};
  auto inv = id->create_invite("kc", i, kNow, &code, &why);
  ASSERT_TRUE(inv) << why;
  int64_t dan = prove("dan@example.com", kNow + 1);
  auto user = id->accept_invite("kc", inv->id, dan, "session", [](const std::string& r) { return r == "member"; }, kNow + 2, &why);
  ASSERT_TRUE(user);
  EXPECT_EQ(user->roles, std::vector<std::string>{"member"});
  i.roles = {"host:owner"};
  i.email = "eve@example.com";
  EXPECT_FALSE(id->create_invite("kc", i, kNow, &code, &why));
  EXPECT_EQ(code, "bad_input");
}

TEST_F(InvitesTest, OneReminderThenLapseThenNinetyDaysCool) {
  std::string code, why;
  auto inv = id->create_invite("kc", make("fay@example.com"), kNow, &code, &why);
  ASSERT_TRUE(inv);
  EXPECT_TRUE(id->due_reminders("kc", kNow + 6 * kDay).empty());
  auto due = id->due_reminders("kc", kNow + 7 * kDay);
  ASSERT_EQ(due.size(), 1u);
  id->mark_reminded("kc", inv->id, kNow + 7 * kDay);
  EXPECT_TRUE(id->due_reminders("kc", kNow + 8 * kDay).empty());  // never a second
  id->prune("kc", kNow + 30 * kDay);
  EXPECT_EQ(id->invite("kc", inv->id)->state, "lapsed");
  EXPECT_FALSE(id->create_invite("kc", make("fay@example.com"), kNow + 31 * kDay, &code, &why));
  EXPECT_EQ(code, "cooling");
  EXPECT_TRUE(id->create_invite("kc", make("fay@example.com"), kNow + 121 * kDay, &code, &why)) << why;
}

TEST_F(InvitesTest, DecliningIsAlwaysPossibleAndFiresAnEventAsNobody) {
  std::string code, why;
  auto inv = id->create_invite("kc", make("gus@example.com"), kNow, &code, &why);
  ASSERT_TRUE(inv);
  id->prune("kc", kNow + 31 * kDay);  // lapsed: its decline link still works
  ASSERT_TRUE(id->decline_invite("kc", inv->id, "one_click", kNow + 32 * kDay, &why)) << why;
  EXPECT_EQ(id->invite("kc", inv->id)->state, "declined");
  EXPECT_FALSE(id->decline_invite("kc", inv->id, "page", kNow + 33 * kDay, &why));
  auto events = id->events_after("kc", 0, 50);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().kind, kInviteDeclined);
  EXPECT_EQ(events.back().user_id, 0);
  EXPECT_EQ(events.back().email, "gus@example.com");
}

TEST_F(InvitesTest, RevokedCoolsAndTheRecordFilters) {
  std::string code, why;
  auto a = id->create_invite("kc", make("hal@example.com"), kNow, &code, &why);
  auto i = make("ivy@example.com");
  i.box = "";
  i.inviter_kind = "operator";
  auto b = id->create_invite("kc", i, kNow + 1, &code, &why);
  ASSERT_TRUE(a && b);
  EXPECT_TRUE(id->revoke_invite("kc", a->id, "box:thesis", kNow + 2, &why));
  EXPECT_FALSE(id->revoke_invite("kc", a->id, "box:thesis", kNow + 3, &why));
  EXPECT_EQ(id->invites("kc", "", "", "", 10).size(), 2u);
  EXPECT_EQ(id->invites("kc", "open", "", "", 10).size(), 1u);
  EXPECT_EQ(id->invites("kc", "", "", "thesis", 10).size(), 1u);
  EXPECT_EQ(id->invites_since("kc", kNow - 1), 2);
  EXPECT_EQ(id->invites_since("kc", kNow - 1, "ann@example.com"), 2);
  EXPECT_FALSE(id->create_invite("kc", make("hal@example.com"), kNow + 4, &code, &why));
  EXPECT_EQ(code, "cooling");
  auto j = a->to_json();
  EXPECT_EQ(j.at("inviter").at("name"), "Ann");
}

TEST_F(InvitesTest, TermsAcceptedAreRecordedPerVersion) {
  int64_t user = prove("jan@example.com", kNow);
  EXPECT_FALSE(id->terms_accepted("kc", user, "/terms"));
  id->accept_terms("kc", user, "/terms", "v1", "sign-in", kNow);
  EXPECT_EQ(*id->terms_accepted("kc", user, "/terms"), "v1");
  id->accept_terms("kc", user, "/terms", "v2", "sign-in", kNow + 1);
  EXPECT_EQ(*id->terms_accepted("kc", user, "/terms"), "v2");
}
