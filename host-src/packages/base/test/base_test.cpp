#include <gtest/gtest.h>

#include <filesystem>

#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/hash.h"
#include "goober/base/process.h"
#include "goober/base/names.h"
#include "goober/base/random.h"
#include "goober/base/time.h"

using namespace goober::base;

TEST(Hash, Sha256KnownVectors) {
  EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(sha256_hex(std::string(1000000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Hash, Crc32KnownVector) { EXPECT_EQ(crc32("123456789"), 0xCBF43926u); }

TEST(Encoding, Base64RoundTrip) {
  for (std::string s : {"", "f", "fo", "foo", "foob", "fooba", "foobar"}) {
    auto enc = base64_encode(s);
    auto dec = base64_decode(enc);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, s);
  }
  EXPECT_EQ(base64_encode("foobar"), "Zm9vYmFy");
  EXPECT_FALSE(base64_decode("Zm9v!").has_value());
  EXPECT_FALSE(base64_decode("Zg==Zg").has_value());
}

TEST(Encoding, Base64UrlForPkce) {
  // RFC 7636 appendix B: the S256 challenge of the example verifier.
  auto digest = sha256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
  EXPECT_EQ(base64url_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size())),
            "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
  EXPECT_EQ(base64url_encode("\xfb\xff"), "-_8");
}

TEST(Encoding, PercentDecode) {
  EXPECT_EQ(*percent_decode("a%20b"), "a b");
  EXPECT_EQ(*percent_decode("%2F"), "/");
  EXPECT_FALSE(percent_decode("%2").has_value());
  EXPECT_FALSE(percent_decode("%zz").has_value());
  EXPECT_EQ(percent_encode("/a b?c=d&e"), "%2Fa%20b%3Fc%3Dd%26e");
  EXPECT_EQ(*percent_decode(percent_encode("x/y~z é")), "x/y~z é");
  EXPECT_EQ(html_escape("<a href=\"x\">&'</a>"), "&lt;a href=&quot;x&quot;&gt;&amp;&#39;&lt;/a&gt;");
}

TEST(Names, Grammar) {
  EXPECT_TRUE(is_valid_name("playground"));
  EXPECT_TRUE(is_valid_name("bobs_plumbing2"));
  EXPECT_FALSE(is_valid_name(""));
  EXPECT_FALSE(is_valid_name("2fast"));
  EXPECT_FALSE(is_valid_name("_host"));
  EXPECT_FALSE(is_valid_name("Bobs"));
  EXPECT_FALSE(is_valid_name("bobs-plumbing"));
  EXPECT_FALSE(is_valid_name("goober_thing"));
  EXPECT_FALSE(is_valid_name(std::string(64, 'a')));
  EXPECT_TRUE(is_valid_name(std::string(63, 'a')));
}

TEST(Files, SafeRelativePaths) {
  EXPECT_TRUE(is_safe_relative_path("index.html"));
  EXPECT_TRUE(is_safe_relative_path("a/b/c.css"));
  EXPECT_FALSE(is_safe_relative_path(""));
  EXPECT_FALSE(is_safe_relative_path("/etc/passwd"));
  EXPECT_FALSE(is_safe_relative_path("a/../b"));
  EXPECT_FALSE(is_safe_relative_path("a//b"));
  EXPECT_FALSE(is_safe_relative_path("a/"));
  EXPECT_FALSE(is_safe_relative_path("./a"));
  EXPECT_FALSE(is_safe_relative_path("a\\b"));
}

TEST(Files, DurableWriteReadListRemove) {
  namespace fs = std::filesystem;
  fs::path root = fs::path(GOOBER_TEST_TMP) / "base_files";
  fs::remove_all(root);
  ASSERT_TRUE(write_file_durable(root / "x/y.txt", "hello"));
  ASSERT_TRUE(write_file_durable(root / "a.txt", "a"));
  EXPECT_EQ(*read_file(root / "x/y.txt"), "hello");
  EXPECT_EQ(list_files(root), (std::vector<std::string>{"a.txt", "x/y.txt"}));
  ASSERT_TRUE(remove_file_durable(root / "a.txt"));
  ASSERT_TRUE(remove_file_durable(root / "a.txt"));
  EXPECT_FALSE(read_file(root / "a.txt").has_value());
}

TEST(Files, TailLines) {
  namespace fs = std::filesystem;
  fs::path file = fs::path(GOOBER_TEST_TMP) / "base_tail/log.txt";
  fs::remove_all(file.parent_path());
  for (int i = 0; i < 10; ++i) append_line(file, std::to_string(i));
  EXPECT_EQ(tail_lines(file, 3), (std::vector<std::string>{"7", "8", "9"}));
}

TEST(Random, SeededIsDeterministic) {
  SeededRandom a(42), b(42), c(43);
  for (int i = 0; i < 100; ++i) {
    auto x = a.next();
    EXPECT_EQ(x, b.next());
    (void)c.next();
  }
  SeededRandom d(42), e(43);
  EXPECT_NE(d.next(), e.next());
}

TEST(Time, FormatUtc) { EXPECT_EQ(format_utc_ms(0), "1970-01-01T00:00:00.000Z"); }

TEST(Process, RunsWithoutAShellAndBoundsOutput) {
  auto r = run_process({"cat"}, "hello; rm -rf /", 1024, 5000);
  ASSERT_TRUE(r.started);
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.out, "hello; rm -rf /");
  auto big = run_process({"yes"}, "", 1000, 5000);
  EXPECT_TRUE(big.truncated);
  auto slow = run_process({"sleep", "5"}, "", 10, 200);
  EXPECT_TRUE(slow.timed_out);
  auto fails = run_process({"sh", "-c", "echo oops >&2; exit 3"}, "", 10, 5000);
  EXPECT_EQ(fails.exit_code, 3);
  EXPECT_EQ(fails.err, "oops\n");
  EXPECT_FALSE(run_process({"no-such-program-goober"}, "", 10, 1000).started);
}
