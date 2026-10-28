// The host-owned /attachment/ endpoints (design/machine/007.Content.md §7): serving
// /attachment/$id.$ext, POST /attachment/upload, and the manager page at /attachment/.

#include <charconv>

#include "backups.h"
#include "goober/base/encoding.h"
#include "goober/base/time.h"
#include "goober/host/host.h"
#include "goober/json/json.h"
#include "goober/router/path.h"
#include "goober/server_http/multipart.h"
#include "http_util.h"

namespace goober::host {
namespace bj = boost::json;
using base::html_escape;
using server::http::Request;
using server::http::Response;

using server::http::parse_multipart;
using server::http::Upload;

namespace {

std::optional<int64_t> parse_id(std::string_view s) {
  int64_t n = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc() || end != s.data() + s.size() || n <= 0) return std::nullopt;
  return n;
}

constexpr const char* kManagerStyle = R"css(
:root{color-scheme:light dark;--fg:#1d1d1f;--bg:#f6f6f3;--muted:#6b6b6b;--card:#fff;--line:#ddd;--accent:#2457d6}
@media (prefers-color-scheme:dark){:root{--fg:#ececec;--bg:#141414;--muted:#9a9a9a;--card:#1e1e1e;--line:#333;--accent:#7aa2ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}
main{max-width:64rem;margin:0 auto;padding:24px 16px}h1{font-size:1.4rem;margin:0 0 4px}
.muted{color:var(--muted);font-size:.9rem}a{color:var(--accent)}
form.up{display:flex;flex-wrap:wrap;gap:8px;align-items:center;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin:16px 0}
form.up input[type=text]{flex:1;min-width:10rem;padding:8px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit}
button{padding:8px 14px;font:inherit;font-weight:600;border:0;border-radius:6px;background:var(--accent);color:#fff;cursor:pointer}
button.del{background:#c0392b}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(15rem,1fr));gap:12px}
.item{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;overflow:hidden}
.thumb{display:flex;align-items:center;justify-content:center;height:9rem;background:var(--bg);border-radius:8px;overflow:hidden}
.thumb img{max-width:100%;max-height:100%}
code{font-size:.8rem;word-break:break-all;display:block;margin:6px 0;user-select:all}
.unused{color:#b9770e;font-weight:600}
)css";

}  // namespace

void Host::handle_attachment(const Request& req, const Finish& finish, const std::string& path,
                             const std::string& tenant) {
  auto cfg = config_.get();
  std::string request_id = req.header("goober-request-id");
  int64_t now = base::now_unix_ms();
  std::string rest = path.size() > 12 ? path.substr(12) : "";  // after "/attachment/"
  std::optional<SignedIn> who = caller(req, tenant);
  std::string op = bearer_operator(req);
  // A one-time upload URL (attachment_upload_url) stands in for the operator who asked for it.
  std::optional<UploadTicket> ticket;
  if (rest == "upload" && (req.method == "put" || req.method == "post")) {
    auto q = router::parse_query(req.query);
    if (auto t = q.find("ticket"); t != q.end()) {
      ticket = upload_ticket(t->second, tenant, true);
      if (!ticket) {
        Response res;
        res.status = 403;
        res.headers.emplace_back("content-type", "application/json");
        res.body = R"({"error":"this upload URL is unknown, already used, expired, or for another site; ask attachment_upload_url for a new one"})";
        return finish(std::move(res), R"({"why":"bad upload ticket"})");
      }
      op = "ticket:" + ticket->by;
    }
  }
  bool is_editor = !op.empty() || (who && (who->host || std::find(who->roles.begin(), who->roles.end(), "owner") !=
                                                            who->roles.end()));
  bj::object extra{{"attachment", rest.empty() ? "manager" : rest}};
  if (who) extra["user_id"] = std::to_string(who->user.user_id);
  if (!op.empty()) extra["operator"] = op;
  bool is_get = req.method == "get" || req.method == "head";
  bool is_post = req.method == "post";
  // Cookies ride every request, so a POST that relies on one must come from this site.
  if (is_post && op.empty() && !same_origin(req, origin_of(req))) {
    return finish(error_page(tenant, 403, request_id), R"({"why":"cross-origin post"})");
  }

  // GET /attachment/$id.$ext
  if (is_get && !rest.empty() && rest.find('/') == std::string::npos) {
    std::string_view id_text = rest;
    std::string ext;
    if (auto dot = rest.find('.'); dot != std::string::npos) {
      id_text = std::string_view(rest).substr(0, dot);
      ext = rest.substr(dot + 1);
    }
    auto id = parse_id(id_text);
    auto a = id ? attachments_->get(tenant, *id) : std::nullopt;
    if (!a || (!ext.empty() && ext != a->ext)) return finish(error_page(tenant, 404, request_id), json::dump(extra));
    if (a->visibility == "members" && !who && op.empty()) return finish(sign_in_or_401(req, tenant), json::dump(extra));
    Response res;
    std::string etag = "\"" + a->sha256.substr(0, 16) + "\"";
    res.headers.emplace_back("etag", etag);
    res.headers.emplace_back("cache-control", std::string(a->visibility == "members" ? "private" : "public") +
                                                  ", max-age=31536000, immutable");
    res.headers.emplace_back("x-content-type-options", "nosniff");
    std::string filename = std::to_string(a->id) + "." + a->ext;
    if (a->inline_ok()) {
      res.headers.emplace_back("content-disposition", "inline; filename=\"" + filename + "\"");
    } else {
      // A browser can run SVG and PDF, so opened directly they download, and run sandboxed if they don't.
      res.headers.emplace_back("content-disposition", "attachment; filename=\"" + filename + "\"");
      res.headers.emplace_back("content-security-policy", "default-src 'none'; style-src 'unsafe-inline'; sandbox");
    }
    if (req.header("if-none-match") == etag) {
      res.status = 304;
      return finish(std::move(res), json::dump(extra));
    }
    auto bytes = attachments_->read(tenant, a->id);
    if (!bytes) return finish(error_page(tenant, 500, request_id), R"({"why":"attachment bytes missing"})");
    res.headers.emplace_back("content-type", a->content_type);
    res.body = std::move(*bytes);
    res.head_only = req.method == "head";
    return finish(std::move(res), json::dump(extra));
  }

  // Everything else is for the site's editors: owners, host operators, and operator tokens.
  if (!is_editor) {
    if (!who && op.empty()) return finish(sign_in_or_401(req, tenant), json::dump(extra));
    return finish(error_page(tenant, 403, request_id), R"({"why":"attachments need owner or a host operator"})");
  }
  bool from_browser = req.header("accept").find("text/html") != std::string::npos && op.empty();

  if ((is_post || (ticket && req.method == "put")) && rest == "upload") {
    std::string err;
    Upload up;
    std::string type = base::to_lower(req.header("content-type"));
    if (type.starts_with("multipart/form-data")) {
      auto parsed = parse_multipart(req.body, req.header("content-type"), &err);
      if (!parsed) return finish(error_page(tenant, 400, request_id), json::dump(bj::object{{"why", err}}));
      up = std::move(*parsed);
    } else {
      up.bytes = req.body;
      up.fields = router::parse_query(req.query);
      up.name = up.fields["name"];
    }
    if (ticket) {
      // The ticket's terms, as attachment_upload_url was asked.
      if (!ticket->name.empty()) up.name = ticket->name;
      up.fields["alt"] = ticket->alt;
      up.fields["visibility"] = ticket->visibility;
    }
    uint64_t max = op.empty() ? kBrowserAttachmentMaxBytes : kOperatorAttachmentMaxBytes;
    if (up.bytes.size() > max) {
      err = "the file is " + std::to_string(up.bytes.size() / 1024) + " KiB; uploads here take up to " +
            std::to_string(max >> 20) + " MiB";
      if (from_browser) return finish(error_page(tenant, 400, request_id), json::dump(bj::object{{"why", err}}));
      Response res;
      res.status = 413;
      res.headers.emplace_back("content-type", "application/json");
      res.body = json::dump(bj::object{{"error", err}});
      return finish(std::move(res), json::dump(bj::object{{"why", err}}));
    }
    auto a = attachments_->put(tenant, up.bytes, up.name, up.fields["alt"], up.fields["visibility"], now, &err);
    if (!a) {
      if (from_browser) return finish(error_page(tenant, 400, request_id), json::dump(bj::object{{"why", err}}));
      Response res;
      res.status = 400;
      res.headers.emplace_back("content-type", "application/json");
      res.body = json::dump(bj::object{{"error", err}});
      return finish(std::move(res), json::dump(bj::object{{"why", err}}));
    }
    extra["id"] = a->id;
    backups_->nudge();  // queued for the bucket at the next backup tick
    if (from_browser) {
      Response res;
      res.status = 303;
      res.headers.emplace_back("location", "/attachment/");
      return finish(std::move(res), json::dump(extra));
    }
    Response res;
    res.status = 201;
    res.headers.emplace_back("content-type", "application/json");
    res.headers.emplace_back("location", a->url());
    // What attachment_put answers, so an agent that uploaded by URL reads the same thing.
    res.body = json::dump(bj::object{{"id", a->id},
                                     {"url", a->url()},
                                     {"markdown", "![" + a->alt + "](" + a->url() + ")"},
                                     {"content_type", a->content_type},
                                     {"size", a->size},
                                     {"sha256", a->sha256},
                                     {"name", a->name},
                                     {"alt", a->alt},
                                     {"visibility", a->visibility}});
    return finish(std::move(res), json::dump(extra));
  }

  // POST /attachment/$id/delete, from the manager page.
  if (is_post && rest.ends_with("/delete")) {
    auto id = parse_id(std::string_view(rest).substr(0, rest.size() - 7));
    std::string err;
    if (!id || !attachments_->remove(tenant, *id, now, &err)) {
      return finish(error_page(tenant, 404, request_id), json::dump(extra));
    }
    Response res;
    res.status = 303;
    res.headers.emplace_back("location", "/attachment/");
    return finish(std::move(res), json::dump(extra));
  }

  // GET /attachment/: the manager.
  if (is_get && rest.empty()) {
    auto list = attachments_->list(tenant);
    auto refs = attachments::references(*content_, tenant);
    std::string items;
    for (const auto& a : list) {
      auto used = refs.find(a.id);
      std::string usage = used == refs.end() ? "<span class=\"unused\">unused</span>" : "used by ";
      if (used != refs.end()) {
        for (size_t i = 0; i < used->second.size(); ++i) {
          usage += (i ? ", " : "") + html_escape(used->second[i]);
        }
      }
      std::string thumb = a.ext == "pdf" ? "<span class=\"muted\">PDF</span>"
                                         : "<img src=\"" + a.url() + "\" alt=\"" + html_escape(a.alt) + "\">";
      items += "<div class=\"item\"><a class=\"thumb\" href=\"" + a.url() + "\">" + thumb + "</a>"
               "<code>" + a.url() + "</code><code>![" + html_escape(a.alt.empty() ? a.name : a.alt) + "](" + a.url() +
               ")</code><div class=\"muted\">" + html_escape(a.name) + " · " + std::to_string(a.size / 1024) + " KiB · " +
               a.visibility + " · " + usage + "</div>"
               "<form method=\"post\" action=\"/attachment/" + std::to_string(a.id) + "/delete\">"
               "<button class=\"del\">Delete</button></form></div>";
    }
    Response res;
    res.headers.emplace_back("content-type", "text/html; charset=utf-8");
    res.headers.emplace_back("cache-control", "no-store");
    res.headers.emplace_back("x-frame-options", "DENY");
    res.headers.emplace_back("referrer-policy", "same-origin");
    res.headers.emplace_back("content-security-policy",
                             "default-src 'none'; img-src 'self'; style-src 'unsafe-inline'; form-action 'self'; "
                             "frame-ancestors 'none'; base-uri 'none'");
    res.body = std::string("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                           "<title>Attachments</title><style>") +
               kManagerStyle + "</style></head><body><main><h1>Attachments</h1><p class=\"muted\">" +
               std::to_string(list.size()) + " for <strong>" + html_escape(tenant) + "</strong>. Up to " +
               std::to_string(kBrowserAttachmentMaxBytes >> 20) +
               " MiB each: PNG, JPEG, GIF, WebP, AVIF, ICO, SVG, or PDF. <a href=\"/identity/account\">Account</a></p>"
               "<form class=\"up\" method=\"post\" action=\"/attachment/upload\" enctype=\"multipart/form-data\">"
               "<input type=\"file\" name=\"file\" required accept=\"image/*,application/pdf\">"
               "<input type=\"text\" name=\"alt\" placeholder=\"Alt text: what the image shows\">"
               "<label class=\"muted\"><input type=\"checkbox\" name=\"visibility\" value=\"members\"> members only</label>"
               "<button>Upload</button></form><div class=\"grid\">" +
               items + "</div></main></body></html>\n";
    res.head_only = req.method == "head";
    return finish(std::move(res), json::dump(extra));
  }
  return finish(error_page(tenant, is_get || is_post ? 404 : 405, request_id), json::dump(extra));
}

}  // namespace goober::host
