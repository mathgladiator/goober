// Source control over MCP (design/machine/020.Source.md): create, check out, commit, build,
// and deploy, as an agent would, against a running host.

#include <gtest/gtest.h>

#include <set>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "../src/builder.h"
#include "../src/keys.h"
#include "goober/base/files.h"
#include "goober/base/process.h"
#include "goober/host/host.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

struct Reply {
  int status = 0;
  std::string body;
};

Reply request(uint16_t port, bhttp::verb verb, const std::string& target, const std::string& body = "",
              const std::string& auth = "") {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  bhttp::request<bhttp::string_body> req(verb, target, 11);
  req.set(bhttp::field::host, "localhost");
  if (!auth.empty()) req.set(bhttp::field::authorization, auth);
  req.body() = body;
  req.prepare_payload();
  bhttp::write(stream, req);
  beast::flat_buffer buf;
  bhttp::response<bhttp::string_body> res;
  bhttp::read(stream, buf, res);
  return {int(res.result_int()), res.body()};
}

bool have(const char* program) {
  for (const char* dir : {"/usr/bin", "/bin", "/usr/local/bin"}) {
    if (fs::exists(fs::path(dir) / program)) return true;
  }
  return false;
}

}  // namespace

class SourceToolsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::path(GOOBER_TEST_TMP) / "source-tools";
    fs::remove_all(root);
    std::string err;
    auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
        "domains": {"localhost": {"tenant": "playground"}}, "operations": {"token": "test-token-0123456789"},
        "threads": {"network": 1, "boxes": 2}, "source": {"build-timeout-ms": 300000}})",
                                    root, &err);
    ASSERT_TRUE(cfg) << err;
    host = std::make_unique<host::Host>(config::local_mode(cfg));
    port = host->start();
  }
  void TearDown() override { host.reset(); }

  bj::value tool(const std::string& name, bj::object args) {
    bj::object req{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                   {"params", bj::object{{"name", name}, {"arguments", std::move(args)}}}};
    auto r = request(port, bhttp::verb::post, "/goober-operations-mcp", json::dump(req), "Bearer test-token-0123456789");
    std::string err;
    auto doc = json::parse(r.body, &err);
    const auto& result = doc->at("result");
    std::string text(result.at("content").at(0).at("text").as_string());
    if (result.at("isError").as_bool()) return bj::object{{"error", text}};
    auto v = json::parse(text, &err);
    return v ? *v : bj::value(text);
  }
  std::string get(const std::string& path) { return request(port, bhttp::verb::get, path).body; }

  fs::path root;
  std::unique_ptr<host::Host> host;
  uint16_t port = 0;
};

