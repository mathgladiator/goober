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

}  // namespace goober::aws
