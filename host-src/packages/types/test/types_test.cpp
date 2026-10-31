#include <gtest/gtest.h>

#include <chrono>

#include "goober/types/types.h"

using namespace goober;
using namespace goober::types;

namespace {

const char* kOrders = R"({
  "openapi": "3.1.0", "info": {"title": "orders", "version": "7"}, "paths": {},
  "components": {"schemas": {
    "Order": {"type": "object", "required": ["id", "quantity"], "additionalProperties": false,
      "properties": {
        "id": {"type": "integer", "format": "int64"},
        "quantity": {"type": "integer", "format": "int32", "minimum": 1, "maximum": 100},
        "gift_wrap": {"type": "boolean", "default": false},
        "note": {"type": "string", "maxLength": 5},
        "kind": {"type": "string", "enum": ["box", "bag"]},
        "when": {"type": "string", "format": "date-time"},
        "lines": {"type": "array", "maxItems": 2, "items": {"$ref": "#/components/schemas/Line"}}}},
    "Line": {"type": "object", "required": ["sku"], "additionalProperties": false,
      "properties": {"sku": {"type": "string", "maxLength": 20}, "price": {"type": "number", "format": "double"}}}
  }}})";

std::string error_of(const Types& t, const char* type, const char* json) {
  std::string err;
  auto v = json::parse(json, &err);
  EXPECT_TRUE(v) << json;
  auto out = t.check(type, *v, &err);
  return out ? "" : err;
}

}  // namespace

TEST(Types, ChecksValuesFillsDefaultsAndStringifiesInt64) {
  std::string err;
  auto t = Types::parse(kOrders, &err);
  ASSERT_TRUE(t) << err;
  auto v = json::parse(R"({"id": 42, "quantity": 3, "lines": [{"sku": "a", "price": 2.5}]})", &err);
  auto out = t->check("Order", *v, &err);
  ASSERT_TRUE(out) << err;
  EXPECT_EQ(json::dump(*out), R"({"id":"42","quantity":3,"gift_wrap":false,"lines":[{"sku":"a","price":2.5E0}]})");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": "9223372036854775807", "quantity": 1})"), "");

  EXPECT_EQ(error_of(*t, "Order", R"({"quantity": 3})"), "$.id: is required");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 0})"), "$.quantity: must be at least 1");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3, "extra": 1})"),
            "$: unknown field \"extra\" (objects are closed)");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3, "note": "toolong"})"), "$.note: is longer than 5 characters");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3, "note": "héllo"})"), "");  // characters, not bytes
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3, "kind": "crate"})"), "$.kind: must be one of box, bag");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3, "note": null})"), "$.note: is null; leave the field out instead");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": "12x", "quantity": 3})"), "$.id: must be an int64, as a string of digits");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3.5})"), "$.quantity: must be an int32");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3, "lines": [{"sku":"a"},{"sku":"b"},{"sku":"c"}]})"),
            "$.lines: holds more than 2 items");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3, "lines": [{"price": 1}]})"), "$.lines[0].sku: is required");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3, "when": "2026-10-27T13:00:00Z"})"), "");
  EXPECT_EQ(error_of(*t, "Order", R"({"id": 1, "quantity": 3, "when": "2026-10-27 13:00"})"),
            "$.when: must be an RFC 3339 instant in UTC, like 2026-10-27T13:00:00Z");
  EXPECT_EQ(error_of(*t, "Order", R"([1])"), "$: must be an object");
}

