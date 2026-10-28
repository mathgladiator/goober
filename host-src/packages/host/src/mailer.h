#pragma once

// The mailer (PROGRESS.md Phase 3; design/machine/018.Mail.md). Sign-in mail is queued and sent
// by one background thread, so how long a sign-in takes never says whether the address has an
// account (013 § Sign-in screen requirements), and a slow server never holds a request. Mail
// that boxes send goes through the outbox instead, and calls deliver() directly. Either way it
// leaves through SMTP (curl) or Amazon SES's API, or, with the dev mailer or on a sandbox
// host, is written to storage/mail/outbox/ and never leaves.

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "goober/aws/aws.h"
#include "goober/config/config.h"
#include "goober/identity/identity.h"

namespace goober::host {

// The MIME text of one mail: plain text, or multipart/alternative when it has HTML. Header
// values lose any CR or LF.
std::string base64_lines(std::string_view data);
std::string mime_message(const std::string& from, const identity::Mail& mail, int64_t now_ms, const std::string& id);

struct MailOutcome {
  bool sent = false;
  bool permanent = false;  // refused in a way asking again won't fix
  std::string via;         // smtp, ses, or dev
  std::string error;
  std::string message_id;  // SES's, when it gives one
};

class MailQueue {
 public:
  using ConfigFn = std::function<std::shared_ptr<const config::HostConfig>()>;
  using CredentialsFn = std::function<std::optional<aws::Credentials>()>;
  // How the dev mailer delivers (a file and a log line).
  using DevFn = std::function<void(const identity::Mail&)>;
  MailQueue(ConfigFn config, CredentialsFn credentials, DevFn dev);
  ~MailQueue();
  // Queues a mail for the background thread, with three tries.
  void send(identity::Mail mail);
  // Delivers one mail now, on the calling thread, once; the outbox calls this.
  MailOutcome deliver(const identity::Mail& mail);
  // Waits until the queue is empty (tests).
  void drain();

 private:
  void run();
  bool deliver_smtp(const config::HostConfig& cfg, const identity::Mail& mail, std::string* error);

  ConfigFn config_;
  CredentialsFn credentials_;
  DevFn dev_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<identity::Mail> queue_;
  bool busy_ = false;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace goober::host
