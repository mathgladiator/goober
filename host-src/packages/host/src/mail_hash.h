#pragma once

// A hash for remembering who wrote and which message (mail_in.cpp's received table), so
// neither an address nor a Message-ID is kept: lowercased, without <> or spaces.

#include <string>
#include <string_view>

#include "goober/base/encoding.h"
#include "goober/base/hash.h"

namespace goober::host {

inline std::string received_hash(std::string_view v) {
  std::string s;
  for (char c : v) {
    if (c != '<' && c != '>' && c != ' ' && c != '\t') s += c;
  }
  return base::sha256_hex("goober-received\n" + base::to_lower(s)).substr(0, 32);
}

}  // namespace goober::host
