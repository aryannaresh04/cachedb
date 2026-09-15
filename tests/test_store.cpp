#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "store.h"

#include <string>

using cachedb::Store;

TEST_CASE("set and get") {
  Store s;

  SUBCASE("a value comes back") {
    s.set("username", "aryan");
    REQUIRE(s.get("username").has_value());
    CHECK(*s.get("username") == "aryan");
    CHECK(s.exists("username"));
    CHECK(s.size() == 1);
  }

  SUBCASE("a key that was never written is absent") {
    CHECK_FALSE(s.get("nothing").has_value());
    CHECK_FALSE(s.exists("nothing"));
    CHECK(s.size() == 0);
  }

  SUBCASE("writing the same key twice overwrites") {
    s.set("k", "first");
    s.set("k", "second");
    CHECK(*s.get("k") == "second");
    CHECK(s.size() == 1);
  }

  SUBCASE("an empty value is stored, not treated as missing") {
    // GET has to distinguish these: an empty value replies "$0", a missing
    // key replies "$-1".
    s.set("k", "");
    REQUIRE(s.get("k").has_value());
    CHECK(*s.get("k") == "");
    CHECK(s.exists("k"));
  }

  SUBCASE("keys and values are binary safe") {
    const std::string key("a\0b", 3);
    const std::string value("x\0\r\ny", 5);
    s.set(key, value);
    REQUIRE(s.get(key).has_value());
    CHECK(*s.get(key) == value);
    // The key is the whole three bytes, not the part before the NUL.
    CHECK_FALSE(s.get("a").has_value());
  }
}

TEST_CASE("delete") {
  Store s;
  s.set("k", "v");

  SUBCASE("deleting a live key reports it and hides it") {
    CHECK(s.del("k"));
    CHECK_FALSE(s.get("k").has_value());
    CHECK_FALSE(s.exists("k"));
    CHECK(s.size() == 0);
  }

  SUBCASE("deleting a key that is not there removes nothing") {
    CHECK_FALSE(s.del("absent"));
    CHECK_FALSE(s.get("absent").has_value());
    CHECK(s.size() == 1);  // "k" is untouched
  }

  SUBCASE("deleting twice only counts the first time") {
    CHECK(s.del("k"));
    CHECK_FALSE(s.del("k"));
    CHECK(s.size() == 0);
  }

  SUBCASE("a deleted key can be written again") {
    s.del("k");
    s.set("k", "again");
    REQUIRE(s.get("k").has_value());
    CHECK(*s.get("k") == "again");
    CHECK(s.size() == 1);
  }
}

TEST_CASE("a value handed out stays valid while other keys are written") {
  // std::map entries do not move, which is what makes it safe for get() to
  // return a view rather than a copy. Under ASan a mistake here is a
  // use-after-free, not a silent wrong answer.
  Store s;
  s.set("k", "value");
  const auto view = s.get("k");
  REQUIRE(view.has_value());

  for (int i = 0; i < 1000; ++i) s.set("filler" + std::to_string(i), "x");

  CHECK(*view == "value");
}
