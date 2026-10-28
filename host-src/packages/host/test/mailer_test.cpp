// The SMTP mailer (PROGRESS.md Phase 3), driven against a stand-in curl.

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <cstdlib>

#include "../src/mailer.h"
#include "goober/base/files.h"
#include "goober/host/host.h"

using namespace goober;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace fs = std::filesystem;

TEST(Mailer, MimeHeadersCantBeInjected) {
  identity::Mail m{"a@b.c\r\nBcc: evil@x.y", "Hi\nthere", "line one\nline two\n"};
  auto text = host::mime_message("no-reply@example.com", m, 0, "abc");
  EXPECT_EQ(text.find("\r\nBcc:"), std::string::npos);
  EXPECT_NE(text.find("To: a@b.c  Bcc: evil@x.y\r\n"), std::string::npos);
  EXPECT_NE(text.find("Subject: Hi there\r\n"), std::string::npos);
  EXPECT_NE(text.find("Message-ID: <abc@example.com>"), std::string::npos);
  EXPECT_NE(text.find("\r\n\r\nline one\r\nline two\r\n"), std::string::npos);
}

TEST(Mailer, SignInCodesGoThroughSmtpWithThePasswordOffTheCommandLine) {
  auto root = fs::path(GOOBER_TEST_TMP) / "mailer";
  fs::remove_all(root);
  fs::create_directories(root / "bin");
  // A stand-in curl that records what it was given.
  base::write_file_durable(root / "bin/curl", R"(#!/bin/sh
dir=$(dirname "$0")
printf '%s\n' "$@" > "$dir/args"
prev=""
for a in "$@"; do
  if [ "$prev" = "-K" ]; then cp "$a" "$dir/conf"; stat -c %a "$a" > "$dir/mode"; fi
  prev="$a"
done
cat > "$dir/stdin"
)");
  chmod((root / "bin/curl").c_str(), 0755);
  base::write_file_durable(root / "smtp-password", "s3cret \"quoted\"\n");
  std::string old_path = std::getenv("PATH") ? std::getenv("PATH") : "";
  setenv("PATH", ((root / "bin").string() + ":" + old_path).c_str(), 1);

  std::string err;
  auto cfg = config::parse_config(R"({"listen": {"port": 0}, "storage": "storage",
      "domains": {"localhost": {"tenant": "playground"}}, "threads": {"network": 1, "boxes": 1},
      "identity": {"mailer": "smtp", "smtp": {"url": "smtp://smtp.example.com:587", "from": "no-reply@example.com",
                   "username": "goober", "password-file": ")" + (root / "smtp-password").string() + R"("}}})",
                                  root, &err);
  ASSERT_TRUE(cfg) << err;
  {
    host::Host h(cfg);
    uint16_t port = h.start();
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    bhttp::request<bhttp::string_body> req(bhttp::verb::post, "/identity/email/start", 11);
    req.set(bhttp::field::host, "localhost");
    req.set(bhttp::field::content_type, "application/x-www-form-urlencoded");
    req.body() = "email=boss%40example.com";
    req.prepare_payload();
    bhttp::write(stream, req);
    beast::flat_buffer buf;
    bhttp::response<bhttp::string_body> res;
    bhttp::read(stream, buf, res);
    EXPECT_EQ(res.result_int(), 200u);
    h.drain_mail();
  }
  setenv("PATH", old_path.c_str(), 1);

  auto args = base::read_file(root / "bin/args");
  ASSERT_TRUE(args);
  EXPECT_NE(args->find("--mail-rcpt\nboss@example.com\n"), std::string::npos) << *args;
  EXPECT_NE(args->find("--ssl-reqd"), std::string::npos);  // STARTTLS required on smtp://
  EXPECT_EQ(args->find("s3cret"), std::string::npos);      // never on the command line
  EXPECT_EQ(*base::read_file(root / "bin/conf"), "user = \"goober:s3cret \\\"quoted\\\"\"\n");
  EXPECT_EQ(*base::read_file(root / "bin/mode"), "600\n");
  auto mail = base::read_file(root / "bin/stdin");
  // Rendered from the sign-in-new template (022): a welcome, as text and as HTML.
  EXPECT_NE(mail->find("Your code to finish signing in is "), std::string::npos) << *mail;
  EXPECT_NE(mail->find("Subject: Welcome to localhost: your sign-in code"), std::string::npos);
  EXPECT_NE(mail->find("Content-Type: multipart/alternative; boundary="), std::string::npos);
  EXPECT_NE(mail->find("Content-Type: text/html; charset=utf-8"), std::string::npos);
  // No code left on disk, and the private curl config is gone.
  EXPECT_FALSE(fs::exists(root / "storage/mail/outbox"));
  for (const auto& e : fs::directory_iterator(root / "storage/mail")) ADD_FAILURE() << e.path();
  EXPECT_NE(base::read_file(root / "storage/logs/mail.log")->find(R"("status":"sent")"), std::string::npos);
}

// Review 5, finding 9: headers that aren't ASCII are encoded words, and a body with a line
// past 998 bytes goes as base64, so a mail server never refuses either.
TEST(Mailer, NonAsciiHeadersAndLongLines) {
  identity::Mail m{"a@b.c", "Café ☕ tomorrow", "short\n" + std::string(2000, 'x') + "\n", "Rancho Peña", ""};
  auto text = host::mime_message("no-reply@example.com", m, 0, "id");
  EXPECT_NE(text.find("Subject: =?UTF-8?B?"), std::string::npos) << text;
  EXPECT_NE(text.find("From: =?UTF-8?B?"), std::string::npos) << text;
  EXPECT_NE(text.find("Content-Transfer-Encoding: base64"), std::string::npos);
  for (size_t start = 0, nl; start < text.size(); start = nl + 2) {
    nl = text.find("\r\n", start);
    if (nl == std::string::npos) nl = text.size();
    EXPECT_LE(nl - start, 998u);
  }
  identity::Mail plain{"a@b.c", "Hello", "hi\n", "Beef Share", ""};
  auto p = host::mime_message("no-reply@example.com", plain, 0, "id");
  EXPECT_NE(p.find("From: \"Beef Share\" <no-reply@example.com>"), std::string::npos);
  EXPECT_NE(p.find("Subject: Hello\r\n"), std::string::npos);
  EXPECT_NE(p.find("Content-Transfer-Encoding: 8bit"), std::string::npos);
}
