#include "mailer.h"

#include <iostream>

#include <chrono>
#include <ctime>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/process.h"
#include "goober/base/random.h"
#include "goober/base/time.h"
#include "goober/json/json.h"

namespace goober::host {

namespace bj = boost::json;

namespace {

std::string header_value(std::string v) {
  for (char& c : v) {
    if (c == '\r' || c == '\n') c = ' ';
  }
  return v;
}

}  // namespace

// A display name as a quoted string: no quotes, backslashes, or line breaks, at most 64 characters.
std::string display_name(const std::string& name) {
  std::string out;
  for (char c : name) {
    if (c == '"' || c == '\\' || c == '\r' || c == '\n' || static_cast<unsigned char>(c) < 0x20) continue;
    out += c;
  }
  if (out.size() > 64) out.resize(64);
  return out.empty() ? "goober" : out;
}

// A header value as RFC 2047 encoded words when it isn't plain ASCII: SES and SMTP servers
// refuse raw UTF-8 in headers. Each word stays under 75 characters.
std::string encoded_words(const std::string& v) {
  bool ascii = true;
  for (unsigned char c : v) ascii &= c >= 0x20 && c < 0x7f;
  if (ascii) return v;
  std::string out;
  size_t i = 0;
  while (i < v.size()) {
    // 45 bytes of UTF-8 become 60 of base64; never split a character.
    size_t n = std::min<size_t>(45, v.size() - i);
    while (n > 0 && i + n < v.size() && (static_cast<unsigned char>(v[i + n]) & 0xC0) == 0x80) --n;
    if (n == 0) n = std::min<size_t>(45, v.size() - i);
    out += (out.empty() ? "" : " ") + std::string("=?UTF-8?B?") + base::base64_encode(v.substr(i, n)) + "?=";
    i += n;
  }
  return out;
}

namespace {

// A text part's body for SMTP: CRLF line endings, and base64 when a line passes 998 bytes
// (RFC 5322). Returns the transfer encoding.
std::string encode_text(const std::string& in, std::string* out) {
  std::string text;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\n' && (i == 0 || in[i - 1] != '\r')) text += '\r';
    text += in[i];
  }
  bool long_line = false;
  for (size_t start = 0, nl; start < text.size(); start = nl + 2) {
    nl = text.find("\r\n", start);
    if (nl == std::string::npos) nl = text.size();
    long_line |= nl - start > 998;
  }
  if (!long_line) {
    *out = std::move(text);
    return "8bit";
  }
  *out = base64_lines(text);
  return "base64";
}

}  // namespace

std::string base64_lines(std::string_view data) {
  std::string b64 = base::base64_encode(data), out;
  for (size_t i = 0; i < b64.size(); i += 76) out += b64.substr(i, 76) + "\r\n";
  return out;
}

std::string mime_message(const std::string& mailer_from, const identity::Mail& mail, int64_t now_ms, const std::string& id) {
  const std::string& from = mail.from_address.empty() ? mailer_from : mail.from_address;
  std::time_t secs = now_ms / 1000;
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char date[64];
  std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &tm);
  std::string domain = from.substr(from.find('@') + 1);
  std::string text;
  std::string encoding = encode_text(mail.text, &text);
  std::string reply_to = mail.reply_to.empty() ? "" : "Reply-To: " + header_value(mail.reply_to) + "\r\n";
  std::string name = display_name(mail.from_name);
  std::string from_name = encoded_words(name) == name ? "\"" + name + "\"" : encoded_words(name);
  std::string headers = "From: " + from_name + " <" + header_value(from) + ">\r\nTo: " + header_value(mail.to) + "\r\n" +
                        reply_to + "Subject: " + encoded_words(header_value(mail.subject)) + "\r\nDate: " + date +
                        "\r\nMessage-ID: <" + id + "@" + domain + ">\r\n" +
                        (mail.in_reply_to.empty() ? "" : "In-Reply-To: " + header_value(mail.in_reply_to) + "\r\n") +
                        (mail.references.empty() ? "" : "References: " + header_value(mail.references) + "\r\n") +
                        (mail.list_unsubscribe.empty()
                             ? ""
                             : "List-Unsubscribe: " + header_value(mail.list_unsubscribe) +
                                   "\r\nList-Unsubscribe-Post: List-Unsubscribe=One-Click\r\n") +
                        "Auto-Submitted: auto-generated\r\nMIME-Version: 1.0\r\n";
  if (mail.html.empty()) {
    return headers + "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: " + encoding + "\r\n\r\n" + text;
  }
  // Text first, HTML last: a client shows the last part it can (RFC 2046 § 5.1.4). The HTML
  // goes as base64, so no line of it can break SMTP, and the boundary can't occur in either.
  std::string boundary = "=_goober_" + id;
  return headers + "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n\r\n" +
         "--" + boundary + "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: " + encoding +
         "\r\n\r\n" + text + (text.ends_with("\r\n") ? "" : "\r\n") + "--" + boundary +
         "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n" +
         base64_lines(base::valid_utf8(mail.html)) + "--" + boundary + "--\r\n";
}

MailQueue::MailQueue(ConfigFn config, CredentialsFn credentials, DevFn dev)
    : config_(std::move(config)),
      credentials_(std::move(credentials)),
      dev_(std::move(dev)),
      worker_([this] { run(); }) {}

