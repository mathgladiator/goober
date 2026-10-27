#include "mailer.h"

#include <fcntl.h>
#include <unistd.h>

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

// A value for curl's config file: double-quoted, with \ and " escaped.
std::string curl_quoted(const std::string& v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

}  // namespace

std::string mime_message(const std::string& from, const identity::Mail& mail, int64_t now_ms, const std::string& id) {
  std::time_t secs = now_ms / 1000;
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char date[64];
  std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &tm);
  std::string domain = from.substr(from.find('@') + 1);
  std::string text;
  // SMTP wants CRLF line endings, and a line holding only "." would end the message early.
  for (size_t i = 0; i < mail.text.size(); ++i) {
    if (mail.text[i] == '\n' && (i == 0 || mail.text[i - 1] != '\r')) text += '\r';
    text += mail.text[i];
  }
  return "From: goober <" + header_value(from) + ">\r\nTo: " + header_value(mail.to) + "\r\nSubject: " +
         header_value(mail.subject) + "\r\nDate: " + date + "\r\nMessage-ID: <" + id + "@" + domain +
         ">\r\nAuto-Submitted: auto-generated\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n"
         "Content-Transfer-Encoding: 8bit\r\n\r\n" +
         text;
}

SmtpQueue::SmtpQueue(ConfigFn config) : config_(std::move(config)), worker_([this] { run(); }) {}

SmtpQueue::~SmtpQueue() {
  {
    std::lock_guard lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  worker_.join();
}

void SmtpQueue::send(identity::Mail mail) {
  {
    std::lock_guard lock(mu_);
    if (queue_.size() >= 1000) return;  // a flood never grows memory without bound
    queue_.push_back(std::move(mail));
  }
  cv_.notify_all();
}

void SmtpQueue::drain() {
  std::unique_lock lock(mu_);
  cv_.wait(lock, [&] { return queue_.empty() && !busy_; });
}

void SmtpQueue::run() {
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
    auto cfg = config_();
    std::string error;
    bool sent = false;
    // Three tries, a little apart: a mail server that hiccups shouldn't cost a sign-in.
    for (int attempt = 0; attempt < 3 && !sent; ++attempt) {
      if (attempt > 0) std::this_thread::sleep_for(std::chrono::seconds(attempt * 3));
      sent = deliver(*cfg, mail, &error);
    }
    bj::object line{{"at", base::format_utc_ms(base::now_unix_ms())},
                    {"to", mail.to},
                    {"subject", mail.subject},
                    {"via", "smtp"},
                    {"status", sent ? "sent" : "failed"}};
    if (!sent) line["error"] = error;
    base::append_line(cfg->storage / "logs" / "mail.log", json::dump(line));
    {
      std::lock_guard lock(mu_);
      busy_ = false;
    }
    cv_.notify_all();
  }
}

bool SmtpQueue::deliver(const config::HostConfig& cfg, const identity::Mail& mail, std::string* error) {
  const auto& smtp = cfg.identity.smtp;
  uint8_t rnd[12];
  base::os_random_fill(rnd);
  std::string id = base::hex_encode(std::string_view(reinterpret_cast<char*>(rnd), sizeof rnd));
  std::string message = mime_message(smtp.from, mail, base::now_unix_ms(), id);
  std::vector<std::string> argv = {"curl", "-sS", "--max-time", "30", "--url", smtp.url, "--mail-from", smtp.from,
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
    conf = cfg.storage / "mail" / (".curl-" + id);
    std::filesystem::create_directories(conf.parent_path());
    std::string text = "user = " + curl_quoted(smtp.username + ":" + *password) + "\n";
    // Created 0600 from the start, so the password is never readable by anyone else.
    int fd = open(conf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return *error = "can't write the curl config", false;
    bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
    close(fd);
    if (!ok) return *error = "can't write the curl config", false;
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
