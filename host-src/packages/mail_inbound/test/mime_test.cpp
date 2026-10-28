#include <gtest/gtest.h>

#include "goober/mail_inbound/mime.h"

using namespace goober::mail::inbound;

TEST(Mime, APlainMessage) {
  auto m = parse_raw_message(
      "From: \"Lee, Ann\" <Ann@Example.org>\r\n"
      "To: support@kcmeatup.com, Bob <bob@example.org>\r\n"
      "Subject: Half a cow\r\n"
      "Message-ID: <abc@example.org>\r\n"
      "In-Reply-To: <x1@kcmeatup.com>\r\n"
      "References: <x0@kcmeatup.com>\r\n <x1@kcmeatup.com>\r\n"
      "\r\n"
      "Do you have half a cow left?\r\n");
  ASSERT_TRUE(m.from);
  EXPECT_EQ(m.from->address, "ann@example.org");
  EXPECT_EQ(m.from->name, "Lee, Ann");
  ASSERT_EQ(m.to.size(), 2u);
  EXPECT_EQ(m.to[1].address, "bob@example.org");
  EXPECT_EQ(m.subject, "Half a cow");
  EXPECT_EQ(m.message_id, "abc@example.org");
  EXPECT_EQ(m.in_reply_to, "x1@kcmeatup.com");
  ASSERT_EQ(m.references.size(), 2u);
  EXPECT_EQ(m.references[1], "x1@kcmeatup.com");
  EXPECT_EQ(m.text, "Do you have half a cow left?\r\n");
  EXPECT_TRUE(m.html.empty());
  EXPECT_TRUE(m.attachments.empty());
}

TEST(Mime, AlternativeBodiesAndEncodedHeaders) {
  auto m = parse_raw_message(
      "From: =?UTF-8?B?Sm9zw6kgUGXDsWE=?= <jose@example.org>\r\n"
      "Subject: =?ISO-8859-1?Q?Caf=E9_tomorrow?= =?UTF-8?Q?_=E2=98=95?=\r\n"
      "Content-Type: multipart/alternative; boundary=\"b1\"\r\n"
      "\r\n"
      "preamble\r\n"
      "--b1\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Transfer-Encoding: quoted-printable\r\n"
      "\r\n"
      "Caf=C3=A9 at nine, a long line that is soft=\r\n"
      " broken.\r\n"
      "--b1\r\n"
      "Content-Type: text/html; charset=windows-1252\r\n"
      "Content-Transfer-Encoding: base64\r\n"
      "\r\n"
      "PHA+k0hplDwvcD4=\r\n"
      "--b1--\r\n"
      "epilogue\r\n");
  ASSERT_TRUE(m.from);
  EXPECT_EQ(m.from->name, "José Peña");
  EXPECT_EQ(m.subject, "Café tomorrow ☕");
  EXPECT_EQ(m.text, "Café at nine, a long line that is soft broken.");
  EXPECT_EQ(m.html, "<p>“Hi”</p>");  // Windows-1252 curly quotes
  EXPECT_TRUE(m.attachments.empty());
}

TEST(Mime, AttachmentsAndInlineImages) {
  auto m = parse_raw_message(
      "From: ann@example.org\n"
      "Content-Type: multipart/mixed; boundary=outer\n"
      "\n"
      "--outer\n"
      "Content-Type: multipart/related; boundary=inner\n"
      "\n"
      "--inner\n"
      "Content-Type: text/html; charset=utf-8\n"
      "\n"
      "<p>Our <b>brisket</b><img src=\"cid:pic1\"></p>\n"
      "--inner\n"
      "Content-Type: image/png\n"
      "Content-ID: <pic1>\n"
      "Content-Transfer-Encoding: base64\n"
      "\n"
      "iVBORw0K\n"
      "--inner--\n"
      "--outer\n"
      "Content-Type: application/pdf; name=\"ignored.pdf\"\n"
      "Content-Disposition: attachment; filename*=utf-8''Men%C3%BA%20fall.pdf\n"
      "Content-Transfer-Encoding: base64\n"
      "\n"
      "JVBERi0x\n"
      "LjQK\n"
      "--outer\n"
      "Content-Type: text/plain\n"
      "Content-Disposition: attachment; filename=\"notes.txt\"\n"
      "\n"
      "not the body\n"
      "--outer--\n");
  EXPECT_EQ(m.html, "<p>Our <b>brisket</b><img src=\"cid:pic1\"></p>");
  EXPECT_NE(m.text.find("brisket"), std::string::npos);  // made from the HTML
  ASSERT_EQ(m.attachments.size(), 3u);
  EXPECT_EQ(m.attachments[0].content_type, "image/png");
  EXPECT_TRUE(m.attachments[0].inline_part);
  EXPECT_EQ(m.attachments[0].content_id, "pic1");
  EXPECT_EQ(m.attachments[0].bytes, std::string("\x89PNG\r\n", 6));
  EXPECT_EQ(m.attachments[1].filename, "Menú fall.pdf");
  EXPECT_EQ(m.attachments[1].bytes, "%PDF-1.4\n");
  EXPECT_FALSE(m.attachments[1].inline_part);
  EXPECT_EQ(m.attachments[2].filename, "notes.txt");
  EXPECT_EQ(m.attachments[2].bytes, "not the body");
}