TEST_F(SourceToolsTest, CheckOutChangeBuildDeployAndGoBack) {
  auto created = tool("source_create", {{"project", "greeter"}, {"template", "wat-box"}, {"description", "says hello"}});
  ASSERT_FALSE(created.as_object().contains("error")) << json::dump(created);
  EXPECT_EQ(created.at("head"), 1);

  auto co = tool("source_checkout", {{"project", "greeter"}});
  ASSERT_EQ(co.at("rev"), 1);
  std::string wat;
  for (const auto& f : co.at("files").as_array()) {
    if (f.at("path") == "box.wat") wat = std::string(f.at("text").as_string());
  }
  ASSERT_NE(wat.find("hello"), std::string::npos);

  auto deployed = tool("source_deploy", {{"verbose", true}, {"project", "greeter"}});
  ASSERT_EQ(deployed.at("state"), "succeeded") << json::dump(deployed);
  EXPECT_EQ(get("/greeter/hello"), "hello");

  // An agent changes the box: same length, new word.
  std::string howdy = wat;
  howdy.replace(howdy.find("\"hello\""), 7, "\"howdy\"");
  auto committed = tool("source_commit", {{"project", "greeter"}, {"base_rev", 1}, {"message", "Say howdy"},
                                          {"agent", "Engineer A"},
                                          {"changes", bj::array{bj::object{{"path", "box.wat"}, {"text", howdy}}}}});
  ASSERT_EQ(committed.at("rev"), 2) << json::dump(committed);
  deployed = tool("source_deploy", {{"verbose", true}, {"project", "greeter"}});
  ASSERT_EQ(deployed.at("state"), "succeeded") << json::dump(deployed);
  EXPECT_EQ(get("/greeter/hello"), "howdy");
  EXPECT_EQ(tool("source_list", {}).at(0).at("deployed_rev"), 2);

  // A stale commit on the same file is refused with the conflict named.
  auto stale = tool("source_commit", {{"project", "greeter"}, {"base_rev", 1}, {"message", "late"},
                                      {"changes", bj::array{bj::object{{"path", "box.wat"}, {"text", wat}}}}});
  EXPECT_EQ(stale.at("committed"), false);
  EXPECT_EQ(stale.at("conflicts").at(0), "box.wat");

  auto diff = tool("source_diff", {{"verbose", true}, {"project", "greeter"}, {"from", 1}});
  EXPECT_NE(std::string(diff.at("diff").as_string()).find("+  (data (i32.const 16) \"howdy\")"), std::string::npos) << diff;
  EXPECT_EQ(diff.at("files").as_array().size(), 1u);
  auto log = tool("source_log", {{"project", "greeter"}});
  EXPECT_EQ(log.at(0).at("message"), "Say howdy");
  EXPECT_EQ(log.at(0).at("by"), "config (agent: Engineer A)");  // the operator behind the token, and who said they were

  // Going back is a new revision, deployed like any other.
  auto back = tool("source_revert", {{"project", "greeter"}, {"rev", 1}});
  EXPECT_EQ(back.at("rev"), 3);
  ASSERT_EQ(tool("source_deploy", {{"verbose", true}, {"project", "greeter"}}).at("state"), "succeeded");
  EXPECT_EQ(get("/greeter/hello"), "hello");
}

TEST_F(SourceToolsTest, SitesDeployAsContent) {
  auto created = tool("source_create", {{"project", "home"}, {"template", "site"}, {"tenant", "playground"}});
  ASSERT_EQ(created.at("head"), 1) << json::dump(created);
  auto deployed = tool("source_deploy", {{"verbose", true}, {"project", "home"}});
  ASSERT_EQ(deployed.at("state"), "succeeded") << json::dump(deployed);
  EXPECT_NE(get("/").find("This page lives in the project home"), std::string::npos);
}

TEST_F(SourceToolsTest, BuildFailuresSayWhy) {
  ASSERT_EQ(tool("source_create", {{"project", "broken"}}).at("head"), 0);
  auto c = tool("source_commit", {{"project", "broken"}, {"base_rev", 0}, {"message", "half"},
                                  {"changes", bj::array{bj::object{{"path", "goober.project.json"}, {"text", R"({"boxes":["."]})"}}}}});
  ASSERT_EQ(c.at("rev"), 1);
  auto built = tool("source_build", {{"verbose", true}, {"project", "broken"}});
  EXPECT_EQ(built.at("state"), "failed");
  EXPECT_NE(std::string(built.at("log").as_string()).find("manifest.json is missing"), std::string::npos) << json::dump(built);
  // A bad module fails at deploy, with the host's own reason.
  tool("source_commit", {{"project", "broken"}, {"base_rev", 1}, {"message", "a box"},
                         {"changes", bj::array{bj::object{{"path", "manifest.json"},
                                                           {"text", R"({"box":"broken","domains":["*"],"routes":{"http":{"/b":{"get":{"name":"b","public":true}}}}})"}},
                                               bj::object{{"path", "box.wat"}, {"text", "(module"}}}}});
  auto deployed = tool("source_deploy", {{"verbose", true}, {"project", "broken"}});
  EXPECT_EQ(deployed.at("state"), "failed");
  EXPECT_NE(std::string(deployed.at("log").as_string()).find("✗ box broken"), std::string::npos) << json::dump(deployed);
  EXPECT_NE(json::dump(tool("source_create", {{"project", "x"}, {"template", "site"}})).find("needs the tenant"), std::string::npos);
  EXPECT_NE(json::dump(tool("source_job", {{"verbose", true}, })).find("broken"), std::string::npos);
}

