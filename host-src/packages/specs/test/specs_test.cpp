#include <gtest/gtest.h>

#include "goober/specs/specs.h"

using namespace goober;
namespace fs = std::filesystem;

namespace {

// Shaped like Hevy's: OpenAPI 3.0, no operationIds, the key as a header parameter.
const char* kHevy = R"({"openapi":"3.0.0","info":{"title":"Hevy","version":"0.0.1"},
  "servers":[{"url":"https://api.hevyapp.com"}],
  "components":{"parameters":{"key":{"name":"api-key","in":"header","required":true,"schema":{"type":"string"}}}},
  "paths":{"/v1/workouts":{"get":{"parameters":[{"$ref":"#/components/parameters/key"},
      {"name":"page","in":"query"},{"name":"pageSize","in":"query"}]},
    "post":{"requestBody":{"content":{"application/json":{"schema":{}}}}}},
   "/v1/workouts/{workoutId}":{"parameters":[{"name":"workoutId","in":"path","required":true}],"get":{}}}})";

// Shaped like Strava's: Swagger 2.0 with a basePath and a multipart upload.
const char* kStrava = R"({"swagger":"2.0","info":{"title":"Strava","version":"3"},"host":"www.strava.com",
  "basePath":"/api/v3","schemes":["https"],"consumes":["application/json"],
  "paths":{"/uploads":{"post":{"operationId":"createUpload","consumes":["multipart/form-data"],
      "parameters":[{"name":"file","in":"formData","type":"file"},{"name":"data_type","in":"formData","type":"string"}]}},
   "/uploads/{uploadId}":{"get":{"operationId":"getUploadById","parameters":[{"name":"uploadId","in":"path","required":true,"type":"integer"}]}},
   "/athlete":{"get":{"operationId":"getLoggedInAthlete"}}}})";

}  // namespace

TEST(Specs, OpenApi3ByVerbAndPath) {
  std::string err;
  auto s = specs::parse(kHevy, &err);
  ASSERT_TRUE(s) << err;
  EXPECT_EQ(s->format, "openapi 3.0.0");
  EXPECT_EQ(s->server, "https://api.hevyapp.com");
  auto* list = s->find("GET /v1/workouts");
  ASSERT_TRUE(list);
  ASSERT_EQ(list->params.size(), 3u);
  EXPECT_EQ(list->params[0].name, "api-key");  // through the $ref
  EXPECT_EQ(list->params[0].in, "header");
  EXPECT_TRUE(s->find("post /v1/workouts"));
  EXPECT_EQ(s->find("POST /v1/workouts")->body_types.at(0), "application/json");
  auto* one = s->find("GET /v1/workouts/{workoutId}");
  ASSERT_TRUE(one);
  EXPECT_EQ(one->params.at(0).in, "path");  // the path-level parameter
  EXPECT_FALSE(s->find("DELETE /v1/workouts"));
  EXPECT_FALSE(s->find("listWorkouts"));
}

TEST(Specs, Swagger2ByOperationId) {
  std::string err;
  auto s = specs::parse(kStrava, &err);
  ASSERT_TRUE(s) << err;
  EXPECT_EQ(s->server, "https://www.strava.com/api/v3");
  auto* up = s->find("createUpload");
  ASSERT_TRUE(up);
  EXPECT_EQ(up->path, "/api/v3/uploads");  // the basePath prepended
  EXPECT_EQ(up->body_types.at(0), "multipart/form-data");
  EXPECT_EQ(s->find("getUploadById")->params.at(0).name, "uploadId");
  EXPECT_FALSE(specs::parse(R"({"openapi":"2.5"})", &err));
  EXPECT_FALSE(specs::parse("openapi: 3.0.0", &err));  // YAML: convert first
  EXPECT_NE(err.find("YAML"), std::string::npos);
}

TEST(Specs, TheStorePinsByHash) {
  auto dir = fs::path(GOOBER_TEST_TMP) / "specs_store";
  fs::remove_all(dir);
  specs::Store store(dir);
  std::string err;
  auto pin = store.put("hevy", kHevy, &err);
  ASSERT_TRUE(pin) << err;
  EXPECT_EQ(pin->size(), std::string("hevy@").size() + 12);
  EXPECT_EQ(*store.put("hevy", kHevy, &err), *pin);  // the same bytes, the same pin
  specs::Store fresh(dir);
  auto s = fresh.get(*pin, &err);
  ASSERT_TRUE(s) << err;
  EXPECT_TRUE(s->find("GET /v1/workouts"));
  EXPECT_FALSE(fresh.get("hevy@000000000000", &err));
  EXPECT_FALSE(store.put("Bad Name", kHevy, &err));
  EXPECT_EQ(fresh.list().size(), 1u);
}
