// The host's default mail templates (design/machine/022.MailTemplates.md). The HTML is
// written for mail clients, not browsers: tables for layout, every style inline, no images,
// no scripts, and no links in anything about signing in (013: a link in a sign-in mail is
// what a phisher would send).

#include "goober/mail_templates/templates.h"

namespace goober::mail::templates {

namespace {

#define GOOBER_FONT "-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif"
#define GOOBER_MONO "ui-monospace,SFMono-Regular,Menlo,Consolas,'Liberation Mono',monospace"
#define GOOBER_H1 "margin:0 0 12px;font:600 22px/1.3 " GOOBER_FONT ";color:#18181b;"
#define GOOBER_P "margin:0 0 16px;"
#define GOOBER_SMALL "margin:0 0 8px;font-size:14px;line-height:1.5;color:#71717a;"
#define GOOBER_CODE                                                                                                \
  "margin:0 0 20px;padding:18px 12px;background:#f4f4f5;border:1px solid #e4e4e7;border-radius:8px;"            \
  "text-align:center;font:600 32px/1 " GOOBER_MONO ";letter-spacing:8px;color:#18181b;"

const char* kLayoutHtml = R"(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light">
<title>{{subject}}</title>
</head>
<body style="margin:0;padding:0;background:#f4f4f5;">
<div style="display:none;max-height:0;overflow:hidden;mso-hide:all;">{{preheader}}</div>
<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0" style="background:#f4f4f5;">
<tr><td align="center" style="padding:32px 16px;">
<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0" style="max-width:560px;background:#ffffff;border:1px solid #e4e4e7;border-radius:12px;">
<tr><td style="padding:24px 32px 0 32px;font:600 14px/1.4 )" GOOBER_FONT R"(;color:#52525b;letter-spacing:0.2px;">{{site}}</td></tr>
<tr><td style="padding:16px 32px 32px 32px;font:16px/1.6 )" GOOBER_FONT R"(;color:#3f3f46;">
{{{content}}}
</td></tr>
</table>
<p style="margin:16px 0 0;font:12px/1.5 )" GOOBER_FONT R"(;color:#a1a1aa;">Sent by {{site}} to {{email}}.</p>
</td></tr>
</table>
</body>
</html>
)";

// The unsubscribe link and the reason a mail came are the host's footer, added after the
// layout so no override can drop them (design/machine/029.Invites.md § 3).
const char* kLayoutText = R"({{{content}}}

--
Sent by {{site}} to {{email}}.
)";

