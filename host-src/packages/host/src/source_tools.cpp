// Source control over the operator MCP (design/human/521.Source.md; design/machine/020.Source.md):
// projects an agent checks out, changes, builds, and deploys, kept in storage/source/.

#include <set>

#include "builder.h"
#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"

namespace goober::host {
namespace bj = boost::json;
namespace fs = std::filesystem;
using mcp::ToolResult;

namespace {

std::optional<std::string> arg(const bj::object& args, const char* key) {
  auto it = args.find(key);
  if (it == args.end() || !it->value().is_string()) return std::nullopt;
  return std::string(it->value().get_string());
}

int64_t int_arg(const bj::object& args, const char* key, int64_t fallback) {
  // A number, or a string of digits: agents told "ids are strings in JSON" send them either way.
  const auto* v = args.if_contains(key);
  if (!v) return fallback;
  if (v->is_int64()) return v->as_int64();
  if (v->is_uint64()) return int64_t(v->as_uint64());
  if (v->is_string()) {
    std::string_view t = v->as_string();
    bool neg = !t.empty() && t.front() == '-';
    if (neg) t.remove_prefix(1);
    if (t.empty() || t.size() > 18 || t.find_first_not_of("0123456789") != std::string_view::npos) return fallback;
    int64_t n = 0;
    for (char c : t) n = n * 10 + (c - '0');
    return neg ? -n : n;
  }
  return fallback;
}

// A commit's or a stage's changes: each {path, text}, {path, base64}, or {path, delete: true}.
bool parse_changes(const bj::array& list, std::vector<source::Change>* out, std::string* error) {
  for (const auto& c : list) {
    const auto* o = c.if_object();
    auto path = o ? arg(*o, "path") : std::nullopt;
    if (!path) return *error = "each change has a path", false;
    source::Change change{*path, std::nullopt};
    auto text = arg(*o, "text");
    auto b64 = arg(*o, "base64");
    bool del = o->contains("delete") && o->at("delete").is_bool() && o->at("delete").as_bool();
    if (int(bool(text)) + int(bool(b64)) + int(del) != 1) {
      return *error = *path + ": give exactly one of text, base64, or delete: true", false;
    }
    if (text) change.content = *text;
    if (b64) {
      auto bytes = base::base64_decode(*b64);
      if (!bytes) return *error = *path + ": base64 isn't valid", false;
      change.content = std::move(*bytes);
    }
    out->push_back(std::move(change));
  }
  return true;
}

// A file for an agent: text as text, anything else as base64.
bj::object file_json(const std::string& path, const std::string& bytes) {
  bj::object o{{"path", path}, {"size", int64_t(bytes.size())}};
  if (source::is_text(bytes)) {
    o["text"] = bytes;
  } else {
    o["base64"] = base::base64_encode(bytes);
  }
  return o;
}

// A job as a tool answers it: compact by default (031 § 3): the log only when it failed, or
// with verbose; otherwise its size and last lines.
bj::object job_json(const BuildJob& j, bool verbose = true) {
  bj::object o{{"job", j.id}, {"project", j.project}, {"rev", j.rev}, {"kind", j.kind}, {"state", j.state}, {"by", j.author}};
  if (verbose || j.state == "failed") {
    o["log"] = j.log;
  } else {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < j.log.size()) {
      size_t n = j.log.find('\n', i);
      if (n == std::string::npos) n = j.log.size();
      if (n > i) lines.push_back(j.log.substr(i, n - i));
      i = n + 1;
    }
    bj::array tail;
    for (size_t k = lines.size() > 3 ? lines.size() - 3 : 0; k < lines.size(); ++k) tail.push_back(bj::value(lines[k]));
    o["log_bytes"] = j.log.size();
    o["log_tail"] = std::move(tail);
  }
  o["queued_at"] = base::format_utc_ms(j.queued_ms);
  if (j.started_ms) o["started_at"] = base::format_utc_ms(j.started_ms);
  if (j.finished_ms) o["finished_at"] = base::format_utc_ms(j.finished_ms);
  for (const auto& kv : j.result) o[kv.key()] = kv.value();
  return o;
}

source::JobRecord record_of(const BuildJob& j) {
  return source::JobRecord{j.id,         j.project,     j.kind,      j.rev, j.state, j.author, j.queued_ms,
                           j.started_ms, j.finished_ms, j.log,       json::dump(j.result)};
}

BuildJob job_of(const source::JobRecord& r) {
  BuildJob j;
  j.id = r.id;
  j.project = r.project;
  j.kind = r.kind;
  j.rev = r.rev;
  j.state = r.state;
  j.author = r.author;
  j.queued_ms = r.queued_ms;
  j.started_ms = r.started_ms;
  j.finished_ms = r.finished_ms;
  j.log = r.log;
  std::string err;
  if (auto v = json::parse(r.result, &err); v && v->is_object()) j.result = v->as_object();
  return j;
}

bj::object deploy_json(const source::Deploy& d) {
  bj::array boxes;
  for (const auto& [box, version] : d.boxes) boxes.push_back(bj::object{{"box", box}, {"version", version}});
  return bj::object{{"project", d.project}, {"rev", d.rev},          {"message", d.message}, {"by", d.author},
                    {"at", base::format_utc_ms(d.at_ms)}, {"job", d.job}, {"boxes", std::move(boxes)}};
}

}  // namespace

