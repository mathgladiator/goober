#pragma once

// A stand-in for S3 and SES in tests: path-style buckets on 127.0.0.1, every request's
// SigV4 signature checked against the one secret it knows, objects kept in memory, and
// listings a few keys per page so paging is exercised.

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "goober/aws/aws.h"
#include "goober/base/encoding.h"
#include "goober/base/hash.h"
#include "goober/json/json.h"
#include "goober/server_http/server.h"

namespace goober::aws::testing {

class FakeAws {
 public:
  explicit FakeAws(Credentials creds, size_t page_size = 3) : creds_(std::move(creds)), page_size_(page_size) {
    server::http::ServerOptions o;
    o.port = 0;
    o.body_max_bytes = 256ull << 20;
    server_ = std::make_unique<server::http::Server>(o, [this](server::http::Request req, server::http::Responder respond) {
      respond(handle(req));
    });
    server_->start();
  }
  ~FakeAws() { server_->stop(); }

  std::string endpoint() const { return "http://127.0.0.1:" + std::to_string(server_->port()); }

  std::map<std::string, std::string> objects(const std::string& bucket) {
    std::lock_guard lock(mu_);
    return buckets_[bucket];
  }
  void put_object(const std::string& bucket, const std::string& key, std::string bytes) {
    std::lock_guard lock(mu_);
    buckets_[bucket][key] = std::move(bytes);
  }
  std::vector<std::string> mails() {
    std::lock_guard lock(mu_);
    return mails_;
  }
  // The next n requests get this status (a 503 to test retries, a 400 to test refusals).
  void fail_next(int n, int status) {
    std::lock_guard lock(mu_);
    fail_left_ = n;
    fail_status_ = status;
  }
  int refused_signatures() {
    std::lock_guard lock(mu_);
    return bad_signatures_;
  }
  int requests() {
    std::lock_guard lock(mu_);
    return requests_;
  }

 private:
  static std::string field(const std::string& auth, const std::string& name) {
    auto at = auth.find(name + "=");
    if (at == std::string::npos) return "";
    at += name.size() + 1;
    auto end = auth.find(',', at);
    return auth.substr(at, end == std::string::npos ? std::string::npos : end - at);
  }

  // Recomputes the signature from what arrived; "" when it holds.
  std::string check_signature(const server::http::Request& req) {
    std::string auth = req.header("authorization");
    if (!auth.starts_with("AWS4-HMAC-SHA256 ")) return "no SigV4 authorization";
    std::string credential = field(auth, "Credential");
    std::string signed_names = field(auth, "SignedHeaders");
    std::string signature = field(auth, "Signature");
    std::vector<std::string> scope;
    for (size_t a = 0, b; a <= credential.size(); a = b + 1) {
      b = credential.find('/', a);
      if (b == std::string::npos) b = credential.size();
      scope.push_back(credential.substr(a, b - a));
    }
    if (scope.size() != 5 || scope[0] != creds_.access_key_id) return "unknown access key";
    std::string date = req.header("x-amz-date");
    if (date.substr(0, 8) != scope[1]) return "the date doesn't match the scope";
    std::string payload = req.header("x-amz-content-sha256");
    if (scope[3] == "s3" && payload.empty()) return "S3 needs x-amz-content-sha256";
    if (payload.empty()) payload = base::sha256_hex(req.body);
    if (payload != "UNSIGNED-PAYLOAD" && payload != base::sha256_hex(req.body)) return "the body doesn't match its hash";
    SignInput in;
    in.method = req.method;
    for (auto& c : in.method) c = char(std::toupper(static_cast<unsigned char>(c)));
    in.host = req.header("host");
    in.path = req.path;
    for (size_t a = 0, b; !req.query.empty() && a <= req.query.size(); a = b + 1) {
      b = req.query.find('&', a);
      if (b == std::string::npos) b = req.query.size();
      std::string pair = req.query.substr(a, b - a);
      auto eq = pair.find('=');
      in.query.emplace_back(base::percent_decode(pair.substr(0, eq)).value_or(""),
                            eq == std::string::npos ? "" : base::percent_decode(pair.substr(eq + 1)).value_or(""));
    }
    in.payload_sha256 = payload;
    in.content_sha256_header = false;
    for (size_t a = 0, b; a <= signed_names.size(); a = b + 1) {
      b = signed_names.find(';', a);
      if (b == std::string::npos) b = signed_names.size();
      std::string name = signed_names.substr(a, b - a);
      if (name == "host" || name == "x-amz-date" || name == "x-amz-security-token") continue;
      in.headers.emplace_back(name, req.header(name));
    }
    Credentials c = creds_;
    c.session_token = req.header("x-amz-security-token");
    auto expected = sign(in, c, scope[2], scope[3], date);
    std::string expected_auth = expected[0].second;
    if (field(expected_auth, "Signature") != signature) return "SignatureDoesNotMatch";
    return "";
  }