// A Rust box, built by cargo in the bubblewrap sandbox, where this machine has the tools.
TEST_F(SourceToolsTest, RustBoxesBuildInTheSandbox) {
  if (!have("bwrap")) GTEST_SKIP() << "no bwrap";
  auto cargo = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".cargo/bin/cargo";
  if (!fs::exists(cargo) && !have("cargo")) GTEST_SKIP() << "no cargo";
  ASSERT_EQ(tool("source_create", {{"project", "rusty"}}).at("head"), 0);
  // No dependencies, so no vendored crates are needed.
  bj::array files{
      bj::object{{"path", "goober.project.json"}, {"text", R"({"boxes":["."]})"}},
      bj::object{{"path", "Cargo.toml"},
                 {"text", "[package]\nname = \"rusty\"\nversion = \"0.1.0\"\nedition = \"2024\"\n[lib]\ncrate-type = "
                          "[\"cdylib\"]\npath = \"src/lib.rs\"\n[profile.release]\nopt-level = \"s\"\npanic = \"abort\"\n"}},
      bj::object{{"path", "manifest.json"},
                 {"text", R"({"box":"rusty","domains":["*"],"routes":{"http":{"/rusty":{"get":{"name":"hello","public":true}}}}})"}},
      bj::object{{"path", "src/lib.rs"},
                 {"text", "#[link(wasm_import_module = \"goober\")]\nunsafe extern \"C\" {\n"
                          "    fn goober_respond(s: i32, tp: *const u8, tl: i32, bp: *const u8, bl: i32) -> i32;\n}\n"
                          "#[unsafe(no_mangle)]\npub extern \"C\" fn goober_route_hello() -> i32 {\n"
                          "    let (t, b) = (b\"text/plain\", b\"rusty\");\n"
                          "    unsafe { goober_respond(200, t.as_ptr(), t.len() as i32, b.as_ptr(), b.len() as i32) };\n"
                          "    0\n}\n"}}};
  ASSERT_EQ(tool("source_commit", {{"project", "rusty"}, {"base_rev", 0}, {"message", "a rust box"}, {"changes", files}})
                .at("rev"),
            1);
  auto job = tool("source_deploy", {{"verbose", true}, {"project", "rusty"}});
  while (job.at("state") == "running" || job.at("state") == "queued") job = tool("source_job", {{"verbose", true}, {"job", job.at("job")}});
  ASSERT_EQ(job.at("state"), "succeeded") << job.at("log").as_string();
  EXPECT_NE(std::string(job.at("log").as_string()).find("in the sandbox"), std::string::npos) << job.at("log").as_string();
  EXPECT_EQ(get("/rusty"), "rusty");

  // A compiler error comes back in the log.
  ASSERT_EQ(tool("source_commit", {{"project", "rusty"}, {"base_rev", 1}, {"message", "oops"},
                                   {"changes", bj::array{bj::object{{"path", "src/lib.rs"}, {"text", "fn broken( {}\n"}}}}})
                .at("rev"),
            2);
  auto failed = tool("source_build", {{"verbose", true}, {"project", "rusty"}});
  while (failed.at("state") == "running" || failed.at("state") == "queued") failed = tool("source_job", {{"verbose", true}, {"job", failed.at("job")}});
  EXPECT_EQ(failed.at("state"), "failed");
  EXPECT_NE(std::string(failed.at("log").as_string()).find("error"), std::string::npos) << failed.at("log").as_string();
}

