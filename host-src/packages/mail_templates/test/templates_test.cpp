#include <gtest/gtest.h>

#include "goober/mail_templates/templates.h"

using namespace goober::mail::templates;
namespace fs = std::filesystem;
namespace bj = boost::json;

namespace {

class MailTemplatesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir = fs::path(GOOBER_TEST_TMP) / "mail_templates";
    fs::remove_all(dir);
    store = std::make_unique<Store>(dir);
  }
  fs::path dir;
  std::unique_ptr<Store> store;
};

bool has(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_F(MailTemplatesTest, EveryDefaultRendersWithItsSample) {
  for (const auto& b : builtins()) {
    std::string err;
    auto r = store->render("playground", b.name, b.sample, &err);
    ASSERT_TRUE(r) << b.name << ": " << err;
    EXPECT_FALSE(r->text.empty()) << b.name;
    EXPECT_TRUE(r->html.starts_with("<!doctype html>")) << b.name;
    EXPECT_FALSE(has(r->html, "{{")) << b.name;
    EXPECT_FALSE(has(r->html, "<script")) << b.name;
  }
}

TEST_F(MailTemplatesTest, SignInMailsCarryTheCodeAndNoLinks) {
  std::string err;
  bj::object vars{{"code", "042917"}, {"site", "kcmeatup.com"}, {"email", "ann@example.com"}, {"expires_minutes", 10}};
  auto fresh = store->render("playground", "sign-in-new", vars, &err);
  ASSERT_TRUE(fresh) << err;
  EXPECT_EQ(fresh->subject, "Welcome to kcmeatup.com: your sign-in code");
  EXPECT_TRUE(has(fresh->text, "042917"));
  EXPECT_TRUE(has(fresh->html, ">042917</div>"));
  EXPECT_TRUE(has(fresh->html, "Welcome to kcmeatup.com"));
  EXPECT_TRUE(has(fresh->html, "Sent by kcmeatup.com to ann@example.com."));
  EXPECT_TRUE(has(fresh->text, "Sent by kcmeatup.com to ann@example.com."));
  EXPECT_FALSE(has(fresh->html, "href"));  // 013: never a link that signs in

  auto back = store->render("playground", "sign-in-returning", vars, &err);
  ASSERT_TRUE(back) << err;
  EXPECT_EQ(back->subject, "Your sign-in code for kcmeatup.com");
  EXPECT_TRUE(has(back->html, "Welcome back"));
  EXPECT_LT(back->text.size(), fresh->text.size());  // leaner
}

TEST_F(MailTemplatesTest, ValuesAreEscapedInHtmlAndLeftAloneInText) {
  std::string err;
  bj::object vars{{"kind", "agent"},  {"acting", "for you"}, {"client", "<b>Evil</b> & Co"},
                  {"from", "x.com"},  {"unchecked", false},   {"site", "a.com"}, {"email", "a@a.com"}};
  auto r = store->render("playground", "new-grant", vars, &err);
  ASSERT_TRUE(r) << err;
  EXPECT_TRUE(has(r->html, "&lt;b&gt;Evil&lt;/b&gt; &amp; Co"));
  EXPECT_FALSE(has(r->html, "<b>Evil"));
  EXPECT_TRUE(has(r->text, "<b>Evil</b> & Co"));
  EXPECT_FALSE(has(r->text, "registered itself"));
}

TEST_F(MailTemplatesTest, OverridesPerTenantThenEveryTenantThenDefault) {
  std::string err, source;
  Template mine{"Howdy from {{site}}", "Code: {{code}}", "<p>Code: <b>{{code}}</b></p>"};
  ASSERT_TRUE(store->put("_host", "sign-in-returning", Template{"Every {{site}}", "E {{code}}", ""}, &err)) << err;
  ASSERT_TRUE(store->put("playground", "sign-in-returning", mine, &err)) << err;
  store->get("playground", "sign-in-returning", &source);
  EXPECT_EQ(source, "tenant");
  store->get("other", "sign-in-returning", &source);
  EXPECT_EQ(source, "every-tenant");
  store->get("other", "sign-in-new", &source);
  EXPECT_EQ(source, "default");

  bj::object vars{{"code", "1"}, {"site", "s"}, {"email", "e@e.com"}};
  EXPECT_EQ(store->render("playground", "sign-in-returning", vars, &err)->subject, "Howdy from s");
  auto every = store->render("other", "sign-in-returning", vars, &err);
  EXPECT_EQ(every->subject, "Every s");
  EXPECT_TRUE(has(every->html, "<p style=\"margin:0 0 16px;\">E 1</p>"));  // HTML made from the text

  EXPECT_TRUE(store->remove("playground", "sign-in-returning"));
  EXPECT_FALSE(store->remove("playground", "sign-in-returning"));
  store->get("playground", "sign-in-returning", &source);
  EXPECT_EQ(source, "every-tenant");
}

TEST_F(MailTemplatesTest, ALayoutWrapsEveryMailAndCustomTemplatesAreListed) {
  std::string err;
  ASSERT_TRUE(store->put("playground", "layout",
                         Template{"", "{{{content}}}\n-- The KC Meat Up crew",
                                  "<html><body style=\"background:#7f1d1d\">{{{content}}}</body></html>"},
                         &err))
      << err;
  ASSERT_TRUE(store->put("playground", "order-ready", Template{"Your order is ready", "Pick it up, {{name}}.", ""}, &err))
      << err;
  auto r = store->render("playground", "order-ready", bj::object{{"name", "Ann"}}, &err);
  ASSERT_TRUE(r) << err;
  EXPECT_EQ(r->text, "Pick it up, Ann.\n-- The KC Meat Up crew");
  EXPECT_EQ(r->html, "<html><body style=\"background:#7f1d1d\"><p style=\"margin:0 0 16px;\">Pick it up, Ann.</p>\n</body></html>");

  auto box = store->wrap_text("playground", "Hi", "Line one\nline <two>\n\nNext", {});
  EXPECT_TRUE(has(box.html, "Line one<br>\nline &lt;two&gt;</p>"));
  EXPECT_TRUE(has(box.html, "<p style=\"margin:0 0 16px;\">Next</p>"));

  bool custom = false, layout = false;
  for (const auto& l : store->list("playground")) {
    custom |= l.name == "order-ready" && !l.builtin && l.source == "tenant";
    layout |= l.name == "layout" && l.builtin && l.source == "tenant";
  }
  EXPECT_TRUE(custom);
  EXPECT_TRUE(layout);
}

TEST_F(MailTemplatesTest, BadTemplatesAreRefused) {
  auto refuse = [&](std::string_view tenant, std::string_view name, Template t, std::string_view why) {
    std::string err;
    EXPECT_FALSE(store->put(tenant, name, t, &err)) << name;
    EXPECT_TRUE(has(err, why)) << err;
  };
  refuse("playground", "Bad_Name", Template{"s", "t", ""}, "[a-z]");
  refuse("../x", "ok", Template{"s", "t", ""}, "not a tenant");
  refuse("playground", "x", Template{"", "t", ""}, "subject is required");
  refuse("playground", "x", Template{"a\nb", "t", ""}, "one line");
  refuse("playground", "x", Template{"s", "", ""}, "text part");
  refuse("playground", "x", Template{"s", "{{#open}}", ""}, "text:");
  refuse("playground", "x", Template{"s", "t", std::string(kMaxPartBytes + 1, 'a')}, "64 KiB");
  refuse("playground", "layout", Template{"", "no content here", ""}, "{{{content}}}");
  refuse("playground", "layout", Template{"", "{{{content}}}", "<p>none</p>"}, "{{{content}}}");
  EXPECT_FALSE(fs::exists(dir / "playground" / "x.json"));
}