TEST(Types, RefusesAnythingOutsideTheSubset) {
  auto refuse = [](const std::string& schema, const char* why) {
    std::string err;
    std::string doc = R"({"openapi":"3.1.0","components":{"schemas":{"T":)" + schema + "}}}";
    EXPECT_FALSE(Types::parse(doc, &err)) << schema;
    EXPECT_NE(err.find(why), std::string::npos) << err;
  };
  refuse(R"({"type":"integer"})", "int32 or int64");
  refuse(R"({"type":"number"})", "float or double");
  refuse(R"({"type":"object","properties":{}})", "additionalProperties");
  refuse(R"({"type":"object","additionalProperties":true})", "additionalProperties");
  refuse(R"({"type":"array","items":{"type":"boolean"}})", "maxItems");
  refuse(R"({"oneOf":[{"type":"boolean"}]})", "outside goober's subset");
  refuse(R"({"type":["string","null"]})", "one type");
  refuse(R"({"type":"string","pattern":"a+"})", "linear time");
  refuse(R"({"type":"string","maximum":3})", "doesn't apply");
  refuse(R"({"type":"string","format":"token"})", "token");
  refuse(R"({"$ref":"https://evil.test/x.json"})", "same document");
  refuse(R"({"$ref":"#/components/schemas/Nope"})", "unknown type Nope");
  refuse(R"({"type":"object","additionalProperties":false,"properties":{"me":{"$ref":"#/components/schemas/T"}}})",
         "recursive");
  refuse(R"({"type":"object","additionalProperties":false,"required":["x"]})", "isn't a property");
  refuse(R"({"type":"integer","format":"int32","default":"x"})", "default isn't valid");
  std::string err;
  EXPECT_FALSE(Types::parse(R"({"swagger":"2.0"})", &err));
  EXPECT_FALSE(Types::parse(R"({"openapi":"3.1.0","components":{"schemas":{"bad-name":{"type":"boolean"}}}})", &err));
}

TEST(Types, QueryTextAndJsonSchemaForAgents) {
  std::string err;
  auto since = parse_schema(*json::parse(R"({"type":"integer","format":"int32","default":0,"maximum":10})", &err),
                            "query.since", true, &err);
  ASSERT_TRUE(since) << err;
  EXPECT_EQ(json::dump(*check_text(**since, "7", &err)), "7");
  EXPECT_FALSE(check_text(**since, "eleven", &err));
  EXPECT_FALSE(check_text(**since, "11", &err));
  EXPECT_EQ(err, "must be at most 10");
  auto flag = parse_schema(*json::parse(R"({"type":"boolean"})", &err), "q", true, &err);
  EXPECT_EQ(json::dump(*check_text(**flag, "true", &err)), "true");
  EXPECT_FALSE(check_text(**flag, "yes", &err));
  EXPECT_FALSE(parse_schema(*json::parse(R"({"type":"object","additionalProperties":false})", &err), "q", true, &err));

  auto t = Types::parse(kOrders, &err);
  auto schema = json::dump(t->json_schema("Order"));
  EXPECT_NE(schema.find(R"("lines":{"type":"array","items":{"type":"object")"), std::string::npos) << schema;
  EXPECT_NE(schema.find(R"("id":{"type":"string","pattern")"), std::string::npos) << schema;
  EXPECT_NE(schema.find(R"("required":["id","quantity"])"), std::string::npos) << schema;
  EXPECT_EQ(json_type_name("json:Order"), "Order");
  EXPECT_EQ(json_type_name("text"), "");
}

TEST(Types, RefusesDocumentsThatExpandEnormously) {
  // Each type holds two of the next: 2^30 nodes once expanded.
  std::string schemas;
  for (int i = 0; i < 30; ++i) {
    schemas += (i ? "," : "") + std::string("\"T") + std::to_string(i) +
               "\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"a\":{\"$ref\":\"#/components/schemas/T" +
               std::to_string(i + 1) + "\"},\"b\":{\"$ref\":\"#/components/schemas/T" + std::to_string(i + 1) + "\"}}}";
  }
  schemas += ",\"T30\":{\"type\":\"boolean\"}";
  std::string err;
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(Types::parse(R"({"openapi":"3.1.0","components":{"schemas":{)" + schemas + "}}}", &err));
  EXPECT_NE(err.find("expands"), std::string::npos) << err;
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

// const (011 § 2): one exact value, for a string, boolean, integer, or number; an int64's is
// compared as its string of digits, as int64 values reach the box.
TEST(Types, ConstAllowsOneValue) {
  std::string err;
  auto t = Types::parse(R"({"openapi":"3.1.0","components":{"schemas":{"Event":{
      "type":"object","additionalProperties":false,"required":["kind"],
      "properties":{"kind":{"type":"string","const":"signup"},
                    "version":{"type":"integer","format":"int32","const":2},
                    "box":{"type":"integer","format":"int64","const":"4294967296"},
                    "on":{"type":"boolean","const":true},
                    "rate":{"type":"number","format":"double","const":1.5}}}}}})", &err);
  ASSERT_TRUE(t) << err;
  EXPECT_EQ(error_of(*t, "Event", R"({"kind":"signup","version":2,"box":4294967296,"on":true,"rate":1.5})"), "");
  EXPECT_EQ(error_of(*t, "Event", R"({"kind":"signup","box":"4294967296"})"), "");
  EXPECT_EQ(error_of(*t, "Event", R"({"kind":"login"})"), R"($.kind: must be "signup")");
  EXPECT_EQ(error_of(*t, "Event", R"({"kind":"signup","version":3})"), "$.version: must be 2");
  EXPECT_EQ(error_of(*t, "Event", R"({"kind":"signup","box":"7"})"), R"($.box: must be "4294967296")");
  EXPECT_EQ(error_of(*t, "Event", R"({"kind":"signup","on":false})"), "$.on: must be true");
  EXPECT_EQ(json::dump(t->json_schema("Event").at("properties").at("kind").at("const")), R"("signup")");
  auto refuse = [](const std::string& schema, const char* why) {
    std::string e;
    std::string doc = R"({"openapi":"3.1.0","components":{"schemas":{"T":)" + schema + "}}}";
    EXPECT_FALSE(Types::parse(doc, &e)) << schema;
    EXPECT_NE(e.find(why), std::string::npos) << e;
  };
  refuse(R"({"type":"string","const":3})", "own type");
  refuse(R"({"type":"integer","format":"int32","const":"3"})", "own type");
  refuse(R"({"type":"array","maxItems":2,"items":{"type":"boolean"},"const":[true]})", "string, boolean, integer, or number");
  refuse(R"({"type":"string","pattern":"^[a-z]+$"})", "isn't supported yet");
}