TEST(Sandbox, SeesNoNetworkAndNoSecrets) {
  if (!have("bwrap")) GTEST_SKIP() << "no bwrap";
  auto dir = fs::path(GOOBER_TEST_TMP) / "sandbox";
  fs::remove_all(dir);
  fs::create_directories(dir / "src");
  fs::create_directories(dir / "target");
  config::HostConfig cfg;
  std::string err;
  auto argv = host::sandboxed({"sh", "-c", "ls /etc; cat /etc/hostname; ls /home; echo HOME=$HOME; touch /work/ok; "
                                           "touch /usr/nope 2>/dev/null || echo readonly"},
                              cfg, dir / "src", dir / "target", "/usr/bin/cargo", "", &err);
  ASSERT_FALSE(argv.empty()) << err;
  base::ProcessOptions po;
  po.env = std::vector<std::string>{};
  auto r = base::run_process(argv, "", 1 << 16, 20000, po);
  ASSERT_EQ(r.exit_code, 0) << r.err;
  EXPECT_EQ(r.out.find("passwd"), std::string::npos) << r.out;  // /etc holds only the linker's bits
  EXPECT_EQ(r.out.find("math"), std::string::npos) << r.out;    // no home directories
  EXPECT_NE(r.out.find("HOME=/work"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("readonly"), std::string::npos) << r.out;
  EXPECT_TRUE(fs::exists(dir / "src/ok"));
}

// Review 5, finding 1: a build script can't make the host read one of its own files as the
// build's output: only an exact `name` names the crate, and symlinks aren't followed.
TEST_F(SourceToolsTest, BuildsCantPointTheHostAtItsOwnFiles) {
  if (!have("bwrap")) GTEST_SKIP() << "no bwrap";
  auto cargo = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".cargo/bin/cargo";
  if (!fs::exists(cargo) && !have("cargo")) GTEST_SKIP() << "no cargo";
  std::string err;
  ASSERT_TRUE(host->keys().set("system", "", "secret_thing", "TOP-SECRET-VALUE", 1, &err, {"api.example.com"})) << err;
  ASSERT_EQ(tool("source_create", {{"project", "sneaky"}}).at("head"), 0);
  bj::array files{
      bj::object{{"path", "goober.project.json"}, {"text", R"({"boxes":["."]})"}},
      bj::object{{"path", "Cargo.toml"},
                 {"text", "[package]\nname = \"sneaky\"\nnamex = \"decoy\"\nversion = \"0.1.0\"\nedition = \"2024\"\n"
                          "build = \"build.rs\"\n[lib]\ncrate-type = [\"cdylib\"]\npath = \"src/lib.rs\"\n"}},
      bj::object{{"path", "build.rs"},
                 {"text", "fn main() {\n  let _ = std::fs::create_dir_all(\"/target/wasm32-unknown-unknown/release\");\n"
                          "  let _ = std::os::unix::fs::symlink(\"../../../../../keys/system/secret_thing\", "
                          "\"/target/wasm32-unknown-unknown/release/decoy.wasm\");\n}\n"}},
      bj::object{{"path", "manifest.json"},
                 {"text", R"({"box":"sneaky","domains":["*"],"routes":{"http":{"/sneaky":{"get":{"name":"sneaky_hi","public":true}}}}})"}},
      bj::object{{"path", "src/lib.rs"},
                 {"text", "#[unsafe(no_mangle)]\npub extern \"C\" fn goober_route_sneaky_hi() -> i32 { 0 }\n"}}};
  ASSERT_EQ(tool("source_commit", {{"project", "sneaky"}, {"base_rev", 0}, {"message", "m"}, {"changes", files}}).at("rev"), 1);
  auto job = tool("source_deploy", {{"verbose", true}, {"project", "sneaky"}});
  while (job.at("state") == "running" || job.at("state") == "queued") job = tool("source_job", {{"verbose", true}, {"job", job.at("job")}});
  std::string log(job.at("log").as_string());
  EXPECT_EQ(log.find("TOP-SECRET"), std::string::npos) << log;
  EXPECT_EQ(job.at("state"), "succeeded") << log;  // it built the real crate, sneaky.wasm
}

// Directory Engineer's report: a deploy with a new schema and a box the host refuses stored the
// schema anyway, for good. Now everything is checked first, by build and deploy alike.
TEST_F(SourceToolsTest, NothingIsStoredUnlessEverythingChecksOut) {
  ASSERT_EQ(tool("source_create", {{"project", "dir"}}).at("head"), 0);
  std::string schema = R"({"schema":"locations_v3","tables":{"locations":{"columns":{"name":{"type":"string"}}}}})";
  std::string bad = R"({"box":"kc_dir","domains":["*"],
    "data":{"sqlite3":{"w":{"schema":"locations_v3","database":"dir","pattern":"sql","tables":{"locations":{"read":true}}}}},
    "routes":{"http":{"/search":{"get":{"name":"search","public":true,
      "query":{"limit":{"type":"int32","default":20}}}}}}})";
  std::string wat = R"((module (memory (export "memory") 1) (func (export "goober_route_search") (result i32) (i32.const 0))))";
  auto c = tool("source_commit", {{"project", "dir"}, {"base_rev", 0}, {"message", "search"},
                                  {"changes", bj::array{
                                      bj::object{{"path", "goober.project.json"}, {"text", R"({"boxes":["."],"schemas":["locations_v3.schema.json"]})"}},
                                      bj::object{{"path", "locations_v3.schema.json"}, {"text", schema}},
                                      bj::object{{"path", "manifest.json"}, {"text", bad}},
                                      bj::object{{"path", "box.wat"}, {"text", wat}}}}});
  ASSERT_EQ(c.at("rev"), 1) << json::dump(c);
  // The build says what the deploy would say.
  auto built = tool("source_build", {{"verbose", true}, {"project", "dir"}});
  EXPECT_EQ(built.at("state"), "failed");
  std::string log(built.at("log").as_string());
  EXPECT_NE(log.find("unknown type \"int32\": in a schema, write {\"type\": \"integer\", \"format\": \"int32\"}"), std::string::npos) << log;
  auto deployed = tool("source_deploy", {{"verbose", true}, {"project", "dir"}});
  EXPECT_EQ(deployed.at("state"), "failed");
  EXPECT_NE(std::string(deployed.at("log").as_string()).find("nothing was stored or deployed"), std::string::npos);
  // The schema wasn't stored, so the fixed revision can still use the name, and change it.
  EXPECT_FALSE(host->schemas().get("locations_v3"));
  std::string good = bad;
  good.replace(good.find(R"({"type":"int32","default":20})"), 29, R"({"type":"integer","format":"int32","default":20})");
  std::string schema2 = R"({"schema":"locations_v3","tables":{"locations":{"columns":{"name":{"type":"string"},"city":{"type":"string","nullable":true}}}}})";
  auto c2 = tool("source_commit", {{"project", "dir"}, {"base_rev", 1}, {"message", "fix"},
                                   {"changes", bj::array{bj::object{{"path", "manifest.json"}, {"text", good}},
                                                         bj::object{{"path", "locations_v3.schema.json"}, {"text", schema2}}}}});
  ASSERT_EQ(c2.at("rev"), 2) << json::dump(c2);
  EXPECT_EQ(tool("source_build", {{"verbose", true}, {"project", "dir"}}).at("state"), "succeeded");
  auto ok = tool("source_deploy", {{"verbose", true}, {"project", "dir"}});
  ASSERT_EQ(ok.at("state"), "succeeded") << json::dump(ok);
  ASSERT_TRUE(host->schemas().get("locations_v3"));
  EXPECT_TRUE(host->schemas().get("locations_v3")->table("locations")->column("city"));
}