const char* kSignInNewHtml = R"(<h1 style=")" GOOBER_H1 R"(">Welcome to {{site}}</h1>
<p style=")" GOOBER_P R"(">Here is your code to finish signing in. Type it into the sign-in page you already have open.</p>
<div style=")" GOOBER_CODE R"(">{{code}}</div>
<p style=")" GOOBER_SMALL R"(">It expires in {{expires_minutes}} minutes. Never share it with anyone: {{site}} will never ask you for it.</p>
<p style=")" GOOBER_SMALL R"(">If you didn't ask for this, ignore this email. No account is made until the code is used.</p>)";

const char* kSignInNewText = R"(Welcome to {{site}}.

Your code to finish signing in is {{code}}.

Type it into the sign-in page you already have open. It expires in {{expires_minutes}} minutes.
Never share it with anyone: {{site}} will never ask you for it.

If you didn't ask for this, ignore this email. No account is made until the code is used.)";

const char* kSignInReturningHtml = R"(<h1 style=")" GOOBER_H1 R"(">Welcome back</h1>
<p style=")" GOOBER_P R"(">Your sign-in code for {{site}}:</p>
<div style=")" GOOBER_CODE R"(">{{code}}</div>
<p style=")" GOOBER_SMALL R"(">It expires in {{expires_minutes}} minutes. Never share it. If you didn't ask for it, ignore this email.</p>)";

const char* kSignInReturningText = R"(Welcome back. Your sign-in code for {{site}} is {{code}}.

It expires in {{expires_minutes}} minutes. Never share it. If you didn't ask for it, ignore this email.)";

const char* kNewGrantHtml = R"(<h1 style=")" GOOBER_H1 R"(">A new {{kind}} can act for you</h1>
<p style=")" GOOBER_P R"(">{{client}}, from {{from}}, was just allowed to act {{acting}} on {{site}}.{{#unchecked}} It registered itself, so nobody has checked it.{{/unchecked}}</p>
<p style=")" GOOBER_SMALL R"(">If that was you, there is nothing to do. If it wasn't, open {{site}} yourself, go to your account page, and remove it.</p>)";

const char* kNewGrantText = R"(A new {{kind}} can act for you on {{site}}: {{client}}, from {{from}}, was just allowed to act {{acting}}.{{#unchecked}} It registered itself, so nobody has checked it.{{/unchecked}}

If that was you, there is nothing to do. If it wasn't, open {{site}} yourself, go to your account page, and remove it.)";

const char* kAgentTokenHtml = R"(<h1 style=")" GOOBER_H1 R"(">An agent can act for you</h1>
<p style=")" GOOBER_P R"(">An operator of {{site}} just gave the agent {{client}} a token to act for you for {{days}} days.</p>
<p style=")" GOOBER_SMALL R"(">If you expected that, there is nothing to do. If not, open {{site}} yourself, go to your account page, and remove it.</p>)";

const char* kAgentTokenText = R"(An operator of {{site}} just gave the agent {{client}} a token to act for you for {{days}} days.

If you expected that, there is nothing to do. If not, open {{site}} yourself, go to your account page, and remove it.)";

#define GOOBER_BUTTON                                                                                                \
  "display:inline-block;padding:12px 20px;background:#2457d6;color:#ffffff;border-radius:8px;font-weight:600;" \
  "text-decoration:none;"

const char* kInviteHtml = R"(<h1 style=")" GOOBER_H1 R"(">{{#reminder}}A reminder: {{/reminder}}{{inviter}} invited you to {{site}}</h1>
{{#note}}<p style=")" GOOBER_P R"(border-left:3px solid #e4e4e7;padding-left:12px;color:#52525b;">{{note}}</p>{{/note}}
{{#has_roles}}<p style=")" GOOBER_P R"(">You'd join as: {{#roles}}{{role}} {{/roles}}</p>{{/has_roles}}
{{#has_lists}}<p style=")" GOOBER_P R"(">You'd get:</p><ul>{{#lists}}<li><strong>{{title}}</strong>: {{about}}</li>{{/lists}}</ul>{{/has_lists}}
<p style=")" GOOBER_P R"("><a href="{{accept_url}}" style=")" GOOBER_BUTTON R"(">See the invite</a></p>
{{#terms_url}}<p style=")" GOOBER_SMALL R"(">Joining means accepting {{site}}'s <a href="{{terms_url}}">terms</a>{{#privacy_url}} and <a href="{{privacy_url}}">privacy policy</a>{{/privacy_url}}.</p>{{/terms_url}}
<p style=")" GOOBER_SMALL R"(">The invite is open until {{expires}}. Not interested? <a href="{{decline_url}}">Don't invite me again</a>: {{site}} won't email you after that.</p>)";

const char* kInviteText = R"({{#reminder}}A reminder: {{/reminder}}{{inviter}} invited you to {{site}}.
{{#note}}
"{{note}}"
{{/note}}{{#has_roles}}
You'd join as: {{#roles}}{{role}} {{/roles}}
{{/has_roles}}{{#has_lists}}
You'd get:{{#lists}}
- {{title}}: {{about}}{{/lists}}
{{/has_lists}}
See the invite: {{accept_url}}
{{#terms_url}}
Joining means accepting {{site}}'s terms ({{terms_url}}){{#privacy_url}} and privacy policy ({{privacy_url}}){{/privacy_url}}.
{{/terms_url}}
The invite is open until {{expires}}. Not interested? Don't invite me again: {{decline_url}})";

const char* kListConfirmHtml = R"(<h1 style=")" GOOBER_H1 R"(">Subscribe to {{list_title}}?</h1>
<p style=")" GOOBER_P R"(">{{list_about}}</p>
<p style=")" GOOBER_P R"("><a href="{{confirm_url}}" style=")" GOOBER_BUTTON R"(">Yes, subscribe me</a></p>
<p style=")" GOOBER_SMALL R"(">If you didn't ask for this, ignore this email. Nothing more comes unless you confirm.</p>)";

const char* kListConfirmText = R"(Subscribe to {{list_title}} from {{site}}?

{{list_about}}

Yes, subscribe me: {{confirm_url}}

If you didn't ask for this, ignore this email. Nothing more comes unless you confirm.)";

}  // namespace

const std::vector<Builtin>& builtins() {
  static const std::vector<Builtin> kBuiltins = {
      {std::string(kLayout),
       "Wraps every mail. {{{content}}} is the rendered mail; also subject, preheader (the text's first line), site, "
       "email, and, on boxes' mail only, unsubscribe (the one-click link). The host adds its own footer after it "
       "(why the mail came, the postal address, the link to stop it), so a layout needn't. Its subject is unused.",
       bj::object{{"content", "<p>Hello.</p>"}, {"subject", "Hello"}, {"preheader", "Hello."}, {"site", "example.com"},
                  {"email", "ann@example.com"}},
       Template{"{{subject}}", kLayoutText, kLayoutHtml}},
      {"sign-in-new",
       "The sign-in code for an address not yet proved: a first sign-in, or an invite taken up.",
       bj::object{{"code", "042917"}, {"site", "example.com"}, {"email", "ann@example.com"}, {"expires_minutes", 10}},
       Template{"Welcome to {{site}}: your sign-in code", kSignInNewText, kSignInNewHtml}},
      {"sign-in-returning",
       "The sign-in code for an address proved before.",
       bj::object{{"code", "042917"}, {"site", "example.com"}, {"email", "ann@example.com"}, {"expires_minutes", 10}},
       Template{"Your sign-in code for {{site}}", kSignInReturningText, kSignInReturningHtml}},
      {"new-grant",
       "Sent when a person lets an app or agent act for them. kind is app or agent; acting is \"as you\" or \"for you\"; "
       "unchecked is true for a client that registered itself.",
       bj::object{{"kind", "agent"}, {"acting", "for you"}, {"client", "Grok"}, {"from", "grok.com"},
                  {"unchecked", true}, {"site", "example.com"}, {"email", "ann@example.com"}},
       Template{"A new {{kind}} can act for you on {{site}}", kNewGrantText, kNewGrantHtml}},
      {"invite",
       "An invite to join a site (029 § 2.3), and its one reminder (reminder true). The text and HTML must each keep "
       "accept_url and decline_url, or the host sends its default instead; the host adds its footer after it. "
       "roles is [{role}], lists [{title, about}]; has_roles and has_lists say whether either is empty.",
       bj::object{{"inviter", "Ann (ann@example.com)"}, {"inviter_email", "ann@example.com"}, {"note", "Come grill with us"},
                  {"roles", bj::array{bj::object{{"role", "member"}}}},
                  {"lists", bj::array{bj::object{{"title", "The daily digest"}, {"about", "One email a day."}}}},
                  {"terms_url", "https://example.com/terms"}, {"privacy_url", "https://example.com/privacy"},
                  {"accept_url", "https://example.com/identity/invite/1/abc"},
                  {"decline_url", "https://example.com/identity/invite/1/abc/decline"},
                  {"postal_address", "1 Main St, Kansas City, MO"}, {"expires", "2026-11-28"}, {"reminder", false},
                  {"has_roles", true}, {"has_lists", true},
                  {"site", "example.com"}, {"email", "bob@example.com"}},
       Template{"{{#reminder}}Reminder: {{/reminder}}{{inviter}} invited you to {{site}}", kInviteText, kInviteHtml}},
      {"list-confirm",
       "Asks someone to confirm a list a box asked to subscribe them to (029 § 4, double opt-in). It must keep "
       "confirm_url, or the host sends its default instead.",
       bj::object{{"list_title", "The daily digest"}, {"list_about", "One email a day."},
                  {"confirm_url", "https://example.com/goober/list/kc/abc/digest/def"}, {"site", "example.com"},
                  {"email", "bob@example.com"}},
       Template{"Confirm: {{list_title}} from {{site}}", kListConfirmText, kListConfirmHtml}},
      {"agent-token",
       "Sent when an operator mints an agent token for a person.",
       bj::object{{"client", "Grok"}, {"days", 30}, {"site", "example.com"}, {"email", "ann@example.com"}},
       Template{"An agent can act for you on {{site}}", kAgentTokenText, kAgentTokenHtml}},
  };
  return kBuiltins;
}

const Builtin* builtin(std::string_view name) {
  for (const auto& b : builtins()) {
    if (b.name == name) return &b;
  }
  return nullptr;
}

}  // namespace goober::mail::templates
