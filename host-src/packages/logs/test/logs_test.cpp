#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>

#include "goober/base/files.h"
#include "goober/logs/logs.h"

using namespace goober;
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {

constexpr int64_t kDay = 24LL * 3600 * 1000;
constexpr int64_t kT0 = 1793232000000;  // 2026-10-29T00:00:00Z

fs::path fresh(const char* name) {
  auto d = fs::path(GOOBER_TEST_TMP) / name;
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

std::vector<std::string> lines_of(const fs::path& f) {
  std::ifstream in(f);
  std::vector<std::string> out;
  std::string l;
  while (std::getline(in, l)) out.push_back(l);
  return out;
}

void write_lines(const fs::path& f, const std::vector<std::string>& lines) {
  std::ofstream out(f, std::ios::trunc);
  for (const auto& l : lines) out << l << "\n";
}

}  // namespace

TEST(Logs, EveryLineIsEnvelopedAndChained) {
  auto dir = fresh("logs_chain");
  logs::Set set(dir, "kc.example.com");
  set.write("access", bj::object{{"path", "/"}, {"status", 200}, {"seq", 99}, {"chain", "forged"}}, kT0 + 1000);
  set.write("access", bj::object{{"path", "/b"}, {"status", 404}}, kT0 + 2000);
  auto lines = lines_of(dir / "access.log");
  ASSERT_EQ(lines.size(), 2u);
  // The envelope first, and the line's own fields never replace it.
  EXPECT_TRUE(lines[0].starts_with(R"({"at":"2026-10-29T00:00:01.000Z","log":"access","seq":1,"host":"kc.example.com","path":"/")"))
      << lines[0];
  EXPECT_NE(lines[1].find(R"("seq":2)"), std::string::npos);
  auto a = logs::split_chain(lines[0]), b = logs::split_chain(lines[1]);
  ASSERT_TRUE(a && b);
  EXPECT_EQ(a->second, logs::chain_of("genesis", a->first));
  EXPECT_EQ(b->second, logs::chain_of(a->second, b->first));
  auto v = logs::verify(dir, "access");
  EXPECT_TRUE(v.ok) << v.broken;
  EXPECT_EQ(v.lines, 2);
}

TEST(Logs, AnEditADeletionOrAnInsertionBreaksTheChain) {
  auto dir = fresh("logs_tamper");
  logs::Set set(dir, "h");
  for (int i = 0; i < 5; ++i) set.write("audit", bj::object{{"action", "x"}, {"n", i}}, kT0 + i);
  auto lines = lines_of(dir / "audit.log");
  auto edited = lines;
  edited[2].replace(edited[2].find("\"n\":2"), 5, "\"n\":7");
  write_lines(dir / "audit.log", edited);
  auto v = logs::verify(dir, "audit");
  EXPECT_FALSE(v.ok);
  EXPECT_NE(v.broken.find("audit.log:3"), std::string::npos) << v.broken;
  auto deleted = lines;
  deleted.erase(deleted.begin() + 1);
  write_lines(dir / "audit.log", deleted);
  EXPECT_FALSE(logs::verify(dir, "audit").ok);
  auto inserted = lines;
  inserted.insert(inserted.begin() + 3, inserted[3]);
  write_lines(dir / "audit.log", inserted);
  EXPECT_FALSE(logs::verify(dir, "audit").ok);
  write_lines(dir / "audit.log", lines);
  EXPECT_TRUE(logs::verify(dir, "audit").ok);
}

TEST(Logs, RotationByDayAndSizeKeepsTheChainAndQueuesAManifest) {
  auto dir = fresh("logs_rotate");
  logs::Set set(dir, "h", 2048);
  set.write("access", bj::object{{"n", 1}}, kT0 + 1000);
  set.write("access", bj::object{{"n", 2}}, kT0 + 2000);
  // The next day: the first write rotates yesterday's file.
  auto r = set.write("access", bj::object{{"n", 3}}, kT0 + kDay + 5);
  ASSERT_TRUE(r);
  EXPECT_TRUE(fs::exists(dir / "access.log.1"));
  EXPECT_TRUE(fs::exists(r->queued));
  EXPECT_EQ(fs::hard_link_count(r->queued), 2u);
  EXPECT_EQ(r->manifest_json.at("first_seq").as_int64(), 1);
  EXPECT_EQ(r->manifest_json.at("last_seq").as_int64(), 2);
  EXPECT_EQ(r->manifest_json.at("lines").as_int64(), 2);
  EXPECT_EQ(r->manifest_json.at("chain_before"), "genesis");
  EXPECT_TRUE(fs::exists(r->manifest));
  // seq and chain go on across the rotation.
  auto now = lines_of(dir / "access.log");
  ASSERT_EQ(now.size(), 1u);
  EXPECT_NE(now[0].find(R"("seq":3)"), std::string::npos);
  EXPECT_TRUE(logs::verify(dir, "access").ok);
  EXPECT_EQ(std::string(r->manifest_json.at("chain_after").as_string()),
            logs::split_chain(lines_of(dir / "access.log.1").back())->second);
  // By size: past 2 KiB, again.
  int rotations = 0;
  for (int i = 0; i < 40; ++i) rotations += bool(set.write("access", bj::object{{"pad", std::string(100, 'x')}}, kT0 + kDay + 10 + i));
  EXPECT_GE(rotations, 1);
  EXPECT_TRUE(fs::exists(dir / "access.log.2"));
  auto v = logs::verify(dir, "access");
  EXPECT_TRUE(v.ok) << v.broken;
  EXPECT_EQ(v.lines, 43);
  // A quiet log rotates at the tick.
  set.write("db", bj::object{{"sql", "SELECT 1"}}, kT0);
  auto stale = set.rotate_stale(kT0 + 2 * kDay);
  bool db = false;
  for (const auto& x : stale) db |= x.log == "db";
  EXPECT_TRUE(db);
}

TEST(Logs, AnotherProcessWritingKeepsOneChain) {
  auto dir = fresh("logs_processes");
  logs::Set set(dir, "h");
  set.write("db", bj::object{{"who", "host"}}, kT0 + 1);
  pid_t pid = ::fork();
  if (pid == 0) {
    logs::Set other(dir, "h");
    for (int i = 0; i < 20; ++i) other.write("db", bj::object{{"who", "cli"}, {"i", i}}, kT0 + 2);
    ::_exit(0);
  }
  for (int i = 0; i < 20; ++i) set.write("db", bj::object{{"who", "host"}, {"i", i}}, kT0 + 3);
  int status = 0;
  ::waitpid(pid, &status, 0);
  set.write("db", bj::object{{"who", "host"}, {"last", true}}, kT0 + 4);
  auto v = logs::verify(dir, "db");
  EXPECT_TRUE(v.ok) << v.broken;
  EXPECT_EQ(v.lines, 42);
  EXPECT_EQ(v.last_seq, "42");
}

TEST(Logs, SearchIsNewestFirstWithinTheWindowAndScope) {
  auto dir = fresh("logs_search");
  logs::Set set(dir, "h");
  for (int i = 0; i < 10; ++i) {
    set.write("access", bj::object{{"path", "/p" + std::to_string(i)}, {"tenant", i % 2 ? "kc" : "other"}, {"status", i == 7 ? 500 : 200}},
              kT0 + i * 1000);
  }
  set.write("mail", bj::object{{"to_h", "abc123"}, {"subject", "Hi"}, {"tenant", "kc"}}, kT0 + 4500);
  logs::Query q;
  q.pattern = "/p";
  auto f = logs::search(dir, {"access"}, q);
  ASSERT_EQ(f.hits.size(), 10u);
  EXPECT_NE(f.hits[0].line.find("/p9"), std::string::npos);
  q.limit = 3;
  f = logs::search(dir, {"access"}, q);
  ASSERT_EQ(f.hits.size(), 3u);
  EXPECT_TRUE(f.truncated);
  // The cursor continues below the last hit.
  logs::Query next = q;
  next.before_log = "access";
  next.before_seq = f.hits.back().seq;
  next.before_at_ms = f.hits.back().at_ms;
  auto f2 = logs::search(dir, {"access"}, next);
  EXPECT_NE(f2.hits[0].line.find("/p6"), std::string::npos);
  // Fields, regex, the window, a hash, and a scope.
  logs::Query fields;
  fields.fields["status"] = "500";
  EXPECT_EQ(logs::search(dir, {"access"}, fields).hits.size(), 1u);
  logs::Query rx;
  rx.regex = std::regex("/p[23]\"");
  EXPECT_EQ(logs::search(dir, {"access"}, rx).hits.size(), 2u);
  logs::Query window;
  window.since_ms = kT0 + 5000;
  window.until_ms = kT0 + 6000;
  EXPECT_EQ(logs::search(dir, {"access"}, window).hits.size(), 2u);
  logs::Query hashed;
  hashed.hashes = {"abc123"};
  auto byhash = logs::search(dir, {"access", "mail"}, hashed);
  ASSERT_EQ(byhash.hits.size(), 1u);
  EXPECT_EQ(byhash.hits[0].log, "mail");
  logs::Query scoped;
  scoped.allow = [](const std::string&, const bj::object& o) { return o.contains("tenant") && o.at("tenant") == "kc"; };
  EXPECT_EQ(logs::search(dir, {"access", "mail"}, scoped).hits.size(), 6u);
  logs::Query small;
  small.max_read = 100;
  EXPECT_TRUE(logs::search(dir, {"access"}, small).truncated);
}

TEST(Logs, NamesAndTimes) {
  EXPECT_TRUE(logs::valid_name("access"));
  EXPECT_TRUE(logs::valid_name("boxes/shop-1"));
  EXPECT_FALSE(logs::valid_name("../etc"));
  EXPECT_FALSE(logs::valid_name("boxes/../x"));
  EXPECT_EQ(*logs::parse_at("2026-10-29T00:00:01.000Z"), kT0 + 1000);
  EXPECT_FALSE(logs::parse_at("yesterday"));
}
