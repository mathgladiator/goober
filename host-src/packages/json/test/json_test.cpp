#include <gtest/gtest.h>

#include "goober/json/json.h"

using namespace goober::json;

TEST(Json, ParseAndFields) {
  std::string err;
  auto v = parse(R"({"a":"x","n":5,"b":true,"o":{},"l":[1]})", &err);
  ASSERT_TRUE(v.has_value()) << err;
  const auto& obj = v->get_object();
  EXPECT_EQ(*get_string(obj, "a", &err), "x");
  EXPECT_EQ(*get_int(obj, "n", &err), 5);
  EXPECT_EQ(*get_bool(obj, "b", &err), true);
  EXPECT_NE(get_object(obj, "o", &err), nullptr);
  EXPECT_NE(get_array(obj, "l", &err), nullptr);
  EXPECT_FALSE(get_string(obj, "missing", &err).has_value());
}

TEST(Json, WrongTypeSetsError) {
  std::string err;
  auto v = parse(R"({"n":"five"})", &err);
  EXPECT_FALSE(get_int(v->get_object(), "n", &err).has_value());
  EXPECT_NE(err.find("\"n\""), std::string::npos);
}

TEST(Json, InvalidText) {
  std::string err;
  EXPECT_FALSE(parse("{nope", &err).has_value());
  EXPECT_FALSE(err.empty());
}

TEST(Json, PrettyIsParseable) {
  std::string err;
  auto v = parse(R"({"a":[1,2,{"b":null}],"c":{}})", &err);
  auto again = parse(pretty(*v), &err);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*v, *again);
}
