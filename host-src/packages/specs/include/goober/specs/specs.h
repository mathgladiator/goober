#pragma once

// Pinned OpenAPI documents (design/machine/034.SpeakingHttp.md § 2). A spec is parsed into the
// operations a call needs: verb, path template, parameter places, and request body types.

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace goober::specs {

struct Param {
  std::string name;
  std::string in;  // path, query, header, cookie
  bool required = false;
};

struct Operation {
  std::string id;      // operationId, or ""
  std::string verb;    // get, post, put, patch, delete, head, options
  std::string path;    // the template as the spec spells it, with a Swagger basePath prepended
  std::vector<Param> params;
  std::vector<std::string> body_types;  // media types the request body takes, or none
  std::string ref() const { return verb + " " + path; }
};

struct Spec {
  std::string format;  // "openapi 3.1.0", "swagger 2.0"
  std::string title, version;
  std::string server;  // the first absolute server URL, or ""
  std::vector<Operation> ops;
  // An operation by operationId, or by "VERB /path" (the verb in any case).
  const Operation* find(std::string_view ref) const;
};

std::optional<Spec> parse(std::string_view text, std::string* error);

// A spec's name: [a-z][a-z0-9_-]*, at most 64.
bool valid_name(std::string_view name);

// storage/specs/$name/$sha256.json, by pin "name@sha12".
class Store {
 public:
  explicit Store(std::filesystem::path dir);
  // Parses, stores, and answers the pin. Refuses a bad name or a spec that doesn't parse.
  std::optional<std::string> put(std::string_view name, std::string_view text, std::string* error);
  std::shared_ptr<const Spec> get(std::string_view pin, std::string* error);
  std::optional<std::string> text(std::string_view pin);
  // Every stored spec: {pin, title, version, operations}.
  struct Listed {
    std::string pin, format, title, version;
    size_t operations = 0;
  };
  std::vector<Listed> list();

 private:
  std::optional<std::filesystem::path> file_of(std::string_view pin);
  std::filesystem::path dir_;
  std::mutex mu_;
  std::map<std::string, std::shared_ptr<const Spec>> cache_;
};

}  // namespace goober::specs
