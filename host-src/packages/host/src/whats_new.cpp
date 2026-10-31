// whats_new: what agents and engineers can do on this build, newest first, from
// host-src/CHANGES.md (built into the binary). Agents keep the tool list they fetched when
// they connected; after a deploy, this says what arrived.

#include <string>

#include "build_id.h"
#include "changes_doc.h"
#include "goober/host/host.h"

namespace goober::host {
namespace bj = boost::json;
using mcp::ToolResult;

namespace {

// The entries: {date, text} for each "- " line under a "## YYYY-MM-DD" heading.
bj::array changes_since(const std::string& since, const std::string& about) {
  bj::array out;
  std::string doc = kChangesDoc, date;
  size_t at = 0;
  while (at < doc.size()) {
    size_t end = doc.find('\n', at);
    if (end == std::string::npos) end = doc.size();
    std::string line = doc.substr(at, end - at);
    at = end + 1;
    if (line.starts_with("## ")) {
      date = line.substr(3);
      continue;
    }
    if (date.empty() || !line.starts_with("- ")) continue;
    if (!since.empty() && date < since) continue;
    std::string text = line.substr(2);
    if (!about.empty() && text.find(about) == std::string::npos) continue;
    out.push_back(bj::object{{"date", date}, {"text", text}});
  }
  return out;
}

}  // namespace

void Host::register_whats_new_tool() {
  mcp_->add_tool(
      {"whats_new",
       "Read tool. What agents and engineers can do on this host's build that they couldn't before, newest first: "
       "each entry a date and one line naming the tools to use. Call it after a deploy or when you reconnect: your "
       "tool list is the one you fetched when you connected, so a reconnect shows new tools, and this says what they "
       "are for. since (YYYY-MM-DD) keeps entries on or after that day (default: every entry); about keeps entries "
       "that mention a word, like a tool's name. Answers {build, entries: [{date, text}]}.",
       mcp::object_schema({{"since", mcp::string_property("A day, YYYY-MM-DD; entries on or after it.")},
                           {"about", mcp::string_property("A word to look for, like version_upload or cache.")}},
                          {}),
       [](const bj::object& args) {
         auto text = [&](const char* k) {
           const auto* v = args.if_contains(k);
           return v && v->is_string() ? std::string(v->as_string()) : std::string();
         };
         return ToolResult::ok(bj::object{{"build", kBuildCommit}, {"entries", changes_since(text("since"), text("about"))}});
       }});
}

}  // namespace goober::host
