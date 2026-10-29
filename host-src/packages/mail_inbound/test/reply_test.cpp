#include <gtest/gtest.h>

#include "goober/mail_inbound/reply.h"

using goober::mail::inbound::reply_text;

TEST(ReplyText, TheWordsAboveTheQuote) {
  EXPECT_EQ(reply_text("I vote Q39.\r\nThursday works.\r\n\r\nOn Tue, Oct 29, 2026 at 6:00 AM The KC Thesis "
                       "<thesis@kcmeatup.com> wrote:\r\n> Today: three proposals\r\n> ...\r\n"),
            "I vote Q39.\nThursday works.");
  // Gmail wraps the header onto two lines.
  EXPECT_EQ(reply_text("Yes!\n\nOn Tue, Oct 29, 2026 at 6:00 AM The KC Thesis <thesis@kcmeatup.com>\nwrote:\n> x"), "Yes!");
  EXPECT_EQ(reply_text("Count me in.\n\n-----Original Message-----\nFrom: x"), "Count me in.");
  EXPECT_EQ(reply_text("Outlook here.\n________________________________\nFrom: The KC Thesis\nSent: Tuesday\nTo: me"),
            "Outlook here.");
  EXPECT_EQ(reply_text("Bare Outlook.\nFrom: The KC Thesis <thesis@kcmeatup.com>\nSent: Tuesday, October 29\nSubject: x"),
            "Bare Outlook.");
  EXPECT_EQ(reply_text("Signed.\n-- \nAda Lovelace\nAnalytical Engines"), "Signed.");
  EXPECT_EQ(reply_text("Short one\n\nSent from my iPhone"), "Short one");
  EXPECT_EQ(reply_text("Mine.\n\n—— reply above this line ——\nToday's digest"), "Mine.");
  EXPECT_EQ(reply_text("Le mar. 29 oct. 2026, The KC Thesis a écrit :\n> x"), "");
  // Text with nothing quoted is kept whole, "from:" in a sentence included.
  EXPECT_EQ(reply_text("Ideas from: everyone.\nOne more line."), "Ideas from: everyone.\nOne more line.");
}
