// One signed request through curl.

#include <cstdlib>

#include "goober/aws/aws.h"
#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/process.h"
#include "goober/base/time.h"
#include "goober/json/json.h"

namespace goober::aws {
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxBody = 4 << 20;

bool loopback(const Endpoint& e) {
  return e.host == "localhost" || e.host.starts_with("localhost:") || e.host == "127.0.0.1" ||
         e.host.starts_with("127.0.0.1:") || e.host.starts_with("[::1]");
}

std::string between(const std::string& text, const std::string& open, const std::string& close) {
  auto a = text.find(open);
  if (a == std::string::npos) return "";
  a += open.size();
  auto b = text.find(close, a);
  return b == std::string::npos ? "" : text.substr(a, b - a);
}

// The service's own words for a failure: S3 answers in XML, SES in JSON.
std::string service_error(int status, const std::string& body) {
  std::string code = between(body, "<Code>", "</Code>");
  std::string message = between(body, "<Message>", "</Message>");
  if (code.empty() && message.empty()) {
    std::string err;
    if (auto v = json::parse(body, &err); v && v->is_object()) {
      for (const char* k : {"message", "Message"}) {
        if (auto* m = v->as_object().if_contains(k); m && m->is_string()) message = std::string(m->as_string());
      }
    }
  }
  std::string out = "status " + std::to_string(status);
  if (!code.empty()) out += ": " + code;
  if (!message.empty()) out += (code.empty() ? ": " : " (") + message.substr(0, 300) + (code.empty() ? "" : ")");
  return out;
}

}  // namespace

Reply perform(const Request& req, const Credentials& creds, const fs::path& scratch) {
  Reply reply;
  if (req.endpoint.scheme != "https" && !(req.endpoint.scheme == "http" && loopback(req.endpoint))) {
    reply.error = "only https endpoints (or http on this machine)";
    return reply;
  }
  if (creds.access_key_id.empty() || creds.secret_access_key.empty()) {
    reply.error = "no AWS credentials";
    return reply;
  }
  SignInput in;
  in.method = req.method;
  in.host = req.endpoint.host;
  in.path = req.path;
  in.query = req.query;
  in.headers = req.headers;
  if (!req.upload.empty()) {
    auto digest = base::sha256_file_hex(req.upload);
    if (!digest) {
      reply.error = "can't read " + req.upload.string();
      return reply;
    }
    in.payload_sha256 = *digest;
  } else {
    in.payload_sha256 = base::sha256_hex(req.body);
  }
  auto auth = sign(in, creds, req.region, req.service, amz_date(base::now_unix_ms()));
  std::string conf;
  for (const auto& [k, v] : req.headers) conf += "header = " + base::curl_quoted(k + ": " + v) + "\n";
  for (const auto& [k, v] : auth) conf += "header = " + base::curl_quoted(k + ": " + v) + "\n";
  auto conf_file = base::write_private_file(scratch, ".aws-", conf);
  if (!conf_file) {
    reply.error = "can't prepare the request";
    return reply;
  }
  auto out_file = base::write_private_file(scratch, ".aws-out-", "");
  if (!out_file) {
    std::error_code ec;
    fs::remove(*conf_file, ec);
    reply.error = "can't prepare the request";
    return reply;
  }
  std::string query;
  for (const auto& [k, v] : req.query) query += (query.empty() ? "?" : "&") + uri_encode(k, true) + "=" + uri_encode(v, true);
  std::string url = req.endpoint.scheme + "://" + req.endpoint.host + req.path + query;
  int64_t seconds = std::max<int64_t>(1, req.timeout_ms / 1000);
  std::vector<std::string> argv = {"curl", "-q", "-g", "--path-as-is", "--noproxy", "*", "-sS",
                                   "--proto", "=" + req.endpoint.scheme, "--max-time", std::to_string(seconds),
                                   // A transfer that stalls for two minutes is given up, whatever its size.
                                   "--speed-limit", "1", "--speed-time", "120",
                                   "-X", req.method, "-K", conf_file->string(), "-o", out_file->string(),
                                   "-w", "%{http_code}"};
  if (!req.upload.empty()) {
    argv.push_back("--upload-file");
    argv.push_back(req.upload.string());
  } else if (!req.body.empty()) {
    argv.push_back("--data-binary");
    argv.push_back("@-");
  }
  argv.push_back("--");
  argv.push_back(url);
  auto r = base::run_process(argv, req.upload.empty() ? req.body : std::string(), 64, req.timeout_ms + 5000);
  std::error_code ec;
  fs::remove(*conf_file, ec);
  auto cleanup = [&] { fs::remove(*out_file, ec); };
  if (!r.started) {
    cleanup();
    reply.error = "could not run curl";
    return reply;
  }
  if (r.timed_out || r.exit_code != 0) {
    cleanup();
    std::string msg = r.err;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    reply.error = r.timed_out ? "the request took too long" : (msg.empty() ? "curl exited with " + std::to_string(r.exit_code) : msg);
    return reply;
  }
  reply.status = std::atoi(r.out.c_str());
  bool ok = reply.status >= 200 && reply.status < 300;
  if (ok && !req.download.empty()) {
    fs::create_directories(req.download.parent_path(), ec);
    fs::rename(*out_file, req.download, ec);
    if (ec) {
      // Another file system: copy, then drop the original.
      ec.clear();
      fs::copy_file(*out_file, req.download, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
      cleanup();
      reply.error = "can't move the download into place: " + ec.message();
    }
    return reply;
  }
  if (fs::file_size(*out_file, ec) > kMaxBody) {
    cleanup();
    reply.error = "the reply is larger than " + std::to_string(kMaxBody) + " bytes";
    return reply;
  }
  reply.body = base::read_file(*out_file).value_or("");
  cleanup();
  if (!ok) reply.error = service_error(reply.status, reply.body);
  return reply;
}

}  // namespace goober::aws
