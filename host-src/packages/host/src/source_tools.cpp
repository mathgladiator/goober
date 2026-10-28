// Source control over the operator MCP (design/human/521.Source.md; design/machine/020.Source.md):
// projects an agent checks out, changes, builds, and deploys, kept in storage/source/.

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
  auto it = args.find(key);
  if (it == args.end()) return fallback;
  if (it->value().is_int64()) return it->value().as_int64();
  if (it->value().is_uint64()) return int64_t(it->value().as_uint64());
  return fallback;
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

bj::object job_json(const BuildJob& j) {
  bj::object o{{"job", j.id},     {"project", j.project}, {"rev", j.rev},     {"kind", j.kind},
               {"state", j.state}, {"by", j.author},       {"log", j.log}};
  o["queued_at"] = base::format_utc_ms(j.queued_ms);
  if (j.finished_ms) o["finished_at"] = base::format_utc_ms(j.finished_ms);
  for (const auto& kv : j.result) o[kv.key()] = kv.value();
  return o;
}

}  // namespace

void Host::register_source_tools() {
  auto cfg = config_.get();
  source_ = std::make_unique<source::Store>(cfg->storage / "source" / "source.db");
  jobs_ = std::make_unique<BuildJobs>();
  auto project_prop = mcp::string_property("The project's name, like ranches.");
  auto rev_prop = mcp::integer_property("A revision number; omit (or 0) for the newest.");

  mcp_->add_tool({"source_list",
                  "The source projects this host keeps: each with its newest revision, and which revision is deployed.",
                  mcp::object_schema({}, {}), [this](const bj::object&) {
                    bj::array out;
                    for (const auto& p : source_->list()) {
                      bj::object o{{"project", p.name}, {"description", p.description}, {"head", p.head},
                                   {"updated_at", base::format_utc_ms(p.updated_ms)}};
                      o["deployed_rev"] = p.deployed_rev ? bj::value(p.deployed_rev) : bj::value(nullptr);
                      if (p.deployed_ms) o["deployed_at"] = base::format_utc_ms(p.deployed_ms);
                      out.push_back(std::move(o));
                    }
                    return ToolResult::ok(std::move(out));
                  }});

  mcp_->add_tool(
      {"source_create",
       "Starts a source project kept in this host, so the code lives next to what it builds. Templates: empty; rust-box "
       "(a Rust box with Cargo.toml, src/lib.rs, the goober.rs binding, and a manifest); wat-box (WebAssembly text); "
       "site (static pages for a tenant). goober.project.json says what the project builds and deploys.",
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
       "max_bytes, default 200000), the rest listed for source_read. Remember the rev: commits name it as base_rev.",
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

  mcp_->add_tool({"source_read", "Chosen files of a project at a revision: text as text, others as base64.",
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
      {"source_commit",
       "Changes files in a project, as a new revision. base_rev is the revision you read (from source_checkout). If "
       "someone committed since, your commit still lands when it touches none of their files; otherwise it's refused "
       "with the conflicting paths, and you read them again. Each change is {path, text} or {path, base64} to write, or "
       "{path, delete: true}.",
       mcp::object_schema({{"project", project_prop},
                           {"base_rev", mcp::integer_property("The revision these changes start from.")},
                           {"message", mcp::string_property("What changed and why, in a sentence or two.")},
                           {"changes", bj::value(bj::object{{"type", "array"},
                                                            {"items", bj::object{{"type", "object"}}},
                                                            {"description", "Up to 500 changes."}})}},
                          {"project", "base_rev", "message", "changes"}),
       [this](const bj::object& args) {
         std::string name = arg(args, "project").value_or("");
         auto* list = args.if_contains("changes");
         if (!list || !list->is_array() || list->as_array().empty() || list->as_array().size() > 500) {
           return ToolResult::fail("changes is a list of 1 to 500 changes");
         }
         std::vector<source::Change> changes;
         for (const auto& c : list->as_array()) {
           const auto* o = c.if_object();
           auto path = o ? arg(*o, "path") : std::nullopt;
           if (!path) return ToolResult::fail("each change has a path");
           source::Change change{*path, std::nullopt};
           auto text = arg(*o, "text");
           auto b64 = arg(*o, "base64");
           bool del = o->contains("delete") && o->at("delete").is_bool() && o->at("delete").as_bool();
           if (int(bool(text)) + int(bool(b64)) + int(del) != 1) {
             return ToolResult::fail(*path + ": give exactly one of text, base64, or delete: true");
           }
           if (text) change.content = *text;
           if (b64) {
             auto bytes = base::base64_decode(*b64);
             if (!bytes) return ToolResult::fail(*path + ": base64 isn't valid");
             change.content = std::move(*bytes);
           }
           changes.push_back(std::move(change));
         }
         std::string err;
         std::vector<std::string> conflicts;
         auto r = source_->commit(name, int_arg(args, "base_rev", -1), changes, arg(args, "message").value_or(""),
                                  current_operator(), base::now_unix_ms(), &err, &conflicts);
         if (!r) {
           if (conflicts.empty()) return ToolResult::fail(err);
           bj::array paths;
           for (const auto& p : conflicts) paths.push_back(bj::value(p));
           return ToolResult::ok(bj::object{{"committed", false}, {"error", err}, {"conflicts", paths}});
         }
         return ToolResult::ok(bj::object{{"committed", true}, {"rev", r->rev}, {"rebased", r->rebased}});
       }});

  mcp_->add_tool({"source_log", "A project's revisions, newest first: number, message, who, when, and the files each changed.",
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
                  "What changed between two revisions of a project (to defaults to the newest), as a unified diff.",
                  mcp::object_schema({{"project", project_prop},
                                      {"from", mcp::integer_property("The older revision.")},
                                      {"to", rev_prop},
                                      {"paths", bj::value(bj::object{{"type", "array"},
                                                                     {"items", bj::object{{"type", "string"}}},
                                                                     {"description", "Only these files."}})}},
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
                    return ToolResult::ok(bj::value(d->empty() ? "(no differences)" : *d));
                  }});

  mcp_->add_tool({"source_revert", "A new revision holding an older revision's files: going back without losing history.",
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
         deploy ? "Builds a revision (newest by default) and deploys it: stores its schemas, uploads each box as a new "
                  "version, and writes each site's files. Returns once done, or after 25 s with a job to follow with "
                  "source_job."
                : "Builds a revision (newest by default) in the host's sandbox (no network; Rust crates from the host's "
                  "vendored set) and reports the log, compiler errors included. Returns once done, or after 25 s with a "
                  "job to follow with source_job.",
         mcp::object_schema({{"project", project_prop}, {"rev", rev_prop}}, {"project"}),
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
           return ToolResult::ok(job_json(*done));
         }});
  }

  mcp_->add_tool({"source_job", "A build or deploy job: its state, log, and what it did. Waits up to 20 s for it to finish.",
                  mcp::object_schema({{"job", mcp::integer_property("The job's number; omit for the recent jobs.")}}, {}),
                  [this](const bj::object& args) {
                    int64_t id = int_arg(args, "job", 0);
                    if (id == 0) {
                      bj::array out;
                      for (const auto& j : jobs_->recent()) {
                        auto o = job_json(j);
                        o.erase("log");
                        out.push_back(std::move(o));
                      }
                      return ToolResult::ok(std::move(out));
                    }
                    auto j = jobs_->get(id, 20000);
                    if (!j) return ToolResult::fail("no job " + std::to_string(id));
                    return ToolResult::ok(job_json(*j));
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
  if (!deploy) {
    bj::array boxes;
    for (const auto& b : built.boxes) boxes.push_back(bj::object{{"box", b.box}, {"bytes", int64_t(b.module.size())}});
    job.result["boxes"] = boxes;
    job.state = "succeeded";
    return;
  }
  // Schemas first, so the boxes that use them can be checked against them; then boxes; then sites.
  std::string err;
  bj::array schemas, boxes, sites;
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
      return;
    }
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
  job.result["schemas"] = schemas;
  job.result["boxes"] = boxes;
  job.result["sites"] = sites;
  job.log += "✓ revision " + std::to_string(job.rev) + " of " + job.project + " is live\n";
  job.state = "succeeded";
}

}  // namespace goober::host
