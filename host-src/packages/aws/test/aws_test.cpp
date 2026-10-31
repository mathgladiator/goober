#include <gtest/gtest.h>

#include <filesystem>

#include "fake_aws.h"
#include "goober/aws/aws.h"
#include "goober/base/encoding.h"
#include "goober/base/files.h"
#include "goober/base/process.h"

namespace goober::aws {
namespace {

namespace fs = std::filesystem;

std::string hex(const std::array<uint8_t, 32>& bytes) {
  return base::hex_encode(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

fs::path scratch(const std::string& name) {
  auto dir = fs::path(GOOBER_TEST_TMP) / ("aws-" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::string signature_of(const Pairs& headers) {
  const std::string& auth = headers.at(0).second;
  return auth.substr(auth.find("Signature=") + 10);
}

// AWS's documented example of deriving a signing key.
TEST(SigV4, SigningKeyFromTheDocumentation) {
  EXPECT_EQ(hex(signing_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "20120215", "us-east-1", "iam")),
            "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d");
}

// get-vanilla from the AWS SigV4 test suite.
TEST(SigV4, GetVanilla) {
  SignInput in;
  in.method = "GET";
  in.host = "example.amazonaws.com";
  in.path = "/";
  in.payload_sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  in.content_sha256_header = false;
  Credentials c{"AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", ""};
  auto headers = sign(in, c, "us-east-1", "service", "20150830T123600Z");
  EXPECT_EQ(headers[0].second,
            "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, "
            "SignedHeaders=host;x-amz-date, Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");
}

// The IAM ListUsers example from the SigV4 documentation: a query, and a signed content-type.
TEST(SigV4, IamListUsers) {
  SignInput in;
  in.method = "GET";
  in.host = "iam.amazonaws.com";
  in.path = "/";
  in.query = {{"Version", "2010-05-08"}, {"Action", "ListUsers"}};
  in.headers = {{"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"}};
  in.payload_sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  in.content_sha256_header = false;
  Credentials c{"AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", ""};
  EXPECT_EQ(canonical_request(in, "20150830T123600Z", c),
            "GET\n/\nAction=ListUsers&Version=2010-05-08\ncontent-type:application/x-www-form-urlencoded; "
            "charset=utf-8\nhost:iam.amazonaws.com\nx-amz-date:20150830T123600Z\n\ncontent-type;host;x-amz-date\n"
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(signature_of(sign(in, c, "us-east-1", "iam", "20150830T123600Z")),
            "5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7");
}

// The S3 GET Object example from the S3 SigV4 documentation, with a Range header.
TEST(SigV4, S3GetObject) {
  SignInput in;
  in.method = "GET";
  in.host = "examplebucket.s3.amazonaws.com";
  in.path = "/test.txt";
  in.headers = {{"range", "bytes=0-9"}};
  in.payload_sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  Credentials c{"AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", ""};
  EXPECT_EQ(signature_of(sign(in, c, "us-east-1", "s3", "20130524T000000Z")),
            "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

TEST(SigV4, UriEncoding) {
  EXPECT_EQ(uri_encode("a b/c~d_e.f-g+h", false), "a%20b/c~d_e.f-g%2Bh");
  EXPECT_EQ(uri_encode("a/b", true), "a%2Fb");
  EXPECT_EQ(uri_encode("\xc3\xa9", true), "%C3%A9");
  EXPECT_EQ(amz_date(1440938160000), "20150830T123600Z");
}

const Credentials kCreds{"AKIDTEST", "secret/with+chars", ""};

// curl signs SigV4 itself (--aws-sigv4): the stand-in, checking with our signer, accepts what
// curl signs, so the two agree on canonical requests, an independent check on ours.
TEST(SigV4, AgreesWithCurl) {
  testing::FakeAws fake(kCreds);
  auto dir = scratch("curl");
  ASSERT_TRUE(base::write_file_durable(dir / "body", "hello from curl"));
  auto r = base::run_process({"curl", "-q", "-sS", "-o", "/dev/null", "-w", "%{http_code}", "--aws-sigv4",
                              "aws:amz:us-east-1:s3", "--user", kCreds.access_key_id + ":" + kCreds.secret_access_key,
                              "-H", "x-amz-content-sha256: " + base::sha256_hex("hello from curl"), "--upload-file",
                              (dir / "body").string(), fake.endpoint() + "/bucket/some%20dir/file.txt?x=1&a=b%2Fc"},
                             "", 1024, 20000);
  ASSERT_TRUE(r.started);
  EXPECT_EQ(r.out, "200") << r.err;
  EXPECT_EQ(fake.refused_signatures(), 0);
  EXPECT_EQ(fake.objects("bucket")["some dir/file.txt"], "hello from curl");
}

TEST(S3, PutGetListDelete) {
  testing::FakeAws fake(kCreds);
  auto dir = scratch("s3");
  S3 s3({"bucket", "us-east-1", fake.endpoint(), "goober/"}, [] { return std::optional(kCreds); }, dir / "tmp");
  std::string err;
  std::string big(300000, 'x');
  for (size_t i = 0; i < big.size(); i += 7) big[i] = char(i);
  ASSERT_TRUE(base::write_file_durable(dir / "big", big));
  ASSERT_TRUE(s3.put_file("nightly/2026-10-27T03-00-00Z.tar.gz", dir / "big", &err)) << err;
  ASSERT_TRUE(s3.put("assets/playground/1.png", "png bytes", &err)) << err;
  ASSERT_TRUE(s3.put("odd key/é+ü", "odd", &err)) << err;
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(s3.put("db/" + std::to_string(i), "d", &err)) << err;
  EXPECT_EQ(fake.objects("bucket")["goober/nightly/2026-10-27T03-00-00Z.tar.gz"], big);
  EXPECT_EQ(fake.objects("bucket")["goober/odd key/\xc3\xa9+\xc3\xbc"], "odd");

  EXPECT_EQ(s3.get("assets/playground/1.png", &err), "png bytes");
  ASSERT_TRUE(s3.get_file("nightly/2026-10-27T03-00-00Z.tar.gz", dir / "back/big", &err)) << err;
  EXPECT_EQ(base::read_file(dir / "back/big"), big);
  EXPECT_FALSE(s3.get("missing", &err));
  EXPECT_NE(err.find("NoSuchKey"), std::string::npos) << err;
  EXPECT_FALSE(s3.get_file("missing", dir / "back/missing", &err));
  EXPECT_FALSE(fs::exists(dir / "back/missing"));

  // Pages of three: eight objects take three pages.
  auto all = s3.list("", &err);
  ASSERT_TRUE(all) << err;
  EXPECT_EQ(all->size(), 8u);
  auto db = s3.list("db/", &err);
  ASSERT_TRUE(db) << err;
  ASSERT_EQ(db->size(), 5u);
  EXPECT_EQ((*db)[0].key, "db/0");
  EXPECT_EQ((*db)[0].size, 1);
  auto odd = s3.list("odd", &err);
  ASSERT_TRUE(odd && odd->size() == 1);
  EXPECT_EQ((*odd)[0].key, "odd key/\xc3\xa9+\xc3\xbc");

  ASSERT_TRUE(s3.remove("db/0", &err)) << err;
  EXPECT_EQ(s3.list("db/", &err)->size(), 4u);
  EXPECT_EQ(fake.refused_signatures(), 0);
  // No secret is left behind in the scratch directory.
  EXPECT_TRUE(base::list_files(dir / "tmp").empty());
}

TEST(S3, FailuresSayWhy) {
  testing::FakeAws fake(kCreds);
  auto dir = scratch("s3-fail");
  std::string err;
  S3 wrong({"bucket", "us-east-1", fake.endpoint(), ""}, [] { return std::optional(Credentials{"AKIDTEST", "nope", ""}); },
           dir);
  EXPECT_FALSE(wrong.put("a", "b", &err));
  EXPECT_NE(err.find("SignatureDoesNotMatch"), std::string::npos) << err;
  S3 none({"bucket", "us-east-1", fake.endpoint(), ""}, [] { return std::optional<Credentials>(); }, dir);
  EXPECT_FALSE(none.put("a", "b", &err));
  EXPECT_NE(err.find("no AWS credentials"), std::string::npos) << err;
  S3 s3({"bucket", "us-east-1", fake.endpoint(), ""}, [] { return std::optional(kCreds); }, dir);
  fake.fail_next(1, 503);
  EXPECT_FALSE(s3.put("a", "b", &err));
  EXPECT_NE(err.find("503"), std::string::npos) << err;
  EXPECT_TRUE(s3.put("a", "b", &err)) << err;
  // Plain http goes only to this machine.
  S3 remote({"bucket", "us-east-1", "http://example.com", ""}, [] { return std::optional(kCreds); }, dir);
  EXPECT_FALSE(remote.put("a", "b", &err));
  EXPECT_NE(err.find("https"), std::string::npos) << err;
}

TEST(Ses, SendsRawMail) {
  testing::FakeAws fake(kCreds);
  auto dir = scratch("ses");
  SesMessage m{"no-reply@example.com", "person@example.org", "Subject: hi\r\n\r\nhello", ""};
  auto r = ses_send(m, kCreds, "us-east-1", dir, fake.endpoint());
  EXPECT_TRUE(r.sent) << r.error;
  EXPECT_EQ(r.message_id, "fake-1");
  ASSERT_EQ(fake.mails().size(), 1u);
  EXPECT_EQ(fake.mails()[0], m.raw);
  fake.fail_next(1, 400);
  r = ses_send(m, kCreds, "us-east-1", dir, fake.endpoint());
  EXPECT_FALSE(r.sent);
  EXPECT_TRUE(r.permanent);
  fake.fail_next(1, 429);
  r = ses_send(m, kCreds, "us-east-1", dir, fake.endpoint());
  EXPECT_FALSE(r.sent);
  EXPECT_FALSE(r.permanent);
  EXPECT_EQ(fake.refused_signatures(), 0);
}

TEST(Ses, ReadsAnIdentitysVerificationAndDkim) {
  testing::FakeAws fake(kCreds);
  auto dir = scratch("ses-identity");
  auto id = ses_identity("example.com", kCreds, "us-east-1", dir, fake.endpoint());
  ASSERT_TRUE(id.ok) << id.error;
  EXPECT_TRUE(id.exists);
  EXPECT_TRUE(id.verified_for_sending);
  EXPECT_EQ(id.dkim_status, "SUCCESS");
  EXPECT_EQ(id.dkim_tokens, (std::vector<std::string>{"tok1", "tok2", "tok3"}));
  auto none = ses_identity("other.example", kCreds, "us-east-1", dir, fake.endpoint());
  EXPECT_TRUE(none.ok) << none.error;
  EXPECT_FALSE(none.exists);
  fake.fail_next(1, 503);
  auto down = ses_identity("example.com", kCreds, "us-east-1", dir, fake.endpoint());
  EXPECT_FALSE(down.ok);
  EXPECT_FALSE(down.error.empty());
  EXPECT_EQ(fake.refused_signatures(), 0);
}

}  // namespace
}  // namespace goober::aws
