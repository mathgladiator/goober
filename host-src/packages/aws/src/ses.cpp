// SES v2 SendEmail (https://docs.aws.amazon.com/ses/latest/APIReference-V2/API_SendEmail.html),
// with the message as raw MIME so its headers are the host's own.

#include "goober/aws/aws.h"
#include "goober/base/encoding.h"
#include "goober/json/json.h"

namespace goober::aws {

SesResult ses_send(const SesMessage& message, const Credentials& creds, const std::string& region,
                   const std::filesystem::path& scratch, const std::string& endpoint) {
  namespace bj = boost::json;
  Request r;
  r.service = "ses";
  r.region = region;
  r.method = "POST";
  r.path = "/v2/email/outbound-emails";
  if (endpoint.empty()) {
    r.endpoint.host = "email." + region + ".amazonaws.com";
  } else {
    auto colon = endpoint.find("://");
    r.endpoint.scheme = colon == std::string::npos ? "https" : endpoint.substr(0, colon);
    r.endpoint.host = colon == std::string::npos ? endpoint : endpoint.substr(colon + 3);
    while (!r.endpoint.host.empty() && r.endpoint.host.back() == '/') r.endpoint.host.pop_back();
  }
  r.headers.emplace_back("content-type", "application/json");
  bj::object body{{"FromEmailAddress", message.from},
                  {"Destination", bj::object{{"ToAddresses", bj::array{bj::value(message.to)}}}},
                  {"Content", bj::object{{"Raw", bj::object{{"Data", base::base64_encode(message.raw)}}}}}};
  if (!message.configuration_set.empty()) body["ConfigurationSetName"] = message.configuration_set;
  r.body = json::dump(body);
  r.timeout_ms = 30000;
  Reply reply = perform(r, creds, scratch);
  SesResult out;
  out.status = reply.status;
  if (reply.ok()) {
    std::string err;
    if (auto v = json::parse(reply.body, &err); v && v->is_object()) {
      if (auto* id = v->as_object().if_contains("MessageId"); id && id->is_string()) out.message_id = std::string(id->as_string());
    }
    out.sent = true;
    return out;
  }
  out.error = reply.error;
  // 403 is credentials or an identity not yet verified: an operator fixes those, so it waits
  // and tries again rather than dropping the mail.
  out.permanent = reply.status >= 400 && reply.status < 500 && reply.status != 429 && reply.status != 408 &&
                  reply.status != 403;
  return out;
}

// SES v2 GetEmailIdentity (https://docs.aws.amazon.com/ses/latest/APIReference-V2/API_GetEmailIdentity.html).
SesIdentity ses_identity(const std::string& identity, const Credentials& creds, const std::string& region,
                         const std::filesystem::path& scratch, const std::string& endpoint) {
  Request r;
  r.service = "ses";
  r.region = region;
  r.method = "GET";
  r.path = "/v2/email/identities/" + base::percent_encode(identity);
  if (endpoint.empty()) {
    r.endpoint.host = "email." + region + ".amazonaws.com";
  } else {
    auto colon = endpoint.find("://");
    r.endpoint.scheme = colon == std::string::npos ? "https" : endpoint.substr(0, colon);
    r.endpoint.host = colon == std::string::npos ? endpoint : endpoint.substr(colon + 3);
    while (!r.endpoint.host.empty() && r.endpoint.host.back() == '/') r.endpoint.host.pop_back();
  }
  r.timeout_ms = 10000;
  Reply reply = perform(r, creds, scratch);
  SesIdentity out;
  if (reply.status == 404) {
    out.ok = true;
    return out;
  }
  if (!reply.ok()) {
    out.error = reply.error.empty() ? "SES answered " + std::to_string(reply.status) : reply.error;
    return out;
  }
  std::string err;
  auto v = json::parse(reply.body, &err);
  if (!v || !v->is_object()) {
    out.error = "SES's answer isn't JSON";
    return out;
  }
  out.ok = out.exists = true;
  const auto& o = v->as_object();
  auto str = [](const boost::json::object& x, const char* k) {
    auto* s = x.if_contains(k);
    return s && s->is_string() ? std::string(s->as_string()) : std::string();
  };
  if (auto* b = o.if_contains("VerifiedForSendingStatus"); b && b->is_bool()) out.verified_for_sending = b->as_bool();
  out.verification_status = str(o, "VerificationStatus");
  if (auto* d = o.if_contains("DkimAttributes"); d && d->is_object()) {
    out.dkim_status = str(d->as_object(), "Status");
    if (auto* s = d->as_object().if_contains("SigningEnabled"); s && s->is_bool()) out.dkim_signing = s->as_bool();
    if (auto* t = d->as_object().if_contains("Tokens"); t && t->is_array()) {
      for (const auto& tok : t->as_array()) {
        if (tok.is_string()) out.dkim_tokens.emplace_back(tok.as_string());
      }
    }
  }
  return out;
}

}  // namespace goober::aws