// GrokBot's report: after source_deploy, a box's new typed MCP tools must be what is listed.
TEST_F(SourceToolsTest, DeployedToolsAreListedWithTheirTypes) {
  ASSERT_EQ(tool("source_create", {{"project", "loc"}}).at("head"), 0);
  auto manifest = [](bool v2) {
    std::string tools = R"("list_locations":{"tool":{"description":"Lists.","requirements":[["owner"]])" +
                        std::string(v2 ? R"(,"input":"json:ListInput")" : "") + R"(}},
      "add_location":{"tool":{"description":"Adds.","requirements":[["owner"]])" +
                        std::string(v2 ? R"(,"input":"json:Location")" : "") + "}}";
    if (v2) {
      tools += R"(,"get_location":{"tool":{"description":"Gets.","requirements":[["owner"]],"input":"json:Slug"}},
      "import_locations":{"tool":{"description":"Imports.","requirements":[["owner"]],"input":"json:ListInput"}})";
    }
    return R"({"box":"kc_locations","domains":["localhost"],)" + std::string(v2 ? R"("types":"types.json",)" : "") +
           R"("routes":{"mcp":{)" + tools + "}}}";
  };
  std::string types = R"({"openapi":"3.1.0","components":{"schemas":{
    "ListInput":{"type":"object","properties":{"q":{"type":"string"}},"additionalProperties":false},
    "Location":{"type":"object","required":["name","slug"],"properties":{"name":{"type":"string"},"slug":{"type":"string"}},"additionalProperties":false},
    "Slug":{"type":"object","required":["slug"],"properties":{"slug":{"type":"string"}},"additionalProperties":false}}}})";
  auto wat = [](bool v2) {
    std::string w = "(module (memory (export \"memory\") 1)";
    for (const char* n : {"list_locations", "add_location", "get_location", "import_locations"}) {
      if (!v2 && (std::string(n) == "get_location" || std::string(n) == "import_locations")) continue;
      w += std::string(" (func (export \"goober_route_") + n + "\") (result i32) (i32.const 0))";
    }
    return w + ")";
  };
  auto commit = [&](int base, bool v2) {
    bj::array changes{bj::object{{"path", "goober.project.json"}, {"text", R"({"boxes":["."]})"}},
                      bj::object{{"path", "manifest.json"}, {"text", manifest(v2)}},
                      bj::object{{"path", "box.wat"}, {"text", wat(v2)}}};
    if (v2) changes.push_back(bj::object{{"path", "types.json"}, {"text", types}});
    return tool("source_commit", {{"project", "loc"}, {"base_rev", base}, {"message", "rev"}, {"changes", changes}});
  };
  ASSERT_EQ(commit(0, false).at("rev"), 1);
  ASSERT_EQ(tool("source_deploy", {{"verbose", true}, {"project", "loc"}}).at("state"), "succeeded");
  ASSERT_EQ(commit(1, true).at("rev"), 2);
  auto deployed = tool("source_deploy", {{"verbose", true}, {"project", "loc"}});
  ASSERT_EQ(deployed.at("state"), "succeeded") << json::dump(deployed);
  std::string err;
  ASSERT_TRUE(host->identity().grant("playground", "owner@example.com", "owner", "test", 1'900'000'000'000, &err)) << err;
  auto preview = tool("mcp_tools_preview", {{"domain", "localhost"}, {"email", "owner@example.com"}});
  std::string version(host->boxes().active()->boxes.at("kc_locations")->version);
  std::set<std::string> listed;
  for (const auto& t : preview.at("tools").as_array()) {
    EXPECT_EQ(std::string(t.at("version").as_string()), version);
    EXPECT_TRUE(t.at("listed").as_bool()) << json::dump(t);
    EXPECT_FALSE(t.at("input").is_null()) << json::dump(t);
    listed.insert(std::string(t.at("name").as_string()));
  }
  EXPECT_EQ(listed, (std::set<std::string>{"add_location", "get_location", "import_locations", "list_locations"}));
}
