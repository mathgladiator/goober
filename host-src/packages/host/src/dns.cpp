#include "dns.h"

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <resolv.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace goober::host::dns {
namespace {

constexpr int kTypeMx = 15, kTypeTxt = 16;

struct Reader {
  const std::vector<uint8_t>& m;
  std::string* error;
  bool fail(const char* why) {
    *error = std::string("malformed DNS answer: ") + why;
    return false;
  }
  bool u16(size_t at, int* out) {
    if (at + 2 > m.size()) return fail("cut short");
    *out = (m[at] << 8) | m[at + 1];
    return true;
  }
  // A name starting at `at`: its text, and where the bytes after it (in place) begin.
  bool name(size_t at, std::string* out, size_t* after) {
    out->clear();
    bool jumped = false;
    int jumps = 0;
    for (;;) {
      if (at >= m.size()) return fail("a name runs past the end");
      uint8_t len = m[at];
      if ((len & 0xC0) == 0xC0) {
        if (at + 1 >= m.size()) return fail("a pointer runs past the end");
        if (++jumps > 32) return fail("a loop of name pointers");
        if (!jumped) *after = at + 2;
        jumped = true;
        at = size_t((len & 0x3F) << 8 | m[at + 1]);
        continue;
      }
      if (len & 0xC0) return fail("an unknown label type");
      if (len == 0) {
        if (!jumped) *after = at + 1;
        if (!out->empty() && out->back() == '.') out->pop_back();
        return true;
      }
      if (at + 1 + len > m.size()) return fail("a label runs past the end");
      for (size_t i = 0; i < len; ++i) out->push_back(char(std::tolower(m[at + 1 + i])));
      out->push_back('.');
      if (out->size() > 255) return fail("a name longer than 255");
      at += 1 + len;
    }
  }
};

// Calls `each(rdata_offset, rdlength)` for every answer of `type`.
template <typename F>
bool answers(const std::vector<uint8_t>& m, int type, std::string* error, F each) {
  Reader r{m, error};
  if (m.size() < 12) return r.fail("shorter than a header");
  int qd = 0, an = 0;
  if (!r.u16(4, &qd) || !r.u16(6, &an)) return false;
  size_t at = 12;
  std::string skip;
  for (int i = 0; i < qd; ++i) {
    if (!r.name(at, &skip, &at)) return false;
    at += 4;
  }
  for (int i = 0; i < an; ++i) {
    if (!r.name(at, &skip, &at)) return false;
    int rtype = 0, rdlen = 0;
    if (!r.u16(at, &rtype) || !r.u16(at + 8, &rdlen)) return false;
    size_t rdata = at + 10;
    if (rdata + size_t(rdlen) > m.size()) return r.fail("record data runs past the end");
    if (rtype == type && !each(r, rdata, size_t(rdlen))) return false;
    at = rdata + size_t(rdlen);
  }
  return true;
}

std::optional<std::vector<uint8_t>> query(const std::string& name, int type, bool* none, std::string* error) {
  *none = false;
  struct __res_state st;
  std::memset(&st, 0, sizeof st);
  if (res_ninit(&st) != 0) return *error = "the resolver wouldn't start", std::nullopt;
  st.retrans = 2;
  st.retry = 2;
  std::vector<uint8_t> buf(65535);
  int n = res_nquery(&st, name.c_str(), ns_c_in, type, buf.data(), int(buf.size()));
  int herr = st.res_h_errno;
  res_nclose(&st);
  if (n < 0) {
    if (herr == HOST_NOT_FOUND || herr == NO_DATA) {
      *none = true;
      return std::vector<uint8_t>{};
    }
    return *error = "the lookup of " + name + " failed" + (herr == TRY_AGAIN ? " (try again)" : ""), std::nullopt;
  }
  buf.resize(size_t(n));
  return buf;
}

}  // namespace

std::optional<std::vector<Mx>> parse_mx(const std::vector<uint8_t>& message, std::string* error) {
  std::vector<Mx> out;
  bool ok = answers(message, kTypeMx, error, [&](Reader& r, size_t at, size_t len) {
    if (len < 3) return r.fail("an MX record too short");
    Mx mx;
    size_t after = 0;
    if (!r.u16(at, &mx.preference) || !r.name(at + 2, &mx.host, &after)) return false;
    out.push_back(std::move(mx));
    return true;
  });
  if (!ok) return std::nullopt;
  std::stable_sort(out.begin(), out.end(), [](const Mx& a, const Mx& b) { return a.preference < b.preference; });
  return out;
}

std::optional<std::vector<std::string>> parse_txt(const std::vector<uint8_t>& message, std::string* error) {
  std::vector<std::string> out;
  bool ok = answers(message, kTypeTxt, error, [&](Reader& r, size_t at, size_t len) {
    std::string text;
    size_t end = at + len;
    while (at < end) {
      size_t n = message[at];
      if (at + 1 + n > end) return r.fail("a TXT string runs past its record");
      text.append(reinterpret_cast<const char*>(&message[at + 1]), n);
      at += 1 + n;
    }
    out.push_back(std::move(text));
    return true;
  });
  if (!ok) return std::nullopt;
  return out;
}

std::optional<std::vector<Mx>> lookup_mx(const std::string& name, std::string* error) {
  bool none = false;
  auto m = query(name, kTypeMx, &none, error);
  if (!m) return std::nullopt;
  if (none) return std::vector<Mx>{};
  return parse_mx(*m, error);
}

std::optional<std::vector<std::string>> lookup_txt(const std::string& name, std::string* error) {
  bool none = false;
  auto m = query(name, kTypeTxt, &none, error);
  if (!m) return std::nullopt;
  if (none) return std::vector<std::string>{};
  return parse_txt(*m, error);
}

std::optional<std::string> smtp_banner(const std::string& address, uint16_t port, int timeout_ms, std::string* error) {
  sockaddr_storage ss{};
  socklen_t len = 0;
  if (address.find(':') == std::string::npos) {
    auto* a = reinterpret_cast<sockaddr_in*>(&ss);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &a->sin_addr) != 1) return *error = "not an address", std::nullopt;
    len = sizeof *a;
  } else {
    auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(port);
    if (inet_pton(AF_INET6, address.c_str(), &a->sin6_addr) != 1) return *error = "not an address", std::nullopt;
    len = sizeof *a;
  }
  int fd = socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return *error = "no socket", std::nullopt;
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } closer{fd};
  if (connect(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0 && errno != EINPROGRESS) {
    return *error = std::string("connect: ") + std::strerror(errno), std::nullopt;
  }
  pollfd p{fd, POLLOUT, 0};
  if (poll(&p, 1, timeout_ms) != 1) return *error = "no answer within " + std::to_string(timeout_ms) + " ms", std::nullopt;
  int soerr = 0;
  socklen_t sl = sizeof soerr;
  getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
  if (soerr) return *error = std::string("connect: ") + std::strerror(soerr), std::nullopt;
  std::string line;
  char buf[512];
  while (line.find('\n') == std::string::npos && line.size() < 512) {
    p = {fd, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) != 1) return *error = "connected, but no banner within " + std::to_string(timeout_ms) + " ms", std::nullopt;
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0) return *error = "connected, but closed before a banner", std::nullopt;
    line.append(buf, size_t(n));
  }
  (void)!::write(fd, "QUIT\r\n", 6);
  line = line.substr(0, line.find_first_of("\r\n"));
  for (auto& c : line) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
  }
  return line.substr(0, 200);
}

}  // namespace goober::host::dns
