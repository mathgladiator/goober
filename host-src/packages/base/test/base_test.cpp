#include <gtest/gtest.h>

#include <fcntl.h>
#include <sstream>

#include <filesystem>

#include "goober/base/chacha20.h"
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

TEST(Hash, Sha256InPiecesMatchesWhole) {
  std::string data;
  for (int i = 0; i < 1000; ++i) data += char(i * 7 + 3);
  for (size_t split : {size_t(0), size_t(1), size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(999)}) {
    Sha256 h;
    h.update(std::string_view(data).substr(0, split));
    h.update(std::string_view(data).substr(split));
    auto digest = h.finish();
    EXPECT_EQ(hex_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), 32)), sha256_hex(data)) << split;
  }
  // finish() starts over: the next digest is of new bytes alone.
  Sha256 h;
  h.update("ignored");
  h.finish();
  h.update("abc");
  auto digest = h.finish();
  EXPECT_EQ(hex_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), 32)),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Hash, Sha256OfAFile) {
  auto dir = std::filesystem::path(GOOBER_TEST_TMP) / "base-sha-file";
  std::filesystem::create_directories(dir);
  ASSERT_TRUE(write_file_durable(dir / "a", std::string(200000, 'a')));
  EXPECT_EQ(sha256_file_hex(dir / "a"), sha256_hex(std::string(200000, 'a')));
  EXPECT_FALSE(sha256_file_hex(dir / "missing"));
}

// RFC 4231 test cases 1, 2, and 6 (a key longer than a block).
TEST(Hash, HmacSha256KnownVectors) {
  EXPECT_EQ(hmac_sha256_hex(std::string(20, '\x0b'), "Hi There"),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  EXPECT_EQ(hmac_sha256_hex("Jefe", "what do ya want for nothing?"),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  EXPECT_EQ(hmac_sha256_hex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First"),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(Hash, EqualConstantTime) {
  EXPECT_TRUE(equal_constant_time("abc", "abc"));
  EXPECT_FALSE(equal_constant_time("abc", "abd"));
  EXPECT_FALSE(equal_constant_time("abc", "ab"));
  EXPECT_TRUE(equal_constant_time("", ""));
}

// RFC 8439 §2.4.2: the sunscreen text under key 00..1f, nonce 0:0:4a:0, counter 1.
TEST(ChaCha20, Rfc8439Vector) {
  std::array<uint8_t, 32> key{};
  for (int i = 0; i < 32; ++i) key[i] = uint8_t(i);
  std::array<uint8_t, 12> nonce{0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0};
  std::string text =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
  std::string expected =
      "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c"
      "359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d";
  std::string whole = text;
  ChaCha20(key, nonce, 1).apply(reinterpret_cast<uint8_t*>(whole.data()), whole.size());
  EXPECT_EQ(hex_encode(whole), expected);
  // In pieces that split blocks anywhere, the same stream.
  std::string pieces = text;
  ChaCha20 c(key, nonce, 1);
  c.apply(reinterpret_cast<uint8_t*>(pieces.data()), 1);
  c.apply(reinterpret_cast<uint8_t*>(pieces.data()) + 1, 70);
  c.apply(reinterpret_cast<uint8_t*>(pieces.data()) + 71, pieces.size() - 71);
  EXPECT_EQ(pieces, whole);
  // And back.
  ChaCha20(key, nonce, 1).apply(reinterpret_cast<uint8_t*>(pieces.data()), pieces.size());
  EXPECT_EQ(pieces, text);
}

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

TEST(Files, LogsRotateAndKeepThreeOld) {
  auto dir = std::filesystem::path(GOOBER_TEST_TMP) / "rotate";
  std::filesystem::remove_all(dir);
  auto log = dir / "x.log";
  for (int i = 0; i < 50; ++i) ASSERT_TRUE(append_line(log, std::string(9, char('a' + i % 26)), 100));
  EXPECT_LE(std::filesystem::file_size(log), 100u);
  EXPECT_TRUE(std::filesystem::exists(dir / "x.log.1"));
  EXPECT_TRUE(std::filesystem::exists(dir / "x.log.3"));
  EXPECT_FALSE(std::filesystem::exists(dir / "x.log.4"));
}

TEST(Process, AChildInheritsOnlyItsStandardStreams) {
  // Descriptors the parent holds open without close-on-exec, like a listening socket.
  int a = ::open("/dev/null", O_RDONLY), b = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(b, 4);
  auto r = goober::base::run_process({"ls", "/proc/self/fd"}, "", 4096, 5000);
  ::close(a);
  ::close(b);
  ASSERT_TRUE(r.started);
  // 0, 1, and 2, and the one ls opens to list the directory: never the parent's.
  std::istringstream in(r.out);
  int fd = 0, count = 0;
  while (in >> fd) {
    EXPECT_LE(fd, 3) << r.out;
    ++count;
  }
  EXPECT_LE(count, 4) << r.out;
}
