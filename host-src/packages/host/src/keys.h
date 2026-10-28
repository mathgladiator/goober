#pragma once

// Credentials for external services (030 § credentials; design/machine/015.Network.md):
// storage/keys/system/$name and storage/keys/tenant/$tenant/$name, mode 0600. Write-only:
// operators set and remove keys, and see names and fingerprints, never values. The host
// reads a value only to attach it to an outgoing request. There is no vault yet (064).

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "goober/aws/aws.h"
#include "goober/json/json.h"

namespace goober::host {

class KeyStore {
 public:
  explicit KeyStore(std::filesystem::path storage) : root_(std::move(storage) / "keys") {}
  // scope is "system" or "tenant" (with tenant set). Values are 1 to 8192 printable bytes.
  bool set(std::string_view scope, std::string_view tenant, std::string_view name, std::string_view value,
           int64_t now_ms, std::string* error);
  bool remove(std::string_view scope, std::string_view tenant, std::string_view name);
  // A credentials name from a manifest, like keys/system/mailer or keys/tenant/{!tenant}/stripe.
  std::optional<std::string> resolve(std::string_view credentials, std::string_view tenant) const;
  // The AWS keys for SES and S3: keys/system/aws_access_key_id and aws_secret_access_key, and
  // aws_session_token when set. nullopt unless both of the first two are.
  std::optional<aws::Credentials> aws_credentials() const;
  // Names, scopes, when each was set, and a short fingerprint; never values.
  boost::json::array list() const;

 private:
  std::optional<std::filesystem::path> file(std::string_view scope, std::string_view tenant, std::string_view name) const;
  std::filesystem::path root_;
};

}  // namespace goober::host
