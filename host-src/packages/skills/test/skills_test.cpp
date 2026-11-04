#include "goober/skills/skills.h"

#include <gtest/gtest.h>

using namespace goober;
using skills::Skill;

namespace {

Skill make(std::string name, std::string title, std::vector<std::string> roles, std::string body,
           std::string description = "Use it when the time comes.", std::string kind = "skill") {
  Skill s;
  s.name = std::move(name);
  s.title = std::move(title);
  s.roles = std::move(roles);
  s.body = std::move(body);
  s.description = std::move(description);
  s.kind = std::move(kind);
  return s;
}

}  // namespace

TEST(Skills, NamesFollowTheAgentSkillsRule) {
  EXPECT_FALSE(skills::name_refusal("deploy-a-box"));
  EXPECT_FALSE(skills::name_refusal("p99-triage"));
  EXPECT_TRUE(skills::name_refusal(""));
  EXPECT_TRUE(skills::name_refusal("Deploy"));
  EXPECT_TRUE(skills::name_refusal("9lives"));
  EXPECT_TRUE(skills::name_refusal("a--b"));
  EXPECT_TRUE(skills::name_refusal("trailing-"));
  EXPECT_TRUE(skills::name_refusal("under_score"));
  EXPECT_TRUE(skills::name_refusal("../etc"));
  EXPECT_TRUE(skills::name_refusal(std::string(65, 'a')));
  EXPECT_FALSE(skills::name_refusal(std::string(64, 'a')));
}

TEST(Skills, CheckPutsASkillInOneForm) {
  auto s = make("deploy-a-box", "  Deploy a box ", {"Operator", "box-engineer", "operator"}, "\r\n# Steps\r\n\r\n1. Upload\r\n\n");
  std::string err;
  ASSERT_TRUE(skills::check(s, &err)) << err;
  EXPECT_EQ(s.title, "Deploy a box");
  EXPECT_EQ(s.roles, (std::vector<std::string>{"box-engineer", "operator"}));
  EXPECT_EQ(s.body, "# Steps\n\n1. Upload\n");
  EXPECT_EQ(s.kind, "skill");

  auto bad = [&](Skill x, const std::string& want) {
    std::string e;
    EXPECT_FALSE(skills::check(x, &e)) << want;
    EXPECT_NE(e.find(want), std::string::npos) << e;
  };
  bad(make("x", "", {"a"}, "b"), "title is required");
  bad(make("x", "two\nlines", {"a"}, "b"), "title must be one line");
  bad(make("x", "t", {}, "b"), "roles is required");
  bad(make("x", "t", {"box engineer"}, "b"), "a role is");
  bad(make("x", "t", {"a", "b", "c", "d", "e", "f", "g", "h", "i"}, "b"), "at most 8 roles");
  bad(make("x", "t", {"a"}, "  \n "), "body is required");
  bad(make("x", "t", {"a"}, std::string(skills::kBodyMaxBytes + 1, 'x')), "body is");
  bad(make("x", "t", {"a"}, "b", "d", "runbook"), "kind is");
  bad(make("x", "t", {"a"}, std::string("b\0c", 3)), "UTF-8");
  bad(make("x", "t", {"a"}, "b", std::string(skills::kDescriptionMaxBytes + 1, 'd')), "description is");
}

TEST(Skills, TheFileIsASkillMdAndReadsBack) {
  auto s = make("incident-first-hour", "The first hour of an incident", {"operator"},
                "# First\n\nCheck `/goober/ready`, then \"host_health\".\n", "When the site is down: \"what\" to look at first.",
                "playbook");
  std::string err;
  ASSERT_TRUE(skills::check(s, &err)) << err;
  s.revision = 3;
  s.created_ms = 1000;
  s.updated_ms = 2000;
  s.created_by = "token:grok";
  s.updated_by = "oauth:owner@example.com:1";
  std::string file = skills::to_file(s);
  // An Agent Skills reader finds its two fields first.
  EXPECT_TRUE(file.starts_with("---\nname: incident-first-hour\ndescription: \"When the site is down: \\\"what\\\" to look at first.\"\n"))
      << file;
  auto back = skills::from_file(file, "incident-first-hour", &err);
  ASSERT_TRUE(back) << err;
  EXPECT_EQ(back->title, s.title);
  EXPECT_EQ(back->description, s.description);
  EXPECT_EQ(back->kind, "playbook");
  EXPECT_EQ(back->roles, s.roles);
  EXPECT_EQ(back->body, s.body);
  EXPECT_EQ(back->revision, 3);
  EXPECT_EQ(back->updated_by, "oauth:owner@example.com:1");
  EXPECT_EQ(skills::to_file(*back), file);
  // A file under another name is refused, as is one changed by hand into something a put refuses.
  EXPECT_FALSE(skills::from_file(file, "other", &err));
  EXPECT_FALSE(skills::from_file("no front matter", "x", &err));
  EXPECT_EQ(skills::etag_of(file).size(), 18u);
  EXPECT_NE(skills::etag_of(file), skills::etag_of(file + " "));
}

