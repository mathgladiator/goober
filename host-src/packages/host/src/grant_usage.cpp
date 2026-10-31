#include "grant_usage.h"

#include "goober/base/time.h"

namespace goober::host {

namespace bj = boost::json;

void GrantUsage::record(const std::string& box, const std::string& version, const db::Usage& used, int64_t now_ms) {
  if (used.empty()) return;
  std::lock_guard lock(mu_);
  auto& mine = seen_[{box, version}];
  for (const auto& [strategy, tables] : used) {
    for (const auto& [table, use] : tables) {
      auto& s = mine[strategy][table];
      s.use.merge(use);
      ++s.calls;
      s.last_ms = now_ms;
    }
  }
}

GrantUsage::Seen2 GrantUsage::of(const std::string& box, const std::string& version) const {
  std::lock_guard lock(mu_);
  auto it = seen_.find({box, version});
  return it == seen_.end() ? Seen2{} : it->second;
}

namespace {

bj::array strings(const std::set<std::string>& s) {
  bj::array a;
  for (const auto& x : s) a.emplace_back(x);
  return a;
}

// A grant's columns as JSON: true, or the list.
bj::value columns(bool all, const std::set<std::string>& some) {
  if (all) return true;
  return strings(some);
}

std::string ops(const manifest::TableGrant& g) {
  std::string o;
  if (g.read_all || !g.read.empty()) o += "r";
  if (g.insert) o += "i";
  if (g.update_all || !g.update.empty()) o += "u";
  if (g.remove) o += "d";
  return o;
}

}  // namespace

bj::object grant_report(const manifest::Manifest& m, const std::string& version, const GrantUsage::Seen2& seen,
                        int64_t since_ms, const db::SchemaStore* schemas) {
  bj::array strategies;
  int findings_total = 0;
  for (const auto& st : m.data) {
    if (!st.runnable()) continue;
    std::optional<db::Schema> schema = schemas ? schemas->get(st.schema) : std::nullopt;
    auto seen_st = seen.find(st.name);
    bj::array tables;
    for (const auto& [table, g] : st.tables) {
      GrantUsage::Seen s;
      if (seen_st != seen.end()) {
        if (auto it = seen_st->second.find(table); it != seen_st->second.end()) s = it->second;
      }
      const db::Table* t = schema ? schema->table(table) : nullptr;
      size_t width = t ? t->columns.size() : 0;
      bj::array findings;
      auto finding = [&](const char* kind, std::string text, bj::object more = {}) {
        more["kind"] = kind;
        more["text"] = std::move(text);
        findings.push_back(std::move(more));
      };
      if (s.calls == 0) {
        finding("unused_table", "granted " + ops(g) + " on " + table + ", and no statement has touched it");
      } else {
        std::vector<std::string> unused;
        if (g.insert && !s.use.insert) unused.push_back("insert");
        if ((g.update_all || !g.update.empty()) && s.use.update.empty()) unused.push_back("update");
        if (g.remove && !s.use.remove) unused.push_back("delete");
        if (!unused.empty()) {
          std::string w;
          for (size_t i = 0; i < unused.size(); ++i) w += (i ? ", " : "") + unused[i];
          finding("unused_write", w + " granted on " + table + " and never done", {{"operations", bj::array(unused.begin(), unused.end())}});
        }
        auto never = [&](const std::set<std::string>& granted, const std::set<std::string>& used, const char* what) {
          std::set<std::string> idle;
          for (const auto& c : granted) {
            if (!used.count(c)) idle.insert(c);
          }
          if (!idle.empty()) {
            finding("unused_columns", std::string(what) + " granted on columns of " + table + " never " + what,
                    {{"operation", what}, {"columns", strings(idle)}});
          }
        };
        never(g.read, s.use.read, "read");
        never(g.update, s.use.update, "update");
        // `true` covers every column, now and to come; the columns used would do.
        if (g.read_all && !s.use.read.empty() && (width == 0 || s.use.read.size() < width)) {
          finding("narrow_read", "read: true on " + table + ", but only these columns were read",
                  {{"suggested", strings(s.use.read)}});
        }
        if (g.update_all && !s.use.update.empty() && (width == 0 || s.use.update.size() < width)) {
          finding("narrow_update", "update: true on " + table + ", but only these columns were updated",
                  {{"suggested", strings(s.use.update)}});
        }
      }
      findings_total += int(findings.size());
      bj::object granted{{"read", columns(g.read_all, g.read)},
                         {"insert", g.insert},
                         {"update", columns(g.update_all, g.update)},
                         {"delete", g.remove},
                         {"ops", ops(g)}};
      bj::object used{{"read", strings(s.use.read)},
                      {"insert", s.use.insert},
                      {"update", strings(s.use.update)},
                      {"delete", s.use.remove}};
      bj::object row{{"table", table}, {"granted", std::move(granted)}, {"used", std::move(used)},
                     {"calls", s.calls}, {"findings", std::move(findings)}};
      if (width) row["columns"] = int64_t(width);
      if (s.last_ms) row["last_at"] = base::format_utc_ms(s.last_ms);
      tables.push_back(std::move(row));
    }
    strategies.push_back(bj::object{{"strategy", st.name},
                                    {"schema", st.schema},
                                    {"database", st.database},
                                    {"tenant", st.tenant.empty() ? bj::value("{!tenant}") : bj::value(st.tenant)},
                                    {"tables", std::move(tables)}});
  }
  bj::array domains;
  for (const auto& d : m.domains) domains.emplace_back(d);
  return {{"box", m.box},
          {"version", version},
          {"domains", std::move(domains)},
          {"since", base::format_utc_ms(since_ms)},
          {"findings", findings_total},
          {"strategies", std::move(strategies)}};
}

std::vector<std::string> grant_advice(const manifest::Manifest& m, const db::SchemaStore& schemas) {
  std::vector<std::string> out;
  for (const auto& st : m.data) {
    if (!st.runnable()) continue;
    auto schema = schemas.get(st.schema);
    if (!schema) continue;  // a schema stored with this deploy: weighed on the next upload
    for (const auto& [table, g] : st.tables) {
      const db::Table* t = schema->table(table);
      if (!t || t->columns.size() <= kWideTable || !g.update_all) continue;
      out.push_back("data.sqlite3." + st.name + ".tables." + table + ": update: true lets the box change all " +
                    std::to_string(t->columns.size()) +
                    " columns, and any added later; list the columns it updates (grant_usage suggests them once it "
                    "has run)");
    }
  }
  return out;
}

}  // namespace goober::host
