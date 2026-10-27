#pragma once

// The SMTP mailer (PROGRESS.md Phase 3): mail is queued and sent by one background thread
// through curl, so how long a sign-in takes never says whether the address has an account
// (013 § Sign-in screen requirements), and a slow server never holds a request.

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "goober/config/config.h"
#include "goober/identity/identity.h"

namespace goober::host {

// The MIME text of one plain-text mail. Header values lose any CR or LF.
std::string mime_message(const std::string& from, const identity::Mail& mail, int64_t now_ms, const std::string& id);

class SmtpQueue {
 public:
  using ConfigFn = std::function<std::shared_ptr<const config::HostConfig>()>;
  // How the dev mailer delivers (a file and a log line), run on the queue's thread too, so
  // a sign-in takes the same time whether or not a mail goes out.
  using DevFn = std::function<void(const identity::Mail&)>;
  SmtpQueue(ConfigFn config, DevFn dev);
  ~SmtpQueue();
  void send(identity::Mail mail);
  // Waits until the queue is empty (tests).
  void drain();

 private:
  void run();
  bool deliver(const config::HostConfig& cfg, const identity::Mail& mail, std::string* error);

  ConfigFn config_;
  DevFn dev_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<identity::Mail> queue_;
  bool busy_ = false;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace goober::host