MailQueue::~MailQueue() {
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  worker_.join();
}

void MailQueue::send(identity::Mail mail) {
  {
    std::lock_guard lock(mu_);
    if (queue_.size() >= 1000) return;  // a flood never grows memory without bound
    queue_.push_back(std::move(mail));
  }
  cv_.notify_all();
}

void MailQueue::drain() {
  std::unique_lock lock(mu_);
  cv_.wait(lock, [&] { return queue_.empty() && !busy_; });
}

MailOutcome MailQueue::deliver(const identity::Mail& mail) {
  auto cfg = config_();
  MailOutcome out;
  // A sandbox (a devbox) never mails anyone: everything goes to the dev mailer.
  if (cfg->sandbox || cfg->identity.mailer == "dev") {
    out.via = "dev";
    try {
      if (dev_) dev_(mail);
      out.sent = true;
    } catch (const std::exception& e) {
      out.error = std::string("the dev mailer failed: ") + e.what();
    }
    return out;
  }
  uint8_t rnd[12];
  base::os_random_fill(rnd);
  std::string id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
  if (cfg->identity.mailer == "ses") {
    out.via = "ses";
    auto creds = credentials_ ? credentials_() : std::nullopt;
    if (!creds) {
      out.error = "no AWS credentials: set keys/system/aws_access_key_id and aws_secret_access_key";
      return out;
    }
    const auto& ses = cfg->identity.ses;
    aws::SesMessage m{mail.from_address.empty() ? ses.from : mail.from_address, mail.to,
                      mime_message(ses.from, mail, base::now_unix_ms(), id), ses.configuration_set};
    auto r = aws::ses_send(m, *creds, ses.region.empty() ? cfg->aws.region : ses.region, cfg->storage / "tmp", ses.endpoint);
    out.sent = r.sent;
    out.permanent = r.permanent;
    out.error = r.error;
    out.message_id = r.message_id;
  } else {
    out.via = "smtp";
    try {
      out.sent = deliver_smtp(*cfg, mail, &out.error);
    } catch (const std::exception& e) {
      out.error = e.what();
    }
  }
  bj::object line{{"at", base::format_utc_ms(base::now_unix_ms())},
                  {"to", mail.to},
                  {"subject", mail.subject},
                  {"via", out.via},
                  {"status", out.sent ? "sent" : "failed"}};
  if (!out.message_id.empty()) line["message_id"] = out.message_id;
  if (!mail.tenant.empty()) line["tenant"] = mail.tenant;
  if (!mail.from_address.empty()) line["from"] = mail.from_address;
  if (!mail.in_reply_to.empty()) line["in_reply_to"] = mail.in_reply_to;
  if (!out.sent) line["error"] = out.error;
  base::append_line(cfg->storage / "logs" / "mail.log", json::dump(line));
  return out;
}

void MailQueue::run() {
  for (;;) {
    identity::Mail mail;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;
      mail = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
    }
    // Three tries, a little apart: a mail server that hiccups shouldn't cost a sign-in.
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (attempt > 0) std::this_thread::sleep_for(std::chrono::seconds(attempt * 3));
      auto outcome = deliver(mail);
      if (outcome.sent || outcome.permanent) break;
    }
    {
      std::lock_guard lock(mu_);
      busy_ = false;
    }
    cv_.notify_all();
  }
}

bool MailQueue::deliver_smtp(const config::HostConfig& cfg, const identity::Mail& mail, std::string* error) {
  const auto& smtp = cfg.identity.smtp;
  uint8_t rnd[12];
  base::os_random_fill(rnd);
  std::string id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
  std::string message = mime_message(smtp.from, mail, base::now_unix_ms(), id);
  std::vector<std::string> argv = {"curl", "-q", "-g", "-sS", "--max-time", "30", "--url", smtp.url, "--mail-from", smtp.from,
                                   "--mail-rcpt", mail.to, "--upload-file", "-"};
  // STARTTLS is required on smtp://; smtps:// is TLS from the first byte.
  if (smtp.url.starts_with("smtp://")) argv.push_back("--ssl-reqd");
  // The password goes to curl in a private config file, never on its command line,
  // where any user on the machine could read it.
  std::filesystem::path conf;
  if (!smtp.username.empty()) {
    auto password = base::read_file(smtp.password_file);
    if (!password) return *error = "can't read identity.smtp.password-file", false;
    while (!password->empty() && (password->back() == '\n' || password->back() == '\r')) password->pop_back();
    auto written = base::write_private_file(cfg.storage / "mail", ".curl-",
                                            "user = " + base::curl_quoted(smtp.username + ":" + *password) + "\n");
    if (!written) return *error = "can't write the curl config", false;
    conf = *written;
    argv.push_back("-K");
    argv.push_back(conf.string());
  }
  auto r = base::run_process(argv, message, 64 * 1024, 40000);
  if (!conf.empty()) {
    std::error_code ec;
    std::filesystem::remove(conf, ec);
  }
  if (!r.started) return *error = "could not run curl", false;
  if (r.timed_out) return *error = "the mail server took too long", false;
  if (r.exit_code != 0) {
    std::string msg = r.err;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return *error = msg.empty() ? "curl exited with " + std::to_string(r.exit_code) : msg, false;
  }
  return true;
}

}  // namespace goober::host