void Host::register_source_tools() {
  auto cfg = config_.get();
  try {
    source_ = std::make_unique<source::Store>(cfg->storage / "source" / "source.db");
  } catch (const std::exception& e) {
    // Moved aside, kept whole for recovery: the projects in it are out of sight until it's back.
    stores_off_.push_back("source");
    set_aside(cfg->storage / "source" / "source.db", e.what());
    source_ = std::make_unique<source::Store>(cfg->storage / "source" / "source.db");
  }
  // Every finished job is kept with its log (020 § 9), and numbers go on from the newest kept.
  jobs_ = std::make_unique<BuildJobs>(source_->max_job_id(), [this](const BuildJob& j) { source_->save_job(record_of(j)); });
  auto project_prop = mcp::string_property("The project's name, like ranches: [a-z][a-z0-9_-]*.");
  auto rev_prop = mcp::integer_property("A revision number (1, 2, ...); omit (or 0) for the newest.");

  mcp_->add_tool({"source_list",
                  "The source projects this host keeps: each with its newest revision, which revision is deployed, "
                  "and how far the backup bucket's mirror of its history has come (mirror: patches through "
                  "patch_rev, snapshots of deployed revisions, and what's still pending). A read tool.",
                  mcp::object_schema({}, {}), [this](const bj::object&) {
                    bj::array out;
                    for (const auto& p : source_->list()) {
                      bj::object o{{"project", p.name}, {"description", p.description}, {"head", p.head},
                                   {"updated_at", base::format_utc_ms(p.updated_ms)}};
                      o["deployed_rev"] = p.deployed_rev ? bj::value(p.deployed_rev) : bj::value(nullptr);
                      if (p.deployed_ms) o["deployed_at"] = base::format_utc_ms(p.deployed_ms);
                      auto m = source_->mirror_state(p.name);
                      o["mirror"] = bj::object{{"patches", m.patches},     {"patch_rev", m.patch_rev},
                                               {"snapshots", m.snapshots}, {"snapshot_rev", m.snapshot_rev},
                                               {"pending", m.pending}};
                      out.push_back(std::move(o));
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"source_tree",
                  "Every file of a project at a revision, without contents (path, size, sha256), with the revision's "
                  "message, author, time, and the files it changed. Read contents with source_read. A read tool.",
                  mcp::object_schema({{"project", project_prop}, {"rev", rev_prop}}, {"project"}),
                  [this](const bj::object& args) {
                    std::string name = arg(args, "project").value_or("");
                    auto project = source_->get(name);
                    if (!project) return ToolResult::fail("no project named " + name);
                    int64_t rev = int_arg(args, "rev", 0);
                    if (rev == 0) rev = project->head;
                    std::string err;
                    auto files = source_->files(name, rev, &err);
                    if (!files) return ToolResult::fail(err);
                    bj::array out;
                    for (const auto& f : *files) out.push_back(bj::object{{"path", f.path}, {"size", f.size}, {"sha256", f.sha256}});
                    bj::object result{{"project", name}, {"rev", rev}, {"head", project->head}, {"files", std::move(out)}};
                    if (auto r = source_->revision(name, rev)) {
                      bj::array changed;
                      for (const auto& p : r->changed) changed.push_back(bj::value(p));
                      result["message"] = r->message;
                      result["by"] = r->author;
                      result["at"] = base::format_utc_ms(r->at_ms);
                      result["changed"] = std::move(changed);
                    }
                    return ToolResult::ok(std::move(result));
                  }});

  mcp_->add_tool({"source_box_history",
                  "Where a box's versions came from: each source deploy that uploaded it, newest first, with the "
                  "project, revision, its message, who deployed it, when, the job (its build log: source_job), and "
                  "the version. Versions uploaded with version_upload have no entry. A read tool.",
                  mcp::object_schema({{"box", mcp::string_property("The box's name, as box_list shows it.")},
                                      {"limit", mcp::integer_property("How many (default 50).")}},
                                     {"box"}),
                  [this](const bj::object& args) {
                    std::string box = arg(args, "box").value_or("");
                    bj::array out;
                    for (const auto& d : source_->deploys_of_box(box, int(int_arg(args, "limit", 50)))) {
                      auto o = deploy_json(d);
                      for (const auto& [b, version] : d.boxes) {
                        if (b == box) o["version"] = version;
                      }
                      out.push_back(std::move(o));
                    }
                    return ToolResult::ok(bj::object{{"box", box}, {"deploys", std::move(out)}});
                  }});

  mcp_->add_tool(
      {"source_create",
       "Starts a source project kept in this host, so the code lives next to what it builds. Templates: empty; rust-box "
       "(a Rust box with Cargo.toml, src/lib.rs, the goober.rs binding, and a manifest); wat-box (WebAssembly text); "
       "site (static pages for a tenant). goober.project.json says what the project builds and deploys: boxes "
       "(directories), schemas (files the build checks in place and the deploy stores), specs, and sites. Answers "
       "{project, head, next}. Nothing is built or deployed until source_build or source_deploy. A changing tool.",
       mcp::object_schema({{"project", project_prop},
                           {"description", mcp::string_property("What the project is for.")},
                           {"template", mcp::enum_property("Files to start with (default empty).",
                                                           {"empty", "rust-box", "wat-box", "site"})},
                           {"tenant", mcp::string_property("For the site template: the tenant it serves.")}},
                          {"project"}),
       [this](const bj::object& args) {
         std::string name = arg(args, "project").value_or("");
         std::string err;
         auto files = template_files(arg(args, "template").value_or("empty"), name, arg(args, "tenant").value_or(""), &err);
         if (!files) return ToolResult::fail(err);
         int64_t now = base::now_unix_ms();
         if (!source_->create(name, arg(args, "description").value_or(""), now, &err)) return ToolResult::fail(err);
         int64_t head = 0;
         if (!files->empty()) {
           auto r = source_->commit(name, 0, *files, "Start from the " + *arg(args, "template") + " template",
                                    current_operator(), now, &err);
           if (!r) return ToolResult::fail(err);
           head = r->rev;
         }
         return ToolResult::ok(bj::object{{"project", name}, {"head", head},
                                          {"next", "source_checkout to read it; source_commit with base_rev " +
                                                       std::to_string(head) + " to change it"}});
       }});

  mcp_->add_tool(
      {"source_checkout",
       "Every file of a project at a revision: text files with their contents in path order (until "
       "max_bytes, default 200000), the rest listed for source_read. Answers {project, rev, head, files: [{path, "
       "size, sha256, text | binary | omitted}]}, plain JSON. Remember the rev: commits name it as base_rev. A read "
       "tool.",
       mcp::object_schema({{"project", project_prop}, {"rev", rev_prop},
                           {"max_bytes", mcp::integer_property("How much file text to include (default 200000, at most 2000000).")}},
                          {"project"}),
       [this](const bj::object& args) {
         std::string name = arg(args, "project").value_or("");
         std::string err;
         auto project = source_->get(name);
         if (!project) return ToolResult::fail("no project named " + name);
         int64_t rev = int_arg(args, "rev", 0);
         if (rev == 0) rev = project->head;
         auto files = source_->files(name, rev, &err);
         if (!files) return ToolResult::fail(err);
         int64_t budget = std::clamp<int64_t>(int_arg(args, "max_bytes", 200000), 0, 2000000);
         bj::array out;
         int64_t omitted = 0;
         for (const auto& f : *files) {
           bj::object o{{"path", f.path}, {"size", f.size}, {"sha256", f.sha256}};
           if (f.size <= budget) {
             auto bytes = source_->read(name, rev, f.path, &err);
             if (bytes && source::is_text(*bytes)) {
               o["text"] = *bytes;
               budget -= f.size;
             } else {
               o["binary"] = true;
             }
           } else {
             o["omitted"] = true;
             ++omitted;
           }
           out.push_back(std::move(o));
         }
         bj::object result{{"project", name}, {"rev", rev}, {"head", project->head}, {"files", out}};
         if (omitted) result["note"] = std::to_string(omitted) + " files were left out for size; read them with source_read";
         return ToolResult::ok(std::move(result));
       }});

  mcp_->add_tool({"source_read",
                  "Chosen files of a project at a revision (up to 50 paths): text as text, others as base64. A read "
                  "tool.",
                  mcp::object_schema({{"project", project_prop}, {"rev", rev_prop},
                                      {"paths", bj::value(bj::object{{"type", "array"},
                                                                     {"items", bj::object{{"type", "string"}}},
                                                                     {"description", "Up to 50 paths."}})}},
                                     {"project", "paths"}),
                  [this](const bj::object& args) {
                    std::string name = arg(args, "project").value_or("");
                    auto* paths = args.if_contains("paths");
                    if (!paths || !paths->is_array() || paths->as_array().size() > 50) {
                      return ToolResult::fail("paths is a list of up to 50 paths");
                    }
                    bj::array out;
                    for (const auto& p : paths->as_array()) {
                      if (!p.is_string()) continue;
                      std::string err;
                      auto bytes = source_->read(name, int_arg(args, "rev", 0), std::string(p.as_string()), &err);
                      if (!bytes) return ToolResult::fail(err);
                      out.push_back(file_json(std::string(p.as_string()), *bytes));
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool(
      {"source_stage",
       "Builds one commit over several calls, when a client can't send every file in one: adds changes (each {path, "
       "text}, {path, base64}, or {path, delete: true}; a later one for a path replaces an earlier) to your draft on a "
       "project, made from base_rev. Nothing is a revision until source_commit with the stage id commits it all at "
       "once, so no half-done revision exists. A draft is yours (your operator sign-in), holds up to 500 changes, and "
       "is dropped by a commit, by discard: true, or after an hour without a change; it survives a restart. "
       "Answers the stage id and the draft's paths. Stage on the base_rev you read; after a conflict, discard, read "
       "again, and stage on the new head. A changing tool.",
       mcp::object_schema({{"project", project_prop},
                           {"base_rev", mcp::integer_property("The revision the draft starts from (from source_checkout).")},
                           {"changes", bj::value(bj::object{{"type", "array"},
                                                            {"items", bj::object{{"type", "object"}}},
                                                            {"description", "Up to 500 changes."}})},
                           {"discard", mcp::boolean_property("Drop your draft on this project.")}},
                          {"project"}),
       [this](const bj::object& args) {
         std::string name = arg(args, "project").value_or("");
         if (!source_->get(name)) return ToolResult::fail("no project named " + name);
         std::string op = current_operator();
         auto discard = args.if_contains("discard");
         if (discard && discard->is_bool() && discard->as_bool()) {
           bool had = source_->draft(name, op, base::now_unix_ms()).has_value();
           source_->discard(name, op);
           return ToolResult::ok(bj::object{{"project", name}, {"discarded", had}});
         }
         auto* list = args.if_contains("changes");
         if (!list || !list->is_array() || list->as_array().empty() || list->as_array().size() > 500) {
           return ToolResult::fail("changes is a list of 1 to 500 changes");
         }
         if (!args.contains("base_rev")) return ToolResult::fail("base_rev is the revision the draft starts from");
         std::vector<source::Change> changes;
         std::string err;
         if (!parse_changes(list->as_array(), &changes, &err)) return ToolResult::fail(err);
         auto draft = source_->stage(name, op, int_arg(args, "base_rev", -1), changes, base::now_unix_ms(), &err);
         if (!draft) return ToolResult::fail(err);
         bj::array files;
         for (const auto& c : draft->changes) {
           files.push_back(c.content ? bj::object{{"path", c.path}, {"bytes", int64_t(c.content->size())}}
                                     : bj::object{{"path", c.path}, {"delete", true}});
         }
         return ToolResult::ok(bj::object{
             {"project", name},
             {"stage", draft->id},
             {"base_rev", draft->base_rev},
             {"files", std::move(files)},
             {"expires_at", base::format_utc_ms(draft->updated_ms + source::kDraftTtlMs)},
             {"next", "stage more, or source_commit {project, base_rev: " + std::to_string(draft->base_rev) +
                          ", message, stage: \"" + draft->id + "\"}"}});
       }});

  mcp_->add_tool(
      {"source_commit",
       "Changes files in a project, as a new revision. base_rev is the revision you read (from source_checkout). If "
       "someone committed since, your commit still lands when it touches none of their files; otherwise it's refused "
       "with the conflicting paths, and you read them again. Each change is {path, text} or {path, base64} to write, or "
       "{path, delete: true}. Too many files for one call? source_stage them, then commit with the stage id. "
       "Answers {committed, rev, rebased} or the conflicts. A commit only stores source: nothing is built or goes "
       "live until source_build or source_deploy. Its pin is the revision number (source commits aren't in the "
       "change log, so there is no change_id). A changing tool.",
       mcp::object_schema({{"project", project_prop},
                           {"base_rev", mcp::integer_property("The revision these changes start from.")},
                           {"message", mcp::string_property("What changed and why, in a sentence or two.")},
                           {"agent", mcp::string_property("Which agent is committing, like \"Engineer A\": recorded "
                                                          "beside the operator you signed in as, as said, unchecked.")},
                           {"changes", bj::value(bj::object{{"type", "array"},
                                                            {"items", bj::object{{"type", "object"}}},
                                                            {"description", "Up to 500 changes."}})},
                           {"stage", mcp::string_property("A draft from source_stage: its changes commit whole, with any "
                                                          "changes given here on top.")}},
                          {"project", "base_rev", "message"}),
       [this](const bj::object& args) {
         std::string name = arg(args, "project").value_or("");
         // A staged draft (source_stage) commits whole, with any changes here on top of it.
         std::optional<source::Draft> draft;
         if (auto stage = arg(args, "stage"); stage && !stage->empty()) {
           draft = source_->draft(name, current_operator(), base::now_unix_ms());
           if (!draft || draft->id != *stage) {
             return ToolResult::fail("no draft " + *stage + " of yours on " + name +
                                     ": it was committed, discarded, or an hour passed without a change; stage again");
           }
           if (draft->base_rev != int_arg(args, "base_rev", -1)) {
             return ToolResult::fail("the draft started from revision " + std::to_string(draft->base_rev) +
                                     "; commit it with that base_rev");
           }
         }
         auto* list = args.if_contains("changes");
         bool none = !list || (list->is_array() && list->as_array().empty());
         if ((none && !draft) || (list && (!list->is_array() || list->as_array().size() > 500))) {
           return ToolResult::fail("changes is a list of 1 to 500 changes (or give a stage from source_stage)");
         }
         std::vector<source::Change> changes;
         if (!none) {
           std::string why;
           if (!parse_changes(list->as_array(), &changes, &why)) return ToolResult::fail(why);
         }
         if (draft) {
           std::map<std::string, source::Change> merged;
           for (auto& c : draft->changes) merged[c.path] = c;
           for (auto& c : changes) merged[c.path] = std::move(c);
           changes.clear();
           for (auto& [path, c] : merged) changes.push_back(std::move(c));
           if (changes.size() > source::kMaxCommitChanges) return ToolResult::fail("a commit holds at most 500 changes");
         }
         // Agents sharing one operator sign-in name themselves; the name is theirs to say, so the
         // log shows it as said, after the operator the host checked.
         std::string author = current_operator();
         if (auto agent = arg(args, "agent"); agent && !agent->empty()) {
           if (agent->size() > 60 || std::any_of(agent->begin(), agent->end(), [](char c) {
                 return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
               })) {
             return ToolResult::fail("agent is a name of up to 60 printable characters");
           }
           author += " (agent: " + *agent + ")";
         }
         std::string err;
         std::vector<std::string> conflicts;
         auto r = source_->commit(name, int_arg(args, "base_rev", -1), changes, arg(args, "message").value_or(""),
                                  author, base::now_unix_ms(), &err, &conflicts);
         if (!r) {
           if (conflicts.empty()) return ToolResult::fail(err);
           bj::array paths;
           for (const auto& p : conflicts) paths.push_back(bj::value(p));
           bj::object out{{"committed", false}, {"error", err}, {"conflicts", paths}};
           if (draft) out["stage"] = draft->id;  // kept: read the conflicting files, stage them again, commit
           return ToolResult::ok(std::move(out));
         }
         if (draft) source_->discard(name, current_operator());
         return ToolResult::ok(bj::object{{"committed", true}, {"rev", r->rev}, {"rebased", r->rebased}});
       }});

  mcp_->add_tool({"source_log",
                  "A project's revisions, newest first: number, message, who, when, and the files each changed. A "
                  "read tool.",
                  mcp::object_schema({{"project", project_prop}, {"limit", mcp::integer_property("How many (default 20).")}},
                                     {"project"}),
                  [this](const bj::object& args) {
                    std::string name = arg(args, "project").value_or("");
                    if (!source_->get(name)) return ToolResult::fail("no project named " + name);
                    bj::array out;
                    for (const auto& r : source_->log(name, int(int_arg(args, "limit", 20)))) {
                      bj::array changed;
                      for (const auto& p : r.changed) changed.push_back(bj::value(p));
                      out.push_back(bj::object{{"rev", r.rev}, {"message", r.message}, {"by", r.author},
                                               {"at", base::format_utc_ms(r.at_ms)}, {"changed", changed}});
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"source_diff",
                  "What changed between two revisions of a project (to defaults to the newest): each file with lines "
                  "added and removed, and with verbose the unified diff. A read tool.",
                  mcp::object_schema({{"project", project_prop},
                                      {"from", mcp::integer_property("The older revision.")},
                                      {"to", rev_prop},
                                      {"paths", bj::value(bj::object{{"type", "array"},
                                                                     {"items", bj::object{{"type", "string"}}},
                                                                     {"description", "Only these files."}})},
                                      {"verbose", mcp::boolean_property("Include the unified diff.")}},
                                     {"project", "from"}),
                  [this](const bj::object& args) {
                    std::vector<std::string> paths;
                    if (auto* p = args.if_contains("paths"); p && p->is_array()) {
                      for (const auto& v : p->as_array()) {
                        if (v.is_string()) paths.emplace_back(v.as_string());
                      }
                    }
                    std::string err;
                    auto d = source_->diff(arg(args, "project").value_or(""), int_arg(args, "from", -1),
                                           int_arg(args, "to", 0), paths, &err);
                    if (!d) return ToolResult::fail(err);
                    // A summary per file; the diff itself with verbose (031 § 3).
                    bj::array files;
                    bj::object* cur = nullptr;
                    int64_t added = 0, removed = 0;
                    size_t i = 0;
                    while (i < d->size()) {
                      size_t n = d->find('\n', i);
                      if (n == std::string::npos) n = d->size();
                      std::string_view line(d->data() + i, n - i);
                      i = n + 1;
                      if (line.starts_with("+++ ")) {
                        std::string path(line.substr(4));
                        if (path.starts_with("b/")) path = path.substr(2);
                        files.push_back(bj::object{{"path", path}, {"added", 0}, {"removed", 0}});
                        cur = &files.back().as_object();
                      } else if (line.starts_with("--- ")) {
                        continue;
                      } else if (cur && line.starts_with("+")) {
                        cur->at("added") = cur->at("added").as_int64() + 1;
                        ++added;
                      } else if (cur && line.starts_with("-")) {
                        cur->at("removed") = cur->at("removed").as_int64() + 1;
                        ++removed;
                      }
                    }
                    bj::object out{{"files", std::move(files)}, {"added", added}, {"removed", removed}};
                    auto vb = args.if_contains("verbose");
                    if (vb && vb->is_bool() && vb->as_bool()) out["diff"] = d->empty() ? "(no differences)" : *d;
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool({"source_revert",
                  "A new revision holding an older revision's files: going back without losing history. Answers "
                  "{rev}. It changes source only: source_deploy that revision to put it live. A changing tool.",
                  mcp::object_schema({{"project", project_prop}, {"rev", mcp::integer_property("The revision to go back to.")}},
                                     {"project", "rev"}),
                  [this](const bj::object& args) {
                    std::string err;
                    auto r = source_->revert(arg(args, "project").value_or(""), int_arg(args, "rev", 0), current_operator(),
                                             base::now_unix_ms(), &err);
                    if (!r) return ToolResult::fail(err);
                    return ToolResult::ok(bj::object{{"rev", *r}});
                  }});

  for (bool deploy : {false, true}) {
    mcp_->add_tool(
        {deploy ? "source_deploy" : "source_build",
         deploy ? "Builds a revision (newest by default) and puts it live: first every check a build runs, and if "
                  "any fails nothing is stored; then it stores the project's schemas for good, uploads each box as a "
                  "new version that goes live at once, and writes each site's files. Answers the job: its state, log, "
                  "and the schemas, box versions, and sites it deployed. Returns once done, or after 25 s with a job to "
                  "follow with source_job. To check without deploying, source_build. A changing tool."
                : "Builds a revision (newest by default) in the host's sandbox (no network; Rust crates from the host's "
                  "vendored set) and checks it exactly as source_deploy would, storing nothing and changing nothing: "
                  "the project's own schemas (those goober.project.json lists) are checked in place, unstored, and "
                  "each box as an upload would be, conflicts with live boxes included. A green build deploys. Answers "
                  "the job: its state and log, compiler errors included. Returns once done, or after 25 s with a job "
                  "to follow with source_job. A changing tool (it runs a build job).",
         mcp::object_schema({{"project", project_prop}, {"rev", rev_prop},
                             {"verbose", mcp::boolean_property("The whole log even when it succeeds.")}},
                            {"project"}),
         [this, deploy](const bj::object& args) {
           std::string name = arg(args, "project").value_or("");
           auto project = source_->get(name);
           if (!project) return ToolResult::fail("no project named " + name);
           int64_t rev = int_arg(args, "rev", 0);
           if (rev == 0) rev = project->head;
           if (rev < 1 || rev > project->head) return ToolResult::fail(name + " has no revision " + std::to_string(rev));
           BuildJob job;
           job.project = name;
           job.rev = rev;
           job.kind = deploy ? "deploy" : "build";
           job.author = current_operator();
           int64_t id = jobs_->submit(job, [this, deploy](BuildJob& j) { run_source_job(j, deploy); });
           auto done = jobs_->get(id, 25000);
           if (!done) return ToolResult::fail("the job vanished");
           auto vb = args.if_contains("verbose");
           return ToolResult::ok(job_json(*done, vb && vb->is_bool() && vb->as_bool()));
         }});
  }

  mcp_->add_tool({"source_job",
                  "A build or deploy job: its state (queued, running, succeeded, failed), log, and what it did. Waits "
                  "up to 20 s for it to finish. Without job, the recent jobs (of one project, with project), newest "
                  "first; every finished job is kept, with its log. Not the same as job_status, which follows a "
                  "box's one-time job (routes.job). A read tool.",
                  mcp::object_schema({{"job", mcp::integer_property("The job's number; omit for the recent jobs.")},
                                      {"project", project_prop},
                                      {"limit", mcp::integer_property("How many recent jobs (default 20).")},
                                      {"verbose", mcp::boolean_property("The whole log even when it succeeded.")}},
                                     {}),
                  [this](const bj::object& args) {
                    int64_t id = int_arg(args, "job", 0);
                    if (id == 0) {
                      std::string project = arg(args, "project").value_or("");
                      int limit = int(std::clamp<int64_t>(int_arg(args, "limit", 20), 1, 200));
                      bj::array out;
                      std::set<int64_t> seen;
                      // Queued and running ones live in memory; the finished, in the store.
                      for (const auto& j : jobs_->recent()) {
                        if ((j.state != "queued" && j.state != "running") || (!project.empty() && j.project != project)) continue;
                        auto o = job_json(j);
                        o.erase("log");
                        seen.insert(j.id);
                        out.push_back(std::move(o));
                      }
                      for (const auto& r : source_->jobs(project, limit)) {
                        if (int(out.size()) >= limit) break;
                        if (seen.count(r.id)) continue;
                        auto o = job_json(job_of(r));
                        o.erase("log");
                        out.push_back(std::move(o));
                      }
                      return ToolResult::ok(std::move(out));
                    }
                    auto j = jobs_->get(id, 20000);
                    if (!j) {
                      if (auto kept = source_->job(id)) j = job_of(*kept);
                    }
                    if (!j) return ToolResult::fail("no job " + std::to_string(id));
                    auto vb = args.if_contains("verbose");
                    return ToolResult::ok(job_json(*j, vb && vb->is_bool() && vb->as_bool()));
                  }});
}

void Host::run_source_job(BuildJob& job, bool deploy) {
  auto cfg = config_.get();
  auto built = build_project(*source_, job.project, job.rev, *cfg, cfg->storage / "tmp" / ("source-build-" + std::to_string(job.id)),
                             cfg->storage / "source" / "cache" / job.project);
  job.log = built.log;
  if (!built.ok) {
    job.state = "failed";
    return;
  }
  // Specs first: content-addressed and shared, so storing one changes nothing a box uses until a
  // manifest pins it; the log says each pin, to write into the manifest (034 § 2).
  for (const auto& [name, text] : built.specs) {
    std::string serr;
    auto pin = specs_->put(name, text, &serr);
    job.log += pin ? "✓ spec " + name + " is " + *pin + "\n" : "✗ spec " + name + ": " + serr + "\n";
    if (!pin) {
      job.state = "failed";
      return;
    }
  }
  // Everything is checked before anything is stored: each schema (new, or the same as the one
  // stored under its name), then each box against those schemas and the active boxes, exactly
  // as an upload would check it. A build runs the same checks, so a green build deploys.
  std::string err;
  {
    std::map<std::string, db::Schema> pending;
    bool ok = true;
    for (const auto& [file, text] : built.schemas) {
      auto s = schemas_->check(text, &err);
      if (!s) {
        job.log += "✗ schema " + file + ": " + err + "\n";
        ok = false;
        continue;
      }
      for (const auto& w : s->warnings) job.log += "! schema " + file + ": " + w + "\n";
      pending[s->name] = *s;
    }
    pending_schemas_ = &pending;
    struct Clear {
      ~Clear() { pending_schemas_ = nullptr; }
    } clear;
    for (const auto& b : built.boxes) {
      if (!boxes_->check(b.manifest, b.module, b.types, &err)) {
        job.log += "✗ box " + b.box + ": " + err + "\n";
        ok = false;
      } else {
        job.log += "✓ box " + b.box + " checks out\n";
      }
    }
    if (!ok) {
      job.log += deploy ? "nothing was stored or deployed: fix the above, commit, and deploy again\n"
                        : "the build would fail to deploy: fix the above\n";
      job.state = "failed";
      return;
    }
  }
  if (!deploy) {
    bj::array boxes;
    for (const auto& b : built.boxes) boxes.push_back(bj::object{{"box", b.box}, {"bytes", int64_t(b.module.size())}});
    job.result["boxes"] = boxes;
    job.state = "succeeded";
    return;
  }
  // Schemas first, so the boxes that use them can be checked against them; then boxes; then sites.
  bj::array schemas, boxes, sites;
  // What went live, kept so each box version leads back to this revision (020 § 9).
  source::Deploy deployed;
  deployed.job = job.id;
  deployed.project = job.project;
  deployed.rev = job.rev;
  deployed.at_ms = base::now_unix_ms();
  deployed.author = job.author;
  for (const auto& [file, text] : built.schemas) {
    auto s = schemas_->put(text, &err);
    if (!s) {
      job.log += "✗ schema " + file + ": " + err + "\n";
      job.state = "failed";
      return;
    }
    schemas.push_back(bj::value(s->name));
    job.log += "stored schema " + s->name + "\n";
  }
  for (const auto& b : built.boxes) {
    auto r = boxes_->upload(b.manifest, b.module, b.types, &err);
    if (!r) {
      job.log += "✗ box " + b.box + ": " + err + "\n";
      job.state = "failed";
      job.result["schemas"] = schemas;
      job.result["boxes"] = boxes;
      // The boxes uploaded before this one are live: they came from this revision all the same.
      if (!deployed.boxes.empty()) source_->record_deploy(deployed);
      return;
    }
    deployed.boxes.emplace_back(b.box, r->version);
    bj::array warnings;
    for (const auto& w : r->warnings) warnings.push_back(bj::value(w));
    boxes.push_back(bj::object{{"box", b.box}, {"version", r->version}, {"warnings", warnings}});
    job.log += "deployed box " + b.box + " as version " + r->version + "\n";
  }
  for (const auto& site : built.sites) {
    int written = 0;
    for (const auto& [path, bytes] : site.files) {
      if (!content_->put(site.tenant, path, bytes, &err)) {
        job.log += "✗ " + site.tenant + "/" + path + ": " + err + "\n";
        job.state = "failed";
        return;
      }
      ++written;
    }
    sites.push_back(bj::object{{"tenant", site.tenant}, {"files", written}});
    job.log += "wrote " + std::to_string(written) + " files for " + site.tenant + "\n";
  }
  source_->set_deployed(job.project, job.rev, base::now_unix_ms());
  source_->record_deploy(deployed);
  job.result["schemas"] = schemas;
  job.result["boxes"] = boxes;
  job.result["sites"] = sites;
  job.log += "✓ revision " + std::to_string(job.rev) + " of " + job.project + " is live\n";
  job.state = "succeeded";
}

}  // namespace goober::host