TEST(Skills, SearchRanksNamesOverBodiesAndNeedsEveryWord) {
  skills::Library lib;
  std::string err;
  auto put = [&](Skill s) {
    ASSERT_TRUE(skills::check(s, &err)) << err;
    lib.put(s, int64_t(skills::to_file(s).size()), "\"e\"");
  };
  put(make("deploy-a-box", "Deploy a box", {"box-engineer"},
           "Upload with version_upload, dry run first.\n\nThen watch version_health for ten minutes after deploying."));
  put(make("site-launch", "Launch a site", {"site-engineer", "operator"},
           "Write pages, then content_deploy.\nDeploying a site means its pages go live.", "Use when a new tenant gets its site."));
  put(make("incident-first-hour", "The first hour of an incident", {"operator"}, "Check host_health, then error_report.",
           "When the site is down", "playbook"));

  auto r = lib.search("deploy", "", "", 10);
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0].at("name"), "deploy-a-box");  // in its name and title
  EXPECT_EQ(r[1].at("name"), "site-launch");   // "content_deploy" and "deploying", only in the body
  // The snippet is the first line that matches: content_deploy's words are "content" and "deploy".
  EXPECT_EQ(r[1].at("snippet"), "Write pages, then content_deploy.");
  EXPECT_NE(std::string(lib.search("deploying", "site-engineer", "", 10).at(0).at("snippet").as_string()).find("Deploying a site"),
            std::string::npos);

  // Every word must match somewhere.
  EXPECT_EQ(lib.search("deploy health", "", "", 10).size(), 1u);
  EXPECT_EQ(lib.search("deploy nothing-like-this", "", "", 10).size(), 0u);
  // Roles and kinds narrow it; a role is a word too.
  EXPECT_EQ(lib.search("deploy", "site-engineer", "", 10).size(), 1u);
  EXPECT_EQ(lib.search("site", "", "playbook", 10).size(), 1u);
  EXPECT_EQ(lib.search("operator", "", "", 10).size(), 2u);
  EXPECT_EQ(lib.search("deploy", "", "", 1).size(), 1u);
  // Case and punctuation don't matter.
  EXPECT_EQ(lib.search("HOST_HEALTH!", "", "", 10).size(), 1u);

  auto roles = lib.roles();
  ASSERT_EQ(roles.size(), 3u);
  EXPECT_EQ(roles[1].at("role"), "operator");
  EXPECT_EQ(roles[1].at("skills"), 2);
  EXPECT_EQ(roles[1].at("kinds").at("playbook"), 1);
  EXPECT_EQ(lib.list("operator", "").size(), 2u);
  EXPECT_EQ(lib.list("", "playbook").size(), 1u);
  EXPECT_FALSE(lib.list("", "").at(0).as_object().contains("body"));

  int64_t before = lib.bytes();
  lib.remove("site-launch");
  EXPECT_LT(lib.bytes(), before);
  EXPECT_EQ(lib.count(), 2u);
  EXPECT_FALSE(lib.get("site-launch"));
}

TEST(Skills, SnippetsCutLongLinesAtWords) {
  skills::Library lib;
  std::string err;
  std::string line;
  for (int i = 0; i < 60; ++i) line += "word" + std::to_string(i) + " ";
  line += "needle ";
  for (int i = 0; i < 60; ++i) line += "after" + std::to_string(i) + " ";
  auto s = make("long-one", "Long", {"operator"}, line);
  ASSERT_TRUE(skills::check(s, &err)) << err;
  lib.put(s, 1, "\"e\"");
  auto r = lib.search("needle", "", "", 5);
  ASSERT_EQ(r.size(), 1u);
  std::string snip(r[0].at("snippet").as_string());
  EXPECT_TRUE(snip.starts_with("…word")) << snip;
  EXPECT_NE(snip.find("needle"), std::string::npos);
  EXPECT_LE(snip.size(), 220u);
}