TEST(Mime, StalwartsHeadersAndBodyApart) {
  // Stalwart hands over raw header values (leading space, folding, trailing CRLF) and the body.
  Headers h{{"From", " Ann <ann@example.org>\r\n"},
            {"Subject", " A very\r\n long subject\r\n"},
            {"Content-Type", " text/plain;\r\n charset=\"iso-8859-1\"\r\n"},
            {"Content-Transfer-Encoding", " 8bit\r\n"}};
  auto m = parse_message(h, "Gr\xFC\xDF Gott\r\n");
  EXPECT_EQ(m.subject, "A very long subject");
  EXPECT_EQ(m.text, "Grüß Gott\r\n");
  EXPECT_EQ(header(m.headers, "subject").value_or(""), "A very long subject");
}

TEST(Mime, BrokenInputDegrades) {
  // No closing boundary, a missing boundary parameter, junk base64: nothing throws.
  auto a = parse_raw_message("Content-Type: multipart/mixed; boundary=x\r\n\r\n--x\r\n\r\nhello");
  EXPECT_EQ(a.text, "hello");
  auto b = parse_raw_message("Content-Type: multipart/mixed\r\n\r\nbody");
  EXPECT_TRUE(b.text.empty());
  auto c = parse_raw_message("Content-Transfer-Encoding: base64\r\n\r\n!!!@@@");
  EXPECT_TRUE(c.text.empty());
  auto d = parse_raw_message("");
  EXPECT_FALSE(d.from);
  // Invalid UTF-8 becomes valid.
  auto e = parse_raw_message("Subject: \xFF\xFE ok\r\n\r\n\xC3\x28");
  EXPECT_EQ(e.subject, "\xEF\xBF\xBD\xEF\xBF\xBD ok");

  // Nesting past the limit is cut off, and says so.
  std::string deep, tail;
  for (int i = 0; i < 20; ++i) {
    deep += "Content-Type: multipart/mixed; boundary=b" + std::to_string(i) + "\r\n\r\n--b" + std::to_string(i) + "\r\n";
    tail = "\r\n--b" + std::to_string(i) + "--" + tail;
  }
  auto f = parse_raw_message(deep + "\r\nleaf" + tail);
  EXPECT_TRUE(f.truncated);
  EXPECT_TRUE(f.text.empty());

  Limits small;
  small.max_text_bytes = 4;
  auto g = parse_raw_message("\r\nhéllo world", small);
  EXPECT_EQ(g.text, "h\xC3\xA9l");
  EXPECT_TRUE(g.truncated);
}

TEST(Mime, MailboxLists) {
  auto l = parse_mailboxes("\"Doe, J\\\"R\\\"\" <j@x.org>, plain@y.org (Plain Person), undisclosed-recipients:;");
  ASSERT_EQ(l.size(), 2u);
  EXPECT_EQ(l[0].name, "Doe, J\"R\"");
  EXPECT_EQ(l[0].address, "j@x.org");
  EXPECT_EQ(l[1].address, "plain@y.org");
  EXPECT_TRUE(parse_mailboxes("nobody").empty());
}

TEST(Mime, AutomaticMailIsRecognized) {
  EXPECT_FALSE(is_automatic({{"Subject", "hi"}}, "ann@example.org"));
  EXPECT_TRUE(is_automatic({}, ""));  // a bounce
  EXPECT_TRUE(is_automatic({{"Auto-Submitted", " auto-replied"}}, "a@b.org"));
  EXPECT_FALSE(is_automatic({{"Auto-Submitted", "no"}}, "a@b.org"));
  EXPECT_TRUE(is_automatic({{"X-Autoreply", "yes"}}, "a@b.org"));
  EXPECT_TRUE(is_automatic({{"Precedence", "Bulk"}}, "a@b.org"));
  EXPECT_FALSE(is_automatic({{"Precedence", "list"}}, "a@b.org"));  // a mailing list's discussion
  EXPECT_TRUE(is_automatic({{"Content-Type", "multipart/report; report-type=delivery-status"}}, "a@b.org"));
}