  server::http::Response xml_error(int status, const std::string& code) {
    server::http::Response r;
    r.status = status;
    r.headers.emplace_back("content-type", "application/xml");
    r.body = "<Error><Code>" + code + "</Code><Message>" + code + "</Message></Error>";
    return r;
  }

  server::http::Response handle(const server::http::Request& req) {
    std::lock_guard lock(mu_);
    ++requests_;
    if (fail_left_ > 0) {
      --fail_left_;
      return xml_error(fail_status_, "Injected");
    }
    if (auto why = check_signature(req); !why.empty()) {
      ++bad_signatures_;
      return xml_error(403, why);
    }
    server::http::Response r;
    if (req.method == "post" && req.path == "/v2/email/outbound-emails") {
      std::string err;
      auto v = json::parse(req.body, &err);
      auto raw = v ? base::base64_decode(std::string(v->at("Content").at("Raw").at("Data").as_string())) : std::nullopt;
      if (!raw) return xml_error(400, "BadRequest");
      mails_.push_back(*raw);
      r.headers.emplace_back("content-type", "application/json");
      r.body = "{\"MessageId\":\"fake-" + std::to_string(mails_.size()) + "\"}";
      return r;
    }
    std::string path = base::percent_decode(req.path).value_or(req.path);
    auto slash = path.find('/', 1);
    std::string bucket = path.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
    std::string key = slash == std::string::npos ? "" : path.substr(slash + 1);
    auto& objects = buckets_[bucket];
    if (key.empty() && req.method == "get") {
      std::string prefix, token;
      for (size_t a = 0, b; a <= req.query.size(); a = b + 1) {
        b = req.query.find('&', a);
        if (b == std::string::npos) b = req.query.size();
        std::string pair = req.query.substr(a, b - a);
        auto eq = pair.find('=');
        std::string k = pair.substr(0, eq), val = base::percent_decode(pair.substr(eq + 1)).value_or("");
        if (k == "prefix") prefix = val;
        if (k == "continuation-token") token = val;
      }
      std::string body = "<ListBucketResult>";
      size_t n = 0;
      std::string last;
      for (auto it = objects.upper_bound(token); it != objects.end(); ++it) {
        if (!it->first.starts_with(prefix)) continue;
        if (n == page_size_) {
          body += "<IsTruncated>true</IsTruncated><NextContinuationToken>" + base::html_escape(last) +
                  "</NextContinuationToken>";
          break;
        }
        body += "<Contents><Key>" + base::percent_encode(it->first) + "</Key><LastModified>2026-10-27T00:00:00.000Z</LastModified><Size>" +
                std::to_string(it->second.size()) + "</Size></Contents>";
        last = it->first;
        ++n;
      }
      r.body = body + "</ListBucketResult>";
      return r;
    }
    if (req.method == "put") {
      objects[key] = req.body;
      return r;
    }
    if (req.method == "get") {
      auto it = objects.find(key);
      if (it == objects.end()) return xml_error(404, "NoSuchKey");
      r.body = it->second;
      return r;
    }
    if (req.method == "delete") {
      objects.erase(key);
      r.status = 204;
      return r;
    }
    return xml_error(405, "MethodNotAllowed");
  }

  Credentials creds_;
  size_t page_size_;
  std::unique_ptr<server::http::Server> server_;
  std::mutex mu_;
  std::map<std::string, std::map<std::string, std::string>> buckets_;
  std::vector<std::string> mails_;
  int fail_left_ = 0, fail_status_ = 0, bad_signatures_ = 0, requests_ = 0;
};

}  // namespace goober::aws::testing
